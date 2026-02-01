#include <limits>

#include "sieve_eviction.hpp"

#include <storage/buffer/buffer_pool.hpp>

#include "storage/buffer/eviction_strategy_registry.hpp"

namespace hyrise {

namespace {
EvictionStrategyRegistrar s_sieve_registrar{
    "sieve", [](BufferPool& buffer_pool) { return std::make_unique<SieveEviction>(buffer_pool); }, {"SIEVE"}};

// Adaptive skip on pinned/lockfail hotspots.
// Start close to canonical SIEVE, increase only on repeated failures at the same hand index.
constexpr std::size_t SIEVE_BASE_SKIP_ON_PINNED = 2;
constexpr std::size_t SIEVE_BASE_SKIP_ON_LOCKFAIL = 2;
constexpr std::size_t SIEVE_SKIP_ON_REFERENCED = 1;  // canonical: advance by one
constexpr std::size_t SIEVE_MAX_SKIP = 16;

// Bounded work, adaptive.
// Start small when contention is low; grow only when we make no progress.
constexpr std::size_t SIEVE_SCAN_MULTIPLIER_MIN = 2;
constexpr std::size_t SIEVE_SCAN_MULTIPLIER_MAX = 8;

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

bool SieveEviction::_advance_hand_to_prev_valid_from_locked(std::size_t start_idx) {
  if (_live_entries == 0 || _eviction_vector.empty()) {
    _hand.store(0, std::memory_order_relaxed);
    return false;
  }

  // Start strictly before start_idx (one step toward tail)
  auto idx = prev_index_or_wrap(start_idx % _eviction_vector.size(), _eviction_vector.size());
  for (std::size_t i = 0; i < _eviction_vector.size(); ++i) {
    if (_eviction_vector[idx].valid) {
      _hand.store(idx, std::memory_order_relaxed);
      return true;
    }
    idx = prev_index_or_wrap(idx, _eviction_vector.size());
  }

  _hand.store(0, std::memory_order_relaxed);
  return false;
}

bool SieveEviction::_advance_hand_to_prev_valid_locked() {
  if (_live_entries == 0 || _eviction_vector.empty()) {
    _hand.store(0, std::memory_order_relaxed);
    return false;
  }

  // Try at most vector.size() steps to find a valid slot.
  auto idx = _hand.load(std::memory_order_relaxed) % _eviction_vector.size();
  for (std::size_t i = 0; i < _eviction_vector.size(); ++i) {
    if (_eviction_vector[idx].valid) {
      _hand.store(idx, std::memory_order_relaxed);
      return true;
    }
    idx = prev_index_or_wrap(idx, _eviction_vector.size());
  }

  // Should not happen if _live_entries > 0
  _hand.store(0, std::memory_order_relaxed);
  return false;
}

bool SieveEviction::_advance_hand_skip_and_find_prev_valid_from_locked(std::size_t start_idx, std::size_t skip_steps) {
  if (_live_entries == 0 || _eviction_vector.empty()) {
    _hand.store(0, std::memory_order_relaxed);
    return false;
  }

  auto idx = start_idx % _eviction_vector.size();
  for (std::size_t s = 0; s < skip_steps; ++s) {
    idx = prev_index_or_wrap(idx, _eviction_vector.size());
  }

  for (std::size_t i = 0; i < _eviction_vector.size(); ++i) {
    if (_eviction_vector[idx].valid) {
      _hand.store(idx, std::memory_order_relaxed);
      return true;
    }
    idx = prev_index_or_wrap(idx, _eviction_vector.size());
  }

  _hand.store(0, std::memory_order_relaxed);
  return false;
}

void SieveEviction::_invalidate_at_locked(const std::size_t idx) {
  DebugAssert(!_eviction_vector.empty(), "invalidate requires non-empty vector");
  DebugAssert(idx < _eviction_vector.size(), "invalidate index out of bounds");

  if (_eviction_vector[idx].valid) {
    _eviction_vector[idx].valid = false;
    DebugAssert(_live_entries > 0, "live entry underflow");
    --_live_entries;
    ++_tombstones;
  }
}

void SieveEviction::_invalidate_at_and_advance_locked(const std::size_t idx) {
  DebugAssert(!_eviction_vector.empty(), "invalidate requires non-empty vector");
  DebugAssert(idx < _eviction_vector.size(), "invalidate index out of bounds");

  _invalidate_at_locked(idx);

  if (_live_entries == 0) {
    _hand.store(0, std::memory_order_relaxed);
    return;
  }

  // Continue scanning in the same direction (toward tail), i.e., move to previous valid.
  _hand.store(prev_index_or_wrap(idx, _eviction_vector.size()), std::memory_order_relaxed);
  (void)_advance_hand_to_prev_valid_locked();
}

void SieveEviction::_compact_if_needed_locked() {
  if (_tombstones == 0)
    return;

  const auto size = _eviction_vector.size();
  if (size == 0)
    return;

  if (!(_tombstones > size / 2 || size > (_live_entries * 4 + 64))) {
    return;
  }

  std::vector<Slot> compacted;
  compacted.reserve(_live_entries);

  // Preserve order of remaining entries.
  for (const auto& s : _eviction_vector) {
    if (s.valid)
      compacted.push_back(s);
  }

  _eviction_vector.swap(compacted);
  _tombstones = 0;

  // Reset hand to head (end) as in add_eviction_candidate when list becomes non-empty.
  if (_live_entries > 0 && !_eviction_vector.empty()) {
    _hand.store(_eviction_vector.size() - 1, std::memory_order_relaxed);
  } else {
    _hand.store(0, std::memory_order_relaxed);
  }
}

bool SieveEviction::_pick_current_candidate_locked(std::size_t& out_idx, EvictionItem& out_item) {
  if (_live_entries == 0 || _eviction_vector.empty())
    return false;

  auto idx = _hand.load(std::memory_order_relaxed) % _eviction_vector.size();
  if (!_eviction_vector[idx].valid) {
    (void)_advance_hand_to_prev_valid_locked();
    if (_live_entries == 0 || _eviction_vector.empty())
      return false;
    idx = _hand.load(std::memory_order_relaxed) % _eviction_vector.size();
    if (!_eviction_vector[idx].valid)
      return false;
  }

  out_idx = idx;
  out_item = _eviction_vector[idx].item;
  return true;
}

bool SieveEviction::perform_evictions(const PageSizeType required_size) {
  const auto bytes_required = bytes_for_size_type(required_size);
  _buffer_pool.reserve_bytes(bytes_required);

  bool counted_episode = false;
  bool evicted_any_in_call = false;

  // Adaptive scan multiplier control:
  // if we do bounded work but cannot evict (still oversubscribed), we gradually increase the scan budget.
  std::size_t no_progress_rounds = 0;

  // Adaptive skip control for hot spots (pinned / lock-fail) on the same index
  std::size_t pinned_last_idx = std::numeric_limits<std::size_t>::max();
  std::size_t pinned_streak = 0;

  std::size_t lockfail_last_idx = std::numeric_limits<std::size_t>::max();
  std::size_t lockfail_streak = 0;

  auto adaptive_skip = [](const std::size_t base, const std::size_t max, const std::size_t streak) {
    // streak: 0 => base, 1 => base*2, 2 => base*4, 3+ => base*8 ... capped
    const auto shift = std::min<std::size_t>(streak, 3);  // cap growth steps
    const auto skip = base << shift;
    return std::min(max, skip);
  };

  while (_buffer_pool.used_bytes.load(std::memory_order_relaxed) > _buffer_pool.max_bytes) {
    if (!counted_episode) {
      increment_counter(_buffer_pool.metrics->num_oversubscription_episodes);
      counted_episode = true;
    }

    std::size_t live_snapshot = 0;
    {
      std::lock_guard<std::mutex> lk(_mutex);
      live_snapshot = _live_entries;
      if (live_snapshot == 0 || _eviction_vector.empty()) {
        increment_counter(_buffer_pool.metrics->num_eviction_failures);
        _buffer_pool.free_bytes(bytes_required);
        return false;
      }
    }

    // Adaptive bounded scan budget: 2,4,6,8 depending on consecutive no-progress rounds.
    const auto scan_multiplier = std::min<std::size_t>(
        SIEVE_SCAN_MULTIPLIER_MAX, SIEVE_SCAN_MULTIPLIER_MIN + std::min<std::size_t>(no_progress_rounds, 3) * 2);

    const auto max_scans = live_snapshot * scan_multiplier + 1;
    bool evicted_in_this_round = false;

    for (std::size_t scans = 0;
         scans < max_scans && _buffer_pool.used_bytes.load(std::memory_order_relaxed) > _buffer_pool.max_bytes;
         ++scans) {
      std::size_t this_idx = 0;
      EvictionItem item{};

      {
        std::lock_guard<std::mutex> lk(_mutex);
        if (!_pick_current_candidate_locked(this_idx, item)) {
          break;
        }
      }

      increment_counter(_buffer_pool.metrics->num_eviction_candidate_inspections);

      const auto region = _buffer_pool.volatile_regions[static_cast<uint64_t>(item.page_id.size_type())];
      auto frame = region->get_frame(item.page_id);
      const auto current_state_and_version = frame->state_and_version();

      // Migrated entry -> tombstone (only if identity still matches slot)
      if (frame->node_id() != _buffer_pool.node_id) {
        std::lock_guard<std::mutex> lk(_mutex);
        if (this_idx < _eviction_vector.size()) {
          auto& slot = _eviction_vector[this_idx];
          if (slot.valid && slot.item.page_id == item.page_id && slot.item.timestamp == item.timestamp) {
            _invalidate_at_and_advance_locked(this_idx);
            increment_counter(_buffer_pool.metrics->num_eviction_queue_items_purged);
          } else if (_live_entries > 0) {
            (void)_advance_hand_to_prev_valid_locked();
          }
        }
        continue;
      }

      // Stale entry -> tombstone (only if identity still matches slot)
      if (Frame::version(current_state_and_version) != item.timestamp) {
        std::lock_guard<std::mutex> lk(_mutex);
        if (this_idx < _eviction_vector.size()) {
          auto& slot = _eviction_vector[this_idx];
          if (slot.valid && slot.item.page_id == item.page_id && slot.item.timestamp == item.timestamp) {
            _invalidate_at_and_advance_locked(this_idx);
            increment_counter(_buffer_pool.metrics->num_eviction_queue_items_purged);
          } else if (_live_entries > 0) {
            (void)_advance_hand_to_prev_valid_locked();
          }
        }
        continue;
      }

      // Pinned -> adaptive skip (start small; grow only when stuck on same idx)
      if (Frame::state(current_state_and_version) != Frame::UNLOCKED) {
        increment_counter(_buffer_pool.metrics->num_eviction_requeues_pinned);

        std::lock_guard<std::mutex> lk(_mutex);
        if (_live_entries > 0 && !_eviction_vector.empty()) {
          if (this_idx == pinned_last_idx) {
            ++pinned_streak;
          } else {
            pinned_last_idx = this_idx;
            pinned_streak = 0;
          }
          const auto skip = adaptive_skip(SIEVE_BASE_SKIP_ON_PINNED, SIEVE_MAX_SKIP, pinned_streak);
          (void)_advance_hand_skip_and_find_prev_valid_from_locked(this_idx, skip);
        }
        continue;
      }

      // Referenced -> clear and advance by 1 (canonical SIEVE behavior)
      if (Frame::is_referenced(current_state_and_version)) {
        increment_counter(_buffer_pool.metrics->num_eviction_requeues_referenced);
        frame->clear_reference();

        // Referenced is not a "hotspot" signal; reset hotspot streaks so we don't over-skip.
        pinned_last_idx = std::numeric_limits<std::size_t>::max();
        pinned_streak = 0;
        lockfail_last_idx = std::numeric_limits<std::size_t>::max();
        lockfail_streak = 0;

        std::lock_guard<std::mutex> lk(_mutex);
        if (_live_entries > 0 && !_eviction_vector.empty()) {
          (void)_advance_hand_skip_and_find_prev_valid_from_locked(this_idx, SIEVE_SKIP_ON_REFERENCED);
        }
        continue;
      }

      // Lock-fail -> adaptive skip (start small; grow only when stuck on same idx)
      if (!frame->try_lock_exclusive(current_state_and_version)) {
        increment_counter(_buffer_pool.metrics->num_eviction_requeues_lock_failed);

        std::lock_guard<std::mutex> lk(_mutex);
        if (_live_entries > 0 && !_eviction_vector.empty()) {
          if (this_idx == lockfail_last_idx) {
            ++lockfail_streak;
          } else {
            lockfail_last_idx = this_idx;
            lockfail_streak = 0;
          }
          const auto skip = adaptive_skip(SIEVE_BASE_SKIP_ON_LOCKFAIL, SIEVE_MAX_SKIP, lockfail_streak);
          (void)_advance_hand_skip_and_find_prev_valid_from_locked(this_idx, skip);
        }
        continue;
      }

      Assert(frame->node_id() == _buffer_pool.node_id, "Memory node mismatch: " + std::to_string(frame->node_id()) +
                                                           " != " + std::to_string(_buffer_pool.node_id));

      // Tombstone under lock, then evict without holding SIEVE lock.
      {
        std::lock_guard<std::mutex> lk(_mutex);
        if (_live_entries == 0 || _eviction_vector.empty() || this_idx >= _eviction_vector.size()) {
          frame->unlock_exclusive();
          continue;
        }

        auto& slot = _eviction_vector[this_idx];

        // ensure we tombstone the SAME item we picked.
        if (!slot.valid || slot.item.page_id != item.page_id || slot.item.timestamp != item.timestamp) {
          frame->unlock_exclusive();
          if (_live_entries > 0) {
            (void)_advance_hand_to_prev_valid_locked();
          }
          continue;
        }

        _invalidate_at_and_advance_locked(this_idx);
      }

      // Successful eviction resets hotspot streaks (we made progress)
      pinned_last_idx = std::numeric_limits<std::size_t>::max();
      pinned_streak = 0;
      lockfail_last_idx = std::numeric_limits<std::size_t>::max();
      lockfail_streak = 0;

      auto evict_item = item;
      _buffer_pool.evict(evict_item, frame);
      increment_counter(_buffer_pool.metrics->num_evictions);
      _buffer_pool.free_bytes(bytes_for_size_type(evict_item.page_id.size_type()));

      evicted_in_this_round = true;
      evicted_any_in_call = true;
    }

    // If still oversubscribed:
    if (_buffer_pool.used_bytes.load(std::memory_order_relaxed) > _buffer_pool.max_bytes) {
      // No progress at all in this call => real failure
      if (!evicted_in_this_round && !evicted_any_in_call) {
        increment_counter(_buffer_pool.metrics->num_eviction_failures);
        _buffer_pool.free_bytes(bytes_required);
        return false;
      }

      // Progress happened at least once in the call, but this bounded round didn't evict:
      // increase scan budget next time by bumping no_progress_rounds.
      if (!evicted_in_this_round) {
        ++no_progress_rounds;
      } else {
        no_progress_rounds = 0;
      }
      continue;
    }

    // We are no longer oversubscribed => reset
    no_progress_rounds = 0;
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

  std::lock_guard<std::mutex> lock{_mutex};

  const auto was_empty = (_live_entries == 0);

  _eviction_vector.push_back(Slot{{page_id, Frame::version(current_state_and_version)}, true});
  ++_live_entries;

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
    std::size_t idx = 0;
    EvictionItem item{};

    {
      std::lock_guard<std::mutex> lk(_mutex);
      if (!_pick_current_candidate_locked(idx, item))
        return;
    }

    const auto region = _buffer_pool.volatile_regions[static_cast<uint64_t>(item.page_id.size_type())];
    auto frame = region->get_frame(item.page_id);
    const auto current_state_and_version = frame->state_and_version();

    // Purge stale or migrated entries.
    if (frame->node_id() != _buffer_pool.node_id || Frame::version(current_state_and_version) != item.timestamp) {
      std::lock_guard<std::mutex> lk(_mutex);
      if (idx < _eviction_vector.size()) {
        auto& slot = _eviction_vector[idx];
        if (slot.valid && slot.item.page_id == item.page_id && slot.item.timestamp == item.timestamp) {
          _invalidate_at_and_advance_locked(idx);
          increment_counter(_buffer_pool.metrics->num_eviction_queue_items_purged);
        } else if (_live_entries > 0) {
          (void)_advance_hand_to_prev_valid_locked();
        }
      }
      --i;
      continue;
    }

    // Keep pinned and referenced frames; SIEVE handles referenced via scan/clear in perform_evictions().
  }
}

std::size_t SieveEviction::memory_consumption() const {
  std::lock_guard<std::mutex> lk(_mutex);
  return sizeof(*this) + sizeof(_eviction_vector) + sizeof(Slot) * _eviction_vector.capacity();
}

}  // namespace hyrise
