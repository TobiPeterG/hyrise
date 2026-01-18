#include "sieve_eviction.hpp"

#include <storage/buffer/buffer_pool.hpp>

namespace hyrise {

SieveEviction::SieveEviction(BufferPool& buffer_pool) : EvictionStrategy(buffer_pool), _hand(0) {}

void SieveEviction::purge_item(std::shared_lock<std::shared_mutex>& shared_lock, const std::size_t this_hand) {
  shared_lock.unlock();
  std::unique_lock unique_lock{_mutex};

  // Check if other thread has already processed this item
  if (_hand.load(std::memory_order_relaxed) != this_hand) {
    return;
  }

  _eviction_vector.erase(_eviction_vector.begin() + this_hand);
  _hand = (this_hand + 1) % _eviction_vector.size();

  increment_counter(_buffer_pool.metrics->num_eviction_queue_items_purged);
}

bool SieveEviction::perform_evictions(const PageSizeType required_size) {
  const auto bytes_required = bytes_for_size_type(required_size);
  auto freed_bytes = std::size_t{0};

  _buffer_pool.reserve_bytes(bytes_required);

  while (_buffer_pool.used_bytes.load(std::memory_order_relaxed) > _buffer_pool.max_bytes) {
    std::shared_lock shared_lock{_mutex};

    if (_eviction_vector.empty()) {
      _buffer_pool.free_bytes(bytes_required);
      return false;
    }

    const auto this_hand = _hand.load(std::memory_order_relaxed);
    auto item = _eviction_vector[this_hand];

    const auto region = _buffer_pool.volatile_regions[static_cast<uint64_t>(item.page_id.size_type())];
    auto frame = region->get_frame(item.page_id);
    auto current_state_and_version = frame->state_and_version();

    if (frame->node_id() != _buffer_pool.node_id) {
      increment_counter(_buffer_pool.metrics->num_eviction_queue_items_purged);
      continue;
    }

    if (!item.can_evict(current_state_and_version)) {
      if (item.can_mark(current_state_and_version)) {
        if (frame->try_mark(current_state_and_version)) {
          _hand.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
      }

      purge_item(shared_lock, this_hand);
      continue;
    }

    if (!frame->try_lock_exclusive(current_state_and_version)) {
      purge_item(shared_lock, this_hand);
      continue;
    }

    Assert(frame->node_id() == _buffer_pool.node_id,
      "Memory node mismatch: " + std::to_string(frame->node_id()) + " != " + std::to_string(_buffer_pool.node_id));

    _hand.fetch_add(1, std::memory_order_relaxed);

    _buffer_pool.evict(item, frame);

    increment_counter(_buffer_pool.metrics->num_evictions);

    const auto size_type = item.page_id.size_type();
    freed_bytes += bytes_for_size_type(size_type);
  }

  _buffer_pool.free_bytes(freed_bytes);

  return true;
}

void SieveEviction::add_eviction_candidate(const PageID& page_id, Frame* const frame) {
  const auto current_state_and_version = frame->state_and_version();
  DebugAssert(frame->node_id() == _buffer_pool.node_id, "Memory node mismatch");
  increment_counter(_buffer_pool.metrics->num_eviction_queue_adds);
  {
    frame->try_mark(current_state_and_version);
    std::unique_lock lock{_mutex};
    _eviction_vector.push_back({page_id, Frame::version(current_state_and_version)});
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

    const auto this_hand = _hand.load(std::memory_order_relaxed);
    const auto item = _eviction_vector[this_hand];

    const auto region = _buffer_pool.volatile_regions[static_cast<uint64_t>(item.page_id.size_type())];
    auto frame = region->get_frame(item.page_id);
    auto current_state_and_version = frame->state_and_version();

    if (!(item.can_evict(current_state_and_version) || item.can_mark(current_state_and_version))) {
      purge_item(shared_lock, this_hand);
      --i;
    }
  }
}

std::size_t SieveEviction::memory_consumption() const {
  return sizeof(*this) + sizeof(_eviction_vector) + sizeof(tbb::concurrent_vector<EvictionQueue>::value_type) * _eviction_vector.size();
}

}  // namespace hyrise