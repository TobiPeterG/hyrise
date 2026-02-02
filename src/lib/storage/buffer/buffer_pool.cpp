#include "buffer_pool.hpp"

#include <algorithm>

#include "metrics.hpp"
#include "storage/buffer/ssd_region.hpp"
#include "volatile_region.hpp"

#include "storage/buffer/eviction_strategy_registry.hpp"

#ifndef NDEBUG
#include <iostream>
#include <sstream>
#include <vector>
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define HYRISE_HAS_EXECINFO 1
#else
#define HYRISE_HAS_EXECINFO 0
#endif
#endif

namespace hyrise {
// TODO: properly check if disabled or not
BufferPool::BufferPool(const bool enabled, const size_t pool_size, const bool enable_eviction_purge_worker,
                       std::array<std::shared_ptr<VolatileRegion>, NUM_PAGE_SIZE_TYPES> volatile_regions,
                       MigrationPolicy migration_policy, std::shared_ptr<SSDRegion> ssd_region,
                       std::shared_ptr<BufferPool> target_buffer_pool, const std::string& eviction_strategy_name,
                       const NodeID numa_node, std::shared_ptr<BufferPoolMetrics> metrics)
    : max_bytes(pool_size),
      used_bytes(0),
      resident_objects(0),
      metrics(metrics),
      enabled(enabled),
      volatile_regions(volatile_regions),
      node_id(numa_node),
      ssd_region(ssd_region),
      target_buffer_pool(target_buffer_pool),
      eviction_strategy(EvictionStrategyRegistry::instance().create(eviction_strategy_name, *this)),
      migration_policy(migration_policy),
      eviction_purge_worker(
          enable_eviction_purge_worker
              ? std::make_unique<PausableLoopThread>(IDLE_EVICTION_QUEUE_PURGE,
                                                     [&](size_t) { eviction_strategy->purge_eviction_candidates(); })
              : nullptr) {
  // Initialize cached approximation once.
  _recompute_cache_now();
}

void BufferPool::_maybe_recompute_cache_on_object_event() {
  // Increment epoch and recompute only every 256 object in/out events.
  const auto epoch = _object_epoch.fetch_add(1, std::memory_order_relaxed) + 1;
  if ((epoch & RECOMPUTE_MASK) == 0) {
    _recompute_cache_now();
  }
}

void BufferPool::_recompute_cache_now() {
  // We approximate average object size by used_bytes / resident_objects.
  // Then cache_size_objects ≈ max_bytes / avg_object_bytes.
  // If resident_objects == 0, fall back to min-page size capacity.
  const auto objs = resident_objects.load(std::memory_order_relaxed);

  const auto min_page_bytes = bytes_for_size_type(MIN_PAGE_SIZE_TYPE);
  const uint64_t safe_min_page = (min_page_bytes ? min_page_bytes : 1u);

  uint64_t avg_object_bytes = safe_min_page;

  if (objs > 0) {
    const auto bytes = used_bytes.load(std::memory_order_relaxed);

    // If bytes is 0 (possible in tests / early init), fall back safely.
    if (bytes > 0) {
      avg_object_bytes = bytes / objs;
      if (avg_object_bytes == 0) {
        avg_object_bytes = 1;
      }
    }
  }

  uint64_t capacity_objects = max_bytes / (avg_object_bytes ? avg_object_bytes : 1u);

  // Keep it non-zero
  if (capacity_objects == 0) {
    capacity_objects = 1;
  }

  if (capacity_objects > std::numeric_limits<uint32_t>::max()) {
    capacity_objects = std::numeric_limits<uint32_t>::max();
  }

  _cached_cache_size_objects.store(static_cast<uint32_t>(capacity_objects), std::memory_order_relaxed);
}

uint32_t BufferPool::cached_cache_size_objects() const {
  auto v = _cached_cache_size_objects.load(std::memory_order_relaxed);
  if (v == 0) {
    // Should not happen
    const auto min_page_bytes = bytes_for_size_type(MIN_PAGE_SIZE_TYPE);
    const auto slots = static_cast<uint64_t>(max_bytes / (min_page_bytes ? min_page_bytes : 1u));
    v = static_cast<uint32_t>(std::min<uint64_t>(slots ? slots : 1u, std::numeric_limits<uint32_t>::max()));
  }
  return v;
}

void BufferPool::account_object_in() {
  resident_objects.fetch_add(1, std::memory_order_relaxed);

  if (metrics) {
    const auto now = metrics->resident_objects.fetch_add(1, std::memory_order_relaxed) + 1;
    // best-effort peak tracking
    auto prev_peak = metrics->peak_resident_objects.load(std::memory_order_relaxed);
    while (now > prev_peak &&
           !metrics->peak_resident_objects.compare_exchange_weak(prev_peak, now, std::memory_order_relaxed)) {
      // retry
    }
  }

  _maybe_recompute_cache_on_object_event();
}

void BufferPool::account_object_out() {
#ifndef NDEBUG
  const auto before = resident_objects.load(std::memory_order_relaxed);
  if (before == 0) {
    std::ostringstream oss;
    oss << "BufferPool::account_object_out underflow: resident_objects=0 node_id=" << node_id << " enabled=" << enabled;
    Fail(oss.str());
  }
#endif

  resident_objects.fetch_sub(1, std::memory_order_relaxed);

  if (metrics) {
#ifndef NDEBUG
    const auto mbefore = metrics->resident_objects.load(std::memory_order_relaxed);
    if (mbefore == 0) {
      std::ostringstream oss;
      oss << "BufferPoolMetrics::resident_objects underflow: node_id=" << node_id << " enabled=" << enabled;
      Fail(oss.str());
    }
#endif
    metrics->resident_objects.fetch_sub(1, std::memory_order_relaxed);
  }

  _maybe_recompute_cache_on_object_event();
}

uint64_t BufferPool::resident_object_count() const {
  return resident_objects.load(std::memory_order_relaxed);
}

void BufferPool::add_eviction_candidate(const PageID page_id, Frame* frame) {
  eviction_strategy->add_eviction_candidate(page_id, frame);
}

void BufferPool::on_access(const PageID& page_id, Frame* frame) {
  eviction_strategy->on_access(page_id, frame);
}

void BufferPool::purge_eviction_candidates() {
  eviction_strategy->purge_eviction_candidates();
}

void BufferPool::free_bytes(const uint64_t bytes) {
#ifndef NDEBUG
  if (bytes == 0) {
    return;
  }

  const auto before = used_bytes.load(std::memory_order_relaxed);
  if (before < bytes) {
    std::ostringstream oss;
    oss << "BufferPool::free_bytes underflow: before=" << before << " sub=" << bytes << " max_bytes=" << max_bytes
        << " node_id=" << node_id << " enabled=" << enabled;

#if HYRISE_HAS_EXECINFO
    std::vector<void*> addrs(64);
    const auto n = ::backtrace(addrs.data(), static_cast<int>(addrs.size()));
    if (n > 0) {
      addrs.resize(static_cast<size_t>(n));
    }
    char** syms = ::backtrace_symbols(addrs.data(), static_cast<int>(addrs.size()));
    if (syms) {
      oss << "\nBacktrace:";
      for (size_t i = 0; i < addrs.size(); ++i) {
        oss << "\n  " << syms[i];
      }
      std::free(syms);
    }
#endif

    Fail(oss.str());
  }
#endif

  used_bytes.fetch_sub(bytes, std::memory_order_relaxed);
}

uint64_t BufferPool::reserve_bytes(const uint64_t bytes) {
#ifndef NDEBUG
  if (bytes == 0) {
    return used_bytes.load(std::memory_order_relaxed);
  }
#endif

  const auto before = used_bytes.fetch_add(bytes, std::memory_order_relaxed);

#ifndef NDEBUG
  const auto after = before + bytes;
  if (after > max_bytes + (bytes_for_size_type(MAX_PAGE_SIZE_TYPE) * 4)) {
    std::cerr << "[BM][DEBUG] reserve_bytes large oversubscription: before=" << before << " add=" << bytes
              << " after=" << after << " max_bytes=" << max_bytes << " node_id=" << node_id << "\n";
  }
#endif

  return before;
}

bool BufferPool::ensure_free_pages(const PageSizeType required_size) const {
  return eviction_strategy->perform_evictions(required_size);
}

void BufferPool::evict(EvictionItem& item, Frame* frame) {
  DebugAssert(Frame::state(frame->state_and_version()) == Frame::LOCKED, "Frame cannot be locked");
  auto region = volatile_regions[static_cast<uint64_t>(item.page_id.size_type())];
  const auto num_bytes = bytes_for_size_type(item.page_id.size_type());

  // We try to evict the current item. Based on the migration policy, we to evict the page to a lower tier.
  // If this fails, we retry and some point, we might land on SSD.
  for (auto repeat = size_t{0}; repeat < MAX_REPEAT_COUNT; ++repeat) {
    // If we have a target buffer pool and we don't want to bypass it, we move the page to the other pool
    auto write_to_ssd =
        !target_buffer_pool || !target_buffer_pool->enabled || migration_policy.bypass_numa_during_write();

#if HYRISE_NUMA_SUPPORT
    // If we intend to migrate to another NUMA node, make sure NUMA is available at runtime and the node exists.
    if (!write_to_ssd) {
      if (numa_available() < 0) {
        write_to_ssd = true;
      } else {
        const auto max_node = numa_max_node();
        if (static_cast<int>(target_buffer_pool->node_id) > max_node) {
          write_to_ssd = true;
        }
      }
    }
#else
    // Built without NUMA support -> never try to migrate to another node
    if (!write_to_ssd) {
      write_to_ssd = true;
    }
#endif

    if (write_to_ssd) {
      // Otherwise we just write the page if its dirty and free the associated pages
      if (frame->is_dirty()) {
        auto data = region->get_page(item.page_id);
        ssd_region->write_page(item.page_id, data);
        region->protect_page(item.page_id);
        frame->reset_dirty();
      }

      // Resident -> evicted (SSD)
      account_object_out();

      region->free(item.page_id);
      frame->unlock_exclusive_and_set_evicted();

      increment_counter(metrics->total_bytes_copied_to_ssd, num_bytes);

      return;
    } else {
      // Or we just move to other numa node and unlock again
      if (!target_buffer_pool->ensure_free_pages(item.page_id.size_type())) {
        yield(repeat);
        continue;
      }

      region->mbind_to_numa_node(item.page_id, target_buffer_pool->node_id);

      // Resident moves between pools: source out, target in.
      account_object_out();
      target_buffer_pool->account_object_in();

      frame->set_node_id(target_buffer_pool->node_id);

      // Each pool has its own logical clock, but last_access_time is stored in
      // the Frame. If we migrate a frame between pools, the delta computation
      // in D-FR can see "time going backwards" and wrap, causing spurious
      // reward. Reset the per-frame timestamp on migration.
      frame->set_last_access_time(0);

      frame->unlock_exclusive();
      target_buffer_pool->add_eviction_candidate(item.page_id, frame);
      return;
    }
  }
  Fail("Could not evict page after trying for " + std::to_string(MAX_REPEAT_COUNT) + " times");
}

size_t BufferPool::memory_consumption() const {
  return sizeof(*this) + eviction_strategy->memory_consumption();
}

size_t BufferPool::free_bytes_node() const {
#if HYRISE_NUMA_SUPPORT
  if (node_id == INVALID_NODE_ID) {
    return 0;
  }
  long long free_bytes;
  numa_node_size(node_id, &free_bytes);
  return free_bytes;
#else
  return 0;
#endif
}

size_t BufferPool::total_bytes_node() const {
#if HYRISE_NUMA_SUPPORT
  if (node_id == INVALID_NODE_ID) {
    return 0;
  }
  return numa_node_size(node_id, nullptr);
#else
  return 0;
#endif
}

}  // namespace hyrise
