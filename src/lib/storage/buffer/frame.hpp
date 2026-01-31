#pragma once

#include <atomic>
#include <bit>
#include <cstdint>
#include <limits>

#include "types.hpp"

namespace hyrise {

class Frame {
 public:
  using StateVersionType = uint64_t;

  static constexpr StateVersionType UNLOCKED = 0;
  static constexpr StateVersionType LOCKED_SHARED = 0xFFFF - 2;  // max shared lock count marker
  static constexpr StateVersionType LOCKED = 0xFFFF - 1;         // exclusive
  static constexpr StateVersionType EVICTED = 0xFFFF;            // evicted

  Frame();

  // Flags and metadata
  void set_node_id(const NodeID node_id);

  void set_dirty(const bool new_dirty);
  bool is_dirty() const;
  void reset_dirty();

  NodeID node_id() const;

  // Reference/frequency counter stored in REF_MASK (2-bit saturating counter in [0..3]).
  // Only eviction strategies should manipulate this.
  uint8_t reference_level() const;
  static uint8_t reference_level(StateVersionType state_and_version);

  // Set reference level to an exact value (clamped by caller).
  void set_reference_level(uint8_t level);

  // Saturating increment with a caller-provided max (<= 3).
  void inc_reference_level_saturating(uint8_t max_level);

  // Decrement if > 0
  void dec_reference_level_if_positive();

  // Clear to 0.
  void clear_reference();

  // Fast check on raw state_and_version
  static bool is_referenced(StateVersionType state_and_version);

  // Per-frame last-access timestamp
  uint32_t last_access_time() const;
  void set_last_access_time(uint32_t t);

  // State transitions
  void unlock_exclusive_and_set_evicted();

  bool try_lock_shared(StateVersionType old_state_and_version);
  bool try_lock_exclusive(StateVersionType old_state_and_version);

  // Removes a shared lock and returns true if the frame is now unlocked
  bool unlock_shared();

  void unlock_exclusive();

  bool is_unlocked() const;

  // State and version helper
  StateVersionType state_and_version() const;
  static StateVersionType state(StateVersionType state_and_version);
  static StateVersionType version(StateVersionType state_and_version);
  static NodeID node_id(StateVersionType state_and_version);

  void debug_print();

 private:
  // clang-format off
  static constexpr uint64_t NODE_ID_MASK      = 0x00000F0000000000;  // 4 bits
  static constexpr uint64_t DIRTY_MASK        = 0x0000100000000000;  // 1 bit
  static constexpr uint64_t REF_MASK          = 0x0000600000000000;  // 2 bits
  static constexpr uint64_t IN_EVICTLIST_MASK = 0x0000800000000000;  // 1 bit
  static constexpr uint64_t STATE_MASK        = 0xFFFF000000000000;  // 16 bits
  static constexpr uint64_t VERSION_MASK      = 0x000000FFFFFFFFFF;  // 40 bits

  static_assert((NODE_ID_MASK ^ DIRTY_MASK ^ REF_MASK ^ IN_EVICTLIST_MASK ^ STATE_MASK ^ VERSION_MASK) ==
                std::numeric_limits<StateVersionType>::max());
  // clang-format on

  static constexpr uint64_t NUM_BITS = sizeof(StateVersionType) * CHAR_BIT;
  static constexpr uint64_t NODE_ID_SHIFT = std::countr_zero(NODE_ID_MASK);
  static constexpr uint64_t DIRTY_SHIFT = std::countr_zero(DIRTY_MASK);
  static constexpr uint64_t REF_SHIFT = std::countr_zero(REF_MASK);
  static constexpr uint64_t STATE_SHIFT = std::countr_zero(STATE_MASK);

  StateVersionType update_state_with_same_version(StateVersionType old_version_and_state, StateVersionType new_state);
  StateVersionType update_state_with_increment_version(StateVersionType old_version_and_state,
                                                       StateVersionType new_state);

  std::atomic<StateVersionType> _state_and_version;

  // Logical timestamp, updated by eviction strategies on access.
  std::atomic<uint32_t> _last_access_time{0};
};

}  // namespace hyrise