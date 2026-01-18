#include "second_chance_eviction.hpp"

namespace hyrise {

SecondChanceEviction::SecondChanceEviction(BufferPool& buffer_pool) : EvictionStrategy(buffer_pool) {}

void SecondChanceEviction::add_eviction_candidate(const PageID& page_id, Frame* const frame) {
  const auto current_state_and_version = frame->state_and_version();
  DebugAssert(frame->node_id() == _buffer_pool.node_id, "Memory node mismatch");
  increment_counter(_buffer_pool.metrics->num_eviction_queue_adds);
  _eviction_queue.push({page_id, Frame::version(current_state_and_version)});
  // _eviction_queue.emplace(page_id, Frame::version(current_state_and_version));
}

bool SecondChanceEviction::perform_evictions(const PageSizeType required_size) {
// TODO: Free at least 64 * PageSite bytes to reduce TLB shootdowns
  const auto bytes_required = bytes_for_size_type(required_size);
  auto freed_bytes = size_t{0};

  _buffer_pool.reserve_bytes(bytes_required);

  auto item = EvictionItem{};

  // Find potential victim frame if we don't have enough space left
  // TODO: Verify, that this is correct, cceh kthe numbersm, verify value type
  while (_buffer_pool.used_bytes.load(std::memory_order_relaxed) > _buffer_pool.max_bytes) {
    if (!_eviction_queue.try_pop(item)) {
#ifndef NDEBUG
      std::cerr << "[BM][DEBUG] ensure_free_pages failed: eviction_queue empty"
                << " required_bytes=" << bytes_required
                << " used_bytes=" << _buffer_pool.used_bytes.load(std::memory_order_relaxed)
                << " max_bytes=" << _buffer_pool.max_bytes
                << " freed_bytes=" << freed_bytes
                << " node_id=" << _buffer_pool.node_id
                << "\n";
#endif
      _buffer_pool.free_bytes(bytes_required);  // TODO: Check if this is correct
      return false;
    }

    auto region = _buffer_pool.volatile_regions[static_cast<uint64_t>(item.page_id.size_type())];
    auto frame = region->get_frame(item.page_id);
    auto current_state_and_version = frame->state_and_version();

    if (frame->node_id() != _buffer_pool.node_id) {
      increment_counter(_buffer_pool.metrics->num_eviction_queue_items_purged);
      continue;
    }

    // If the frame is already marked, we can evict it
    if (!item.can_evict(current_state_and_version)) {
      // If the frame is UNLOCKED, we can mark it
      if (item.can_mark(current_state_and_version)) {
        if (frame->try_mark(current_state_and_version)) {
          add_eviction_candidate(item.page_id, frame);
          continue;
        }
      }
      increment_counter(_buffer_pool.metrics->num_eviction_queue_items_purged);
      continue;
    }

    // Try locking the frame exclusively, TODO: prefer shared locking
    if (!frame->try_lock_exclusive(current_state_and_version)) {
      increment_counter(_buffer_pool.metrics->num_eviction_queue_items_purged);
      continue;
    }

    Assert(frame->node_id() == _buffer_pool.node_id,
           "Memory node mismatch: " + std::to_string(frame->node_id()) + " != " + std::to_string(_buffer_pool.node_id));

    _buffer_pool.evict(item, frame);

    increment_counter(_buffer_pool.metrics->num_evictions);

    // DebugAssert(Frame::state(frame->state_and_version()) != Frame::LOCKED, "Frame cannot be locked");

    const auto size_type = item.page_id.size_type();
    const auto evicted_bytes = bytes_for_size_type(size_type);
    freed_bytes += evicted_bytes;

    _buffer_pool.free_bytes(evicted_bytes);
  }

  return true;
}

void SecondChanceEviction::purge_eviction_candidates() {
  auto item = EvictionItem{};
  for (auto i = size_t{0}; i < MAX_EVICTION_QUEUE_PURGES; ++i) {
    if (!_eviction_queue.try_pop(item)) {
      return;
    }

    const auto region = _buffer_pool.volatile_regions[static_cast<uint64_t>(item.page_id.size_type())];
    auto frame = region->get_frame(item.page_id);
    auto current_state_and_version = frame->state_and_version();

    // The item is in state UNLOCKED and can be marked
    if (item.can_evict(current_state_and_version) || item.can_mark(current_state_and_version)) {
      _eviction_queue.push(item);
      continue;
    }
  }
}

std::size_t SecondChanceEviction::memory_consumption() const {
  return sizeof(*this) + sizeof(_eviction_queue) + sizeof(EvictionQueue::value_type) * _eviction_queue.unsafe_size();
}

}  // namespace hyrise