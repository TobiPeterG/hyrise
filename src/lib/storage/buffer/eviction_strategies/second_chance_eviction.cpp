#include "second_chance_eviction.hpp"

#include "storage/buffer/eviction_strategy_registry.hpp"

namespace hyrise {

namespace {
EvictionStrategyRegistrar s_second_chance_registrar{
    "second_chance",
    [](BufferPool& buffer_pool) { return std::make_unique<SecondChanceEviction>(buffer_pool); },
    {"second chance", "second-chance", "2nd_chance", "2nd chance"}};
}  // namespace

SecondChanceEviction::SecondChanceEviction(BufferPool& buffer_pool) : EvictionStrategy(buffer_pool) {}

void SecondChanceEviction::add_eviction_candidate(const PageID& page_id, Frame* const frame) {
  const auto current_state_and_version = frame->state_and_version();
  DebugAssert(frame->node_id() == _buffer_pool.node_id, "Memory node mismatch");
  increment_counter(_buffer_pool.metrics->num_eviction_queue_adds);
  _eviction_queue.push({page_id, Frame::version(current_state_and_version)});
}

bool SecondChanceEviction::perform_evictions(const PageSizeType required_size) {
  // TODO: Free at least 64 * PageSite bytes to reduce TLB shootdowns
  const auto bytes_required = bytes_for_size_type(required_size);
  auto freed_bytes = size_t{0};

  _buffer_pool.reserve_bytes(bytes_required);

  auto item = EvictionItem{};

  // Find potential victim frame if we don't have enough space left
  // TODO: Verify, that this is correct
  while (_buffer_pool.used_bytes.load(std::memory_order_relaxed) > _buffer_pool.max_bytes) {
    if (!_eviction_queue.try_pop(item)) {
#ifndef NDEBUG
      std::cerr << "[BM][DEBUG] ensure_free_pages failed: eviction_queue empty"
                << " required_bytes=" << bytes_required
                << " used_bytes=" << _buffer_pool.used_bytes.load(std::memory_order_relaxed)
                << " max_bytes=" << _buffer_pool.max_bytes << " freed_bytes=" << freed_bytes
                << " node_id=" << _buffer_pool.node_id << "\n";
#endif
      _buffer_pool.free_bytes(bytes_required);
      return false;
    }

    auto region = _buffer_pool.volatile_regions[static_cast<uint64_t>(item.page_id.size_type())];
    auto frame = region->get_frame(item.page_id);
    const auto current_state_and_version = frame->state_and_version();

    if (frame->node_id() != _buffer_pool.node_id) {
      increment_counter(_buffer_pool.metrics->num_eviction_queue_items_purged);
      continue;
    }

    // Stale entry (frame generation changed)
    if (Frame::version(current_state_and_version) != item.timestamp) {
      increment_counter(_buffer_pool.metrics->num_eviction_queue_items_purged);
      continue;
    }

    // Keep pinned frames in the list; skip by requeueing.
    if (Frame::state(current_state_and_version) != Frame::UNLOCKED) {
      _eviction_queue.push(item);
      continue;
    }

    // Second chance
    if (Frame::is_referenced(current_state_and_version)) {
      frame->clear_reference();
      _eviction_queue.push(item);
      continue;
    }

    // Try locking the frame exclusively
    if (!frame->try_lock_exclusive(current_state_and_version)) {
      _eviction_queue.push(item);
      continue;
    }

    Assert(frame->node_id() == _buffer_pool.node_id,
           "Memory node mismatch: " + std::to_string(frame->node_id()) + " != " + std::to_string(_buffer_pool.node_id));

    _buffer_pool.evict(item, frame);
    increment_counter(_buffer_pool.metrics->num_evictions);

    const auto evicted_bytes = bytes_for_size_type(item.page_id.size_type());
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
    const auto current_state_and_version = frame->state_and_version();

    // Purge stale or migrated entries. Keep pinned/ref entries.
    if (frame->node_id() != _buffer_pool.node_id || Frame::version(current_state_and_version) != item.timestamp) {
      increment_counter(_buffer_pool.metrics->num_eviction_queue_items_purged);
      continue;
    }

    _eviction_queue.push(item);
  }
}

std::size_t SecondChanceEviction::memory_consumption() const {
  return sizeof(*this) + sizeof(_eviction_queue) + sizeof(EvictionQueue::value_type) * _eviction_queue.unsafe_size();
}

}  // namespace hyrise
