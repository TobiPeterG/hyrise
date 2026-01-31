#pragma once

#include <atomic>
#include <memory>

#include <storage/buffer/eviction_strategy.hpp>
#include "storage/buffer/helper.hpp"
#include "storage/buffer/migration_policy.hpp"
#include "types.hpp"
#include "utils/pausable_loop_thread.hpp"

namespace hyrise {

class SSDRegion;
class VolatileRegion;
struct BufferPoolMetrics;

struct BufferPool {
  friend EvictionStrategy;

  BufferPool(const bool enabled, const size_t pool_size, const bool enable_eviction_purge_worker,
             std::array<std::shared_ptr<VolatileRegion>, NUM_PAGE_SIZE_TYPES> volatile_regions,
             MigrationPolicy migration_policy, std::shared_ptr<SSDRegion> ssd_region,
             std::shared_ptr<BufferPool> target_buffer_pool, const std::string& eviction_strategy_name,
             const NodeID numa_node, std::shared_ptr<BufferPoolMetrics> metrics);

  void evict(EvictionItem& item, Frame* frame);

  uint64_t reserve_bytes(const uint64_t bytes);

  void free_bytes(const uint64_t bytes);

  bool ensure_free_pages(const PageSizeType required_size) const;

  void add_eviction_candidate(const PageID page_id, Frame* frame);

  void on_access(const PageID& page_id, Frame* frame);

  void purge_eviction_candidates();

  size_t free_bytes_node() const;

  size_t total_bytes_node() const;

  size_t memory_consumption() const;

  // Each resident PageID counts as one object.
  void account_object_in();
  void account_object_out();

  uint64_t resident_object_count() const;

  // cached approximation of "cache_size in objects".
  // Updated every 256 object in/out operations
  uint32_t cached_cache_size_objects() const;

  // The maximum number of bytes that can be allocated
  const uint64_t max_bytes;

  // The number of bytes that are currently used (budget/reservation based)
  std::atomic_uint64_t used_bytes;

  // Number of resident objects in this pool.
  std::atomic_uint64_t resident_objects{0};

  std::shared_ptr<BufferPoolMetrics> metrics;

  std::shared_ptr<SSDRegion> ssd_region;

  std::shared_ptr<BufferPool> target_buffer_pool;

  // Async background worker that purges the eviction queue
  std::unique_ptr<PausableLoopThread> eviction_purge_worker;

  std::unique_ptr<EvictionStrategy> eviction_strategy;

  const MigrationPolicy migration_policy;

  std::array<std::shared_ptr<VolatileRegion>, NUM_PAGE_SIZE_TYPES> volatile_regions;

  const NodeID node_id;

  const bool enabled;

 private:
  // Recompute cached cache-size approximation occasionally.
  void _maybe_recompute_cache_on_object_event();
  void _recompute_cache_now();

  // Update period: 256 object events
  static constexpr uint32_t RECOMPUTE_MASK = 0xFF;  // every 256

  // Counts object in/out events.
  std::atomic_uint32_t _object_epoch{0};

  // Cached approximation of "cache_size in objects".
  std::atomic_uint32_t _cached_cache_size_objects{0};
};

}  // namespace hyrise
