#include "delayed_fifo_reinsertion_eviction.hpp"

#include <limits>

#include "storage/buffer/buffer_pool.hpp"
#include "storage/buffer/eviction_strategy_registry.hpp"
#include "storage/buffer/helper.hpp"

namespace hyrise {

namespace {
EvictionStrategyRegistrar s_dfr_registrar{
    "dfr",
    [](BufferPool& buffer_pool) { return std::make_unique<DelayedFifoReinsertionEviction>(buffer_pool); },
    {"d-fr", "D-FR", "delayed-fr", "delayed_fifo_reinsertion", "delayed-fifo-reinsertion"}};
}  // namespace

DelayedFifoReinsertionEviction::DelayedFifoReinsertionEviction(BufferPool& buffer_pool)
    : EvictionStrategy(buffer_pool) {}

uint8_t DelayedFifoReinsertionEviction::_max_freq() const {
  const auto max = static_cast<uint8_t>((1u << FREQ_BITS) - 1u);
  return static_cast<uint8_t>(max > 3 ? 3 : max);
}

uint32_t DelayedFifoReinsertionEviction::_delay_time_cached() const {
  // Paper: delay_time = delay_ratio * cache_size (in objects/slots).
  // Here: cache_size is approximated and cached in BufferPool
  const auto cache_size_objects = static_cast<uint64_t>(_buffer_pool.cached_cache_size_objects());

  const auto delay = static_cast<uint64_t>(DELAY_RATIO * static_cast<double>(cache_size_objects));

  return static_cast<uint32_t>(delay > std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max()
                                                                            : delay);
}

void DelayedFifoReinsertionEviction::on_access(const PageID& /*page_id*/, Frame* const frame) {
  // on access, increment freq only if not within delay window;
  // always update access_time.

  const auto now = _time.fetch_add(1, std::memory_order_relaxed) + 1;

  const auto last = frame->last_access_time();

  // Unsigned wraparound subtraction is intentional (modular logical time).
  const uint32_t delta = now - last;

  const auto delay_time = _delay_time_cached();

  // Guard: last != 0 (first access / post-migration reset should not reward immediately)
  if (last != 0 && delta > delay_time) {
    frame->inc_reference_level_saturating(_max_freq());
  }

  frame->set_last_access_time(now);
}

void DelayedFifoReinsertionEviction::add_eviction_candidate(const PageID& page_id, Frame* const frame) {
  const auto current_state_and_version = frame->state_and_version();
  DebugAssert(frame->node_id() == _buffer_pool.node_id, "Memory node mismatch");
  increment_counter(_buffer_pool.metrics->num_eviction_queue_adds);
  _eviction_queue.push({page_id, Frame::version(current_state_and_version)});
}

bool DelayedFifoReinsertionEviction::perform_evictions(const PageSizeType required_size) {
  // TODO: Free at least 64 * PageSite bytes to reduce TLB shootdowns
  const auto bytes_required = bytes_for_size_type(required_size);
  auto freed_bytes = size_t{0};

  _buffer_pool.reserve_bytes(bytes_required);

  auto item = EvictionItem{};

  // Find potential victim frame if we don't have enough space left
  // TODO: Verify, that this is correct
  bool counted_episode = false;
  while (_buffer_pool.used_bytes.load(std::memory_order_relaxed) > _buffer_pool.max_bytes) {
    if (!counted_episode) {
      increment_counter(_buffer_pool.metrics->num_oversubscription_episodes);
      counted_episode = true;
    }

    if (!_eviction_queue.try_pop(item)) {
#ifndef NDEBUG
      std::cerr << "[BM][DEBUG] ensure_free_pages failed: eviction_queue empty"
                << " required_bytes=" << bytes_required
                << " used_bytes=" << _buffer_pool.used_bytes.load(std::memory_order_relaxed)
                << " max_bytes=" << _buffer_pool.max_bytes << " freed_bytes=" << freed_bytes
                << " node_id=" << _buffer_pool.node_id << "\n";
#endif
      increment_counter(_buffer_pool.metrics->num_eviction_failures);
      _buffer_pool.free_bytes(bytes_required);
      return false;
    }

    increment_counter(_buffer_pool.metrics->num_eviction_candidate_inspections);

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
      increment_counter(_buffer_pool.metrics->num_eviction_requeues_pinned);
      _eviction_queue.push(item);
      continue;
    }

    // If freq > 0, decrement and reinsert (promotion deferred to eviction).
    if (Frame::is_referenced(current_state_and_version)) {
      increment_counter(_buffer_pool.metrics->num_eviction_requeues_referenced);
      frame->dec_reference_level_if_positive();
      _eviction_queue.push(item);
      continue;
    }

    // Try locking the frame exclusively
    if (!frame->try_lock_exclusive(current_state_and_version)) {
      increment_counter(_buffer_pool.metrics->num_eviction_requeues_lock_failed);
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

void DelayedFifoReinsertionEviction::purge_eviction_candidates() {
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

std::size_t DelayedFifoReinsertionEviction::memory_consumption() const {
  return sizeof(*this) + sizeof(_eviction_queue) + sizeof(EvictionQueue::value_type) * _eviction_queue.unsafe_size();
}

}  // namespace hyrise
