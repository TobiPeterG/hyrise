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

void SieveEviction::_advance_hand_locked() {
  if (_eviction_vector.empty()) {
    _hand.store(0, std::memory_order_relaxed);
    return;
  }
  const auto cur = _hand.load(std::memory_order_relaxed) % _eviction_vector.size();
  const auto next = prev_index_or_wrap(cur, _eviction_vector.size());
  _hand.store(next, std::memory_order_relaxed);
}

void SieveEviction::_erase_at_and_advance_locked(const std::size_t idx) {
  DebugAssert(!_eviction_vector.empty(), "erase requires non-empty vector");
  DebugAssert(idx < _eviction_vector.size(), "erase index out of bounds");

  _eviction_vector.erase(_eviction_vector.begin() + idx);

  if (_eviction_vector.empty()) {
    _hand.store(0, std::memory_order_relaxed);
    return;
  }

  // Continue scanning in the same direction (toward tail), i.e., move to previous index.
  const auto next = prev_index_or_wrap(idx % _eviction_vector.size(), _eviction_vector.size());
  _hand.store(next, std::memory_order_relaxed);
}

void SieveEviction::purge_item(std::shared_lock<std::shared_mutex>& shared_lock, const std::size_t this_hand) {
  // We observed this candidate under shared lock. To erase, we need unique.
  shared_lock.unlock();
  std::unique_lock unique_lock{_mutex};

  if (_eviction_vector.empty()) {
    return;
  }

  // Only purge if the hand still points to the same logical position.
  const auto cur = _hand.load(std::memory_order_relaxed) % _eviction_vector.size();
  if (cur != this_hand) {
    return;
  }

  _erase_at_and_advance_locked(this_hand);
  increment_counter(_buffer_pool.metrics->num_eviction_queue_items_purged);
}

bool SieveEviction::perform_evictions(const PageSizeType required_size) {
  const auto bytes_required = bytes_for_size_type(required_size);

  _buffer_pool.reserve_bytes(bytes_required);

  // We must ensure progress: if everything is pinned / constantly referenced / lock-failing,
  // do a bounded scan and fail (rolling back the reservation) rather than spinning forever.
  bool counted_episode = false;
  while (_buffer_pool.used_bytes.load(std::memory_order_relaxed) > _buffer_pool.max_bytes) {
    if (!counted_episode) {
      increment_counter(_buffer_pool.metrics->num_oversubscription_episodes);
      counted_episode = true;
    }

    // Bound the number of "inspections" per oversubscription episode.
    // 2x is usually enough to clear references once and come back around.
    std::size_t max_scans = 0;
    {
      std::shared_lock shared_lock{_mutex};
      if (_eviction_vector.empty()) {
        increment_counter(_buffer_pool.metrics->num_eviction_failures);
        _buffer_pool.free_bytes(bytes_required);
        return false;
      }
      max_scans = _eviction_vector.size() * 2 + 1;
    }

    for (std::size_t scans = 0; scans < max_scans &&
                                _buffer_pool.used_bytes.load(std::memory_order_relaxed) > _buffer_pool.max_bytes;
         ++scans) {
      std::shared_lock shared_lock{_mutex};

      if (_eviction_vector.empty()) {
        increment_counter(_buffer_pool.metrics->num_eviction_failures);
        _buffer_pool.free_bytes(bytes_required);
        return false;
      }

      // SIEVE: end is head (new), begin is tail (old).
      // Our hand moves from head toward tail by decrementing index with wrap.
      const auto this_hand = _hand.load(std::memory_order_relaxed) % _eviction_vector.size();
      const auto item = _eviction_vector[this_hand];

      increment_counter(_buffer_pool.metrics->num_eviction_candidate_inspections);

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

      // Not evictable now (pinned / locked) -> advance hand under unique lock
      if (Frame::state(current_state_and_version) != Frame::UNLOCKED) {
        increment_counter(_buffer_pool.metrics->num_eviction_requeues_pinned);
        shared_lock.unlock();
        std::unique_lock unique_lock{_mutex};
        _advance_hand_locked();
        continue;
      }

      // SIEVE visited/reference bit: clear on first visit, keep item in place, advance hand.
      if (Frame::is_referenced(current_state_and_version)) {
        increment_counter(_buffer_pool.metrics->num_eviction_requeues_referenced);
        frame->clear_reference();
        shared_lock.unlock();
        std::unique_lock unique_lock{_mutex};
        _advance_hand_locked();
        continue;
      }

      // Try locking the frame exclusively; if it fails, advance hand and retry.
      if (!frame->try_lock_exclusive(current_state_and_version)) {
        increment_counter(_buffer_pool.metrics->num_eviction_requeues_lock_failed);
        shared_lock.unlock();
        std::unique_lock unique_lock{_mutex};
        _advance_hand_locked();
        continue;
      }

      Assert(frame->node_id() == _buffer_pool.node_id,
             "Memory node mismatch: " + std::to_string(frame->node_id()) + " != " + std::to_string(_buffer_pool.node_id));

      // We have the victim frame exclusively locked.
      // Now remove the victim from the vector under unique lock, but only if hand still matches.
      shared_lock.unlock();
      std::unique_lock unique_lock{_mutex};

      if (_eviction_vector.empty()) {
        frame->unlock_exclusive();
        continue;
      }

      const auto hand_now = _hand.load(std::memory_order_relaxed) % _eviction_vector.size();
      if (hand_now != this_hand) {
        frame->unlock_exclusive();
        continue;
      }

      // Erase victim and advance hand (still under lock)
      _erase_at_and_advance_locked(this_hand);

      // Evict while still holding the frame exclusively locked (vector lock released)
      auto evict_item = item;
      unique_lock.unlock();

      _buffer_pool.evict(evict_item, frame);
      increment_counter(_buffer_pool.metrics->num_evictions);

      // Free budget immediately (we reserved bytes_required up-front; evict frees actual bytes).
      _buffer_pool.free_bytes(bytes_for_size_type(evict_item.page_id.size_type()));
    }

    // If we're still oversubscribed after a bounded scan, abort
    if (_buffer_pool.used_bytes.load(std::memory_order_relaxed) > _buffer_pool.max_bytes) {
      increment_counter(_buffer_pool.metrics->num_eviction_failures);
      _buffer_pool.free_bytes(bytes_required);
      return false;
    }
  }

  return true;
}

void SieveEviction::on_access(const PageID& /*page_id*/, Frame* const frame) {
  // visited/reference bit that is cleared upon scan.
  // On access, we set it.
  frame->set_reference_level(1);
}

void SieveEviction::add_eviction_candidate(const PageID& page_id, Frame* const frame) {
  const auto current_state_and_version = frame->state_and_version();
  DebugAssert(frame->node_id() == _buffer_pool.node_id, "Memory node mismatch");
  increment_counter(_buffer_pool.metrics->num_eviction_queue_adds);

  std::unique_lock lock{_mutex};

  const auto was_empty = _eviction_vector.empty();
  _eviction_vector.push_back({page_id, Frame::version(current_state_and_version)});

  // New objects at head (end). When list becomes non-empty, start hand at head.
  if (was_empty) {
    _hand.store(_eviction_vector.size() - 1, std::memory_order_relaxed);
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

    // Purge stale or migrated entries (the only ones that can never become valid again).
    if (frame->node_id() != _buffer_pool.node_id || Frame::version(current_state_and_version) != item.timestamp) {
      purge_item(shared_lock, this_hand);
      --i;
      continue;
    }

    // Keep pinned and referenced frames; SIEVE handles referenced via scan/clear in perform_evictions().
  }
}

std::size_t SieveEviction::memory_consumption() const {
  return sizeof(*this) + sizeof(_eviction_vector) + sizeof(EvictionItem) * _eviction_vector.size();
}

}  // namespace hyrise
