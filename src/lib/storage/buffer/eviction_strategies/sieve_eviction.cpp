#include "sieve_eviction.hpp"

#include <storage/buffer/buffer_pool.hpp>

#include "storage/buffer/eviction_strategy_registry.hpp"

namespace hyrise {

namespace {
EvictionStrategyRegistrar s_sieve_registrar{
    "sieve", [](BufferPool& buffer_pool) { return std::make_unique<SieveEviction>(buffer_pool); }, {"SIEVE"}};
}  // namespace

SieveEviction::SieveEviction(BufferPool& buffer_pool) : EvictionStrategy(buffer_pool), _hand(0) {}

namespace {

std::size_t prev_index_or_wrap(const std::size_t idx, const std::size_t size) {
  DebugAssert(size > 0, "prev_index_or_wrap requires non-empty container");
  return (idx == 0) ? (size - 1) : (idx - 1);
}

}  // namespace

void SieveEviction::purge_item(std::shared_lock<std::shared_mutex>& shared_lock, const std::size_t this_hand) {
  shared_lock.unlock();
  std::unique_lock unique_lock{_mutex};

  // Check if other thread has already processed this item
  if (_hand.load(std::memory_order_relaxed) != this_hand) {
    return;
  }

  if (_eviction_vector.empty()) {
    return;
  }

  _eviction_vector.erase(_eviction_vector.begin() + this_hand);

  if (_eviction_vector.empty()) {
    _hand.store(0, std::memory_order_relaxed);
  } else {
    // Hand moves from head (end) toward tail (begin). After erasing at this_hand, keep moving in the same direction.
    const auto next_hand = prev_index_or_wrap(this_hand, _eviction_vector.size());
    _hand.store(next_hand, std::memory_order_relaxed);
  }

  increment_counter(_buffer_pool.metrics->num_eviction_queue_items_purged);
}

bool SieveEviction::perform_evictions(const PageSizeType required_size) {
  const auto bytes_required = bytes_for_size_type(required_size);

  _buffer_pool.reserve_bytes(bytes_required);

  while (_buffer_pool.used_bytes.load(std::memory_order_relaxed) > _buffer_pool.max_bytes) {
    std::shared_lock shared_lock{_mutex};

    if (_eviction_vector.empty()) {
      _buffer_pool.free_bytes(bytes_required);
      return false;
    }

    // SIEVE: treat end of vector as "head" (new objects), begin as "tail" (old objects).
    // The eviction hand starts at the head and moves toward the tail, which quickly demotes new objects.
    const auto this_hand = _hand.load(std::memory_order_relaxed) % _eviction_vector.size();
    const auto item = _eviction_vector[this_hand];

    const auto region = _buffer_pool.volatile_regions[static_cast<uint64_t>(item.page_id.size_type())];
    auto frame = region->get_frame(item.page_id);
    const auto current_state_and_version = frame->state_and_version();

    // Migrated entry -> purge
    if (frame->node_id() != _buffer_pool.node_id) {
      purge_item(shared_lock, this_hand);
      continue;
    }

    // Stale entry (frame generation changed) -> purge
    if (Frame::version(current_state_and_version) != item.timestamp) {
      purge_item(shared_lock, this_hand);
      continue;
    }

    // Keep pinned frames in the list, but skip them (do not purge).
    if (Frame::state(current_state_and_version) != Frame::UNLOCKED) {
      const auto next_hand = prev_index_or_wrap(this_hand, _eviction_vector.size());
      _hand.store(next_hand, std::memory_order_relaxed);
      continue;
    }

    // SIEVE "visited" bit:
    // - If referenced/visited, clear and keep the object in place.
    // - Otherwise evict.
    if (Frame::is_referenced(current_state_and_version)) {
      frame->clear_reference();
      const auto next_hand = prev_index_or_wrap(this_hand, _eviction_vector.size());
      _hand.store(next_hand, std::memory_order_relaxed);
      continue;
    }

    // Try locking the frame exclusively
    if (!frame->try_lock_exclusive(current_state_and_version)) {
      const auto next_hand = prev_index_or_wrap(this_hand, _eviction_vector.size());
      _hand.store(next_hand, std::memory_order_relaxed);
      continue;
    }

    Assert(frame->node_id() == _buffer_pool.node_id,
           "Memory node mismatch: " + std::to_string(frame->node_id()) + " != " + std::to_string(_buffer_pool.node_id));

    // Remove the item under unique lock, then evict.
    shared_lock.unlock();
    std::unique_lock unique_lock{_mutex};

    if (_eviction_vector.empty()) {
      frame->unlock_exclusive();
      continue;
    }

    // If the hand moved, drop the lock and retry
    const auto hand_now = _hand.load(std::memory_order_relaxed) % _eviction_vector.size();
    if (hand_now != this_hand) {
      frame->unlock_exclusive();
      continue;
    }

    // Erase victim from vector
    _eviction_vector.erase(_eviction_vector.begin() + this_hand);

    if (_eviction_vector.empty()) {
      _hand.store(0, std::memory_order_relaxed);
    } else {
      // Continue scanning from the previous position (toward tail)
      const auto next_hand = prev_index_or_wrap(this_hand, _eviction_vector.size());
      _hand.store(next_hand, std::memory_order_relaxed);
    }

    // Evict (still holding the frame exclusively locked)
    auto evict_item = item;
    unique_lock.unlock();

    _buffer_pool.evict(evict_item, frame);
    increment_counter(_buffer_pool.metrics->num_evictions);

    // free budget immediately
    _buffer_pool.free_bytes(bytes_for_size_type(evict_item.page_id.size_type()));
  }

  return true;
}

void SieveEviction::add_eviction_candidate(const PageID& page_id, Frame* const frame) {
  const auto current_state_and_version = frame->state_and_version();
  DebugAssert(frame->node_id() == _buffer_pool.node_id, "Memory node mismatch");
  increment_counter(_buffer_pool.metrics->num_eviction_queue_adds);

  {
    std::unique_lock lock{_mutex};

    const auto was_empty = _eviction_vector.empty();
    _eviction_vector.push_back({page_id, Frame::version(current_state_and_version)});

    // SIEVE: new objects are added at the head (end). To achieve quick demotion,
    // the hand should start at the head (newest) when the list transitions from empty->non-empty.
    if (was_empty) {
      _hand.store(_eviction_vector.size() - 1, std::memory_order_relaxed);
    }
  }

  // I'm using the end of the vector as what the SIEVE paper calls the head. This is because I believe adding elements
  // in the beginning would always cause moving all other values (at least in the usual std:: implementation of a
  // vector).
}

void SieveEviction::purge_eviction_candidates() {
  for (auto i = std::size_t{0}; i < MAX_EVICTION_QUEUE_PURGES; ++i) {
    std::shared_lock shared_lock{_mutex};
    if (_eviction_vector.empty()) {
      return;
    }

    const auto this_hand = _hand.load(std::memory_order_relaxed) % _eviction_vector.size();
    const auto item = _eviction_vector[this_hand];

    const auto region = _buffer_pool.volatile_regions[static_cast<uint64_t>(item.page_id.size_type())];
    auto frame = region->get_frame(item.page_id);
    const auto current_state_and_version = frame->state_and_version();

    // Purge stale or migrated entries.
    if (frame->node_id() != _buffer_pool.node_id || Frame::version(current_state_and_version) != item.timestamp) {
      purge_item(shared_lock, this_hand);
      --i;
      continue;
    }

    // Keep pinned frames; only purge entries that can never become valid again (stale/migrated).
  }
}

std::size_t SieveEviction::memory_consumption() const {
  return sizeof(*this) + sizeof(_eviction_vector) +
         sizeof(tbb::concurrent_vector<EvictionQueue>::value_type) * _eviction_vector.size();
}

}  // namespace hyrise
