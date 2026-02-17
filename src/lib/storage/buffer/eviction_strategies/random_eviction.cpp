#include "random_eviction.hpp"

#include "storage/buffer/eviction_strategy_registry.hpp"

namespace hyrise {

namespace {
EvictionStrategyRegistrar s_random_registrar{
  "random",
  [](BufferPool& buffer_pool) { return std::make_unique<RandomEviction>(buffer_pool); },
  {}};
}

RandomEviction::RandomEviction(BufferPool& buffer_pool) : EvictionStrategy(buffer_pool) {}

void RandomEviction::add_eviction_candidate(const PageID& page_id, Frame* frame) {
  DebugAssert(frame->node_id() == _buffer_pool.node_id, "Memory node mismatch");
  increment_counter(_buffer_pool.metrics->num_eviction_queue_adds);

  const auto current_state_and_version = frame->state_and_version();
  const auto timestamp = Frame::version(current_state_and_version);

  // One could argue that this isn't actually random, and they would be correct. I have chosen this way for two reasons:
  // 1) a hash should theoretically be well distributed, so random-like. The seeding here should vary enough.
  // 2) you'd either have to store one random engine per thread (which seems difficult here), or always create a new
  //    random engine, which would take an eternity.
  const auto priority = std::hash<uint64_t>{}(page_id.index + timestamp);
  _queue.push(QueueEntry{priority, {.page_id = page_id, .timestamp = timestamp}});
}

bool RandomEviction::perform_evictions(const PageSizeType required_size) {
  const auto bytes_required = bytes_for_size_type(required_size);

  _buffer_pool.reserve_bytes(bytes_required);
  QueueEntry item;
  while (_buffer_pool.used_bytes.load(std::memory_order_relaxed) > _buffer_pool.max_bytes) {
    if (!_queue.try_pop(item)) {
      increment_counter(_buffer_pool.metrics->num_eviction_failures);
      _buffer_pool.free_bytes(bytes_required);
      return false;
    }

    increment_counter(_buffer_pool.metrics->num_eviction_candidate_inspections);

    const auto region = _buffer_pool.volatile_regions[static_cast<uint64_t>(item.second.page_id.size_type())];
    auto frame = region->get_frame(item.second.page_id);
    const auto current_state_and_version = frame->state_and_version();

    if (frame->node_id() != _buffer_pool.node_id) {
      increment_counter(_buffer_pool.metrics->num_eviction_queue_items_purged);
      continue;
    }

    // Stale entry (frame generation changed)
    if (Frame::version(current_state_and_version) != item.second.timestamp) {
      increment_counter(_buffer_pool.metrics->num_eviction_queue_items_purged);
      continue;
    }

    // Keep pinned frames in the list; skip by requeueing.
    if (Frame::state(current_state_and_version) != Frame::UNLOCKED) {
      increment_counter(_buffer_pool.metrics->num_eviction_requeues_pinned);
      _queue.push(item);
      continue;
    }

    // Try locking the frame exclusively
    if (!frame->try_lock_exclusive(current_state_and_version)) {
      increment_counter(_buffer_pool.metrics->num_eviction_requeues_lock_failed);
      _queue.push(item);
      continue;
    }

    Assert(frame->node_id() == _buffer_pool.node_id,
           "Memory node mismatch: " + std::to_string(frame->node_id()) + " != " + std::to_string(_buffer_pool.node_id));

    _buffer_pool.evict(item.second, frame);
    increment_counter(_buffer_pool.metrics->num_evictions);

    const auto evicted_bytes = bytes_for_size_type(item.second.page_id.size_type());
    _buffer_pool.free_bytes(evicted_bytes);
  }

  return true;
}

void RandomEviction::purge_eviction_candidates() {
  auto item = QueueEntry{};
  for (auto i = size_t{0}; i < MAX_EVICTION_QUEUE_PURGES; ++i) {
    if (!_queue.try_pop(item)) {
      return;
    }

    const auto region = _buffer_pool.volatile_regions[static_cast<uint64_t>(item.second.page_id.size_type())];
    auto frame = region->get_frame(item.second.page_id);
    const auto current_state_and_version = frame->state_and_version();

    // Purge stale or migrated entries. Keep pinned/ref entries.
    if (frame->node_id() != _buffer_pool.node_id || Frame::version(current_state_and_version) != item.second.timestamp) {
      increment_counter(_buffer_pool.metrics->num_eviction_queue_items_purged);
      continue;
    }

    _queue.push(item);
  }
}

// void RandomEviction::on_access(const PageID& page_id, Frame* frame) {
//
// }

std::size_t RandomEviction::memory_consumption() const {
  return sizeof(*this)
  + sizeof(_queue) + (sizeof(decltype(_queue)::value_type) * _queue.size());
}

bool RandomEviction::PriorityFunction::operator()(const QueueEntry& left, const QueueEntry& right) const {
  return left.first < right.first;
}

}  // namespace hyrise