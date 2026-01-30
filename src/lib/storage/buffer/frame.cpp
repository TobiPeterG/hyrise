#include <bitset>
#include <iostream>

#include "storage/buffer/helper.hpp"
#include "storage/buffer/frame.hpp"

namespace hyrise {

Frame::Frame() {
  _state_and_version.store(update_state_with_same_version(0, EVICTED), std::memory_order_release);
}

void Frame::set_node_id(const NodeID node_id) {
  DebugAssert(node_id != INVALID_NODE_ID, "Cannot set empty numa node");
  DebugAssert(state(_state_and_version.load()) == LOCKED, "Frame must be locked to set memory node.");
  auto old_state_and_version = _state_and_version.load();
  while (true) {
    auto new_state_and_version =
        old_state_and_version ^
        ((old_state_and_version ^ (static_cast<Frame::StateVersionType>(node_id) << NODE_ID_SHIFT)) & NODE_ID_MASK);
    if (_state_and_version.compare_exchange_strong(old_state_and_version, new_state_and_version)) {
      DebugAssert((old_state_and_version & ~NODE_ID_MASK) == (new_state_and_version & ~NODE_ID_MASK),
                  "Settings the numa node failed");
      break;
    }
  }

  // TODO: May want to test that the state is not modified
  DebugAssert(Frame::node_id(_state_and_version.load()) == node_id,
              "Setting numa node didnt work: " + std::to_string(Frame::node_id(_state_and_version.load())));
}

void Frame::set_dirty(const bool new_dirty) {
  if (new_dirty) {
    _state_and_version.fetch_or(DIRTY_MASK, std::memory_order_relaxed);
  } else {
    _state_and_version.fetch_and(~DIRTY_MASK, std::memory_order_relaxed);
  }
  DebugAssert(is_dirty() == new_dirty, "Setting dirty didnt work");
}

void Frame::reset_dirty() {
  _state_and_version.fetch_and(~DIRTY_MASK, std::memory_order_relaxed);
}

bool Frame::is_dirty() const {
  return (_state_and_version.load(std::memory_order_relaxed) & DIRTY_MASK) != 0;
}

uint8_t Frame::reference_level() const {
  const auto sav = _state_and_version.load(std::memory_order_relaxed);
  return static_cast<uint8_t>((sav & REF_MASK) >> REF_SHIFT);
}

bool Frame::is_referenced(StateVersionType state_and_version) {
  return ((state_and_version & REF_MASK) >> REF_SHIFT) != 0;
}

void Frame::mark_referenced() {
  auto old = _state_and_version.load(std::memory_order_relaxed);
  while (true) {
    const auto ref = static_cast<uint8_t>((old & REF_MASK) >> REF_SHIFT);
    const auto new_ref = static_cast<uint8_t>(ref < 3 ? (ref + 1) : 3);
    const auto cleared = old & ~REF_MASK;
    const auto desired = cleared | (static_cast<uint64_t>(new_ref) << REF_SHIFT);
    if (_state_and_version.compare_exchange_weak(old, desired, std::memory_order_relaxed)) {
      return;
    }
  }
}

void Frame::set_reference_max() {
  auto old = _state_and_version.load(std::memory_order_relaxed);
  while (true) {
    const auto cleared = old & ~REF_MASK;
    const auto desired = cleared | (static_cast<uint64_t>(3) << REF_SHIFT);
    if (_state_and_version.compare_exchange_weak(old, desired, std::memory_order_relaxed)) {
      return;
    }
  }
}

void Frame::clear_reference() {
  _state_and_version.fetch_and(~REF_MASK, std::memory_order_relaxed);
}

void Frame::unlock_exclusive_and_set_evicted() {
  Assert(state(_state_and_version.load()) == LOCKED, "Frame must be locked to set evicted flag.");
  _state_and_version.store(update_state_with_increment_version(_state_and_version.load(), EVICTED),
                           std::memory_order_release);
}

Frame::StateVersionType Frame::state(Frame::StateVersionType state_and_version) {
  return (state_and_version & STATE_MASK) >> STATE_SHIFT;
}

Frame::StateVersionType Frame::version(Frame::StateVersionType state_and_version) {
  return state_and_version & VERSION_MASK;
}

Frame::StateVersionType Frame::state_and_version() const {
  return _state_and_version.load(std::memory_order_relaxed);
}

NodeID Frame::node_id() const {
  return node_id(_state_and_version.load(std::memory_order_relaxed));
}

NodeID Frame::node_id(Frame::StateVersionType state_and_version) {
  return static_cast<NodeID>((state_and_version & NODE_ID_MASK) >> NODE_ID_SHIFT);
}

bool Frame::try_lock_shared(Frame::StateVersionType old_state_and_version) {
  auto old_state = state(old_state_and_version);
  if (old_state < LOCKED_SHARED) {
    return _state_and_version.compare_exchange_strong(
        old_state_and_version, update_state_with_same_version(old_state_and_version, old_state + 1));
  }
  return false;
}

bool Frame::try_lock_exclusive(Frame::StateVersionType old_state_and_version) {
  Assert(state(old_state_and_version) == UNLOCKED || state(old_state_and_version) == EVICTED,
         "Frame must be unlocked to lock exclusive, instead: " + std::to_string(state(old_state_and_version)));
  return _state_and_version.compare_exchange_strong(old_state_and_version,
                                                    update_state_with_same_version(old_state_and_version, LOCKED));
}

bool Frame::unlock_shared() {
  while (true) {
    auto old_state_and_version = _state_and_version.load(std::memory_order_relaxed);
    auto old_state = state(old_state_and_version);
    Assert(old_state > 0 && old_state <= LOCKED_SHARED, "Frame must be locked shared to unlock shared.");
    auto new_state = old_state - 1;
    if (_state_and_version.compare_exchange_strong(old_state_and_version,
                                                   update_state_with_same_version(old_state_and_version, new_state))) {
      return new_state == Frame::UNLOCKED;  // return true if last shared lock was released
    }
  }
}

void Frame::unlock_exclusive() {
  Assert(state(_state_and_version.load()) == LOCKED,
         "Frame must be locked to unlock exclusive. " + std::to_string(state(_state_and_version.load())));

  // ersion is a generation counter.
  // It must not be incremented on normal unlock, only when the frame generation changes (e.g., on eviction/reuse).
  _state_and_version.store(update_state_with_same_version(_state_and_version.load(), UNLOCKED),
                           std::memory_order_release);
}

Frame::StateVersionType Frame::update_state_with_same_version(Frame::StateVersionType old_version_and_state,
                                                              Frame::StateVersionType new_state) {
  constexpr auto SHIFT = NUM_BITS - STATE_SHIFT;
  static_assert(SHIFT == 16, "Shift must be 8.");
  return ((old_version_and_state << SHIFT) >> SHIFT) | (new_state << STATE_SHIFT);
}

Frame::StateVersionType Frame::update_state_with_increment_version(Frame::StateVersionType old_version_and_state,
                                                                   Frame::StateVersionType new_state) {
  constexpr auto SHIFT = NUM_BITS - STATE_SHIFT;
  static_assert(SHIFT == 16, "Shift must be 8.");
  return (((old_version_and_state << SHIFT) >> SHIFT) + 1) | (new_state << STATE_SHIFT);
}

void Frame::debug_print() {
  std::cout << std::bitset<sizeof(_state_and_version) * CHAR_BIT>(_state_and_version.load()) << std::endl;
}

bool Frame::is_unlocked() const {
  return state(_state_and_version.load(std::memory_order_relaxed)) == UNLOCKED;
}

}  // namespace hyrise
