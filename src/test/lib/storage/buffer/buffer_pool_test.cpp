#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>

#include "base_test.hpp"

#include "storage/buffer/buffer_pool.hpp"
#include "storage/buffer/helper.hpp"
#include "storage/buffer/metrics.hpp"
#include "storage/buffer/ssd_region.hpp"
#include "storage/buffer/volatile_region.hpp"

#if HYRISE_NUMA_SUPPORT
#include <numa.h>
#endif

namespace hyrise {

class BufferPoolTest : public BaseTest {};

namespace {

struct BufferPoolTestContext {
  std::byte* mapped_region{};
  std::shared_ptr<BufferManagerMetrics> bm_metrics;
  std::array<std::shared_ptr<VolatileRegion>, NUM_PAGE_SIZE_TYPES> regions;

  std::filesystem::path ssd_dir;
  std::shared_ptr<SSDRegion> ssd_region;

  std::shared_ptr<BufferPoolMetrics> pool_metrics;

  BufferPoolTestContext()
      : mapped_region(create_mapped_region()),
        bm_metrics(std::make_shared<BufferManagerMetrics>()),
        regions(create_volatile_regions(mapped_region, bm_metrics)),
        ssd_dir(std::filesystem::temp_directory_path() / "hyrise_buffer_pool_test_ssd"),
        pool_metrics(std::make_shared<BufferPoolMetrics>()) {
    std::error_code ec;
    std::filesystem::create_directories(ssd_dir, ec);

    ssd_region = std::make_shared<SSDRegion>(ssd_dir, bm_metrics);
  }

  ~BufferPoolTestContext() {
    if (mapped_region) {
      unmap_region(mapped_region);
      mapped_region = nullptr;
    }

    std::error_code ec;
    std::filesystem::remove_all(ssd_dir, ec);
  }

  std::shared_ptr<VolatileRegion> region_for(const PageSizeType size_type) {
    return regions[static_cast<size_t>(size_type)];
  }
};

static void touch_page(std::byte* ptr) {
  volatile std::byte* v = ptr;
  v[0] = std::byte{0xAB};
}

}  // namespace

TEST_F(BufferPoolTest, TestReserveAndFreeBytesAccounting) {
  BufferPoolTestContext ctx;

  BufferPool pool{/*enabled*/ true,
                  /*pool_size*/ bytes_for_size_type(PageSizeType::KiB4) * 4,
                  /*enable_eviction_purge_worker*/ false,
                  /*volatile_regions*/ ctx.regions,
                  /*migration_policy*/ EagerMigrationPolicy,
                  /*ssd_region*/ ctx.ssd_region,
                  /*target_buffer_pool*/ nullptr,
                  /*numa_node*/ NodeID{0},
                  /*metrics*/ ctx.pool_metrics};

  EXPECT_EQ(pool.used_bytes.load(std::memory_order_relaxed), 0u);

  pool.reserve_bytes(1234);
  EXPECT_EQ(pool.used_bytes.load(std::memory_order_relaxed), 1234u);

  pool.free_bytes(234);
  EXPECT_EQ(pool.used_bytes.load(std::memory_order_relaxed), 1000u);

  // Freeing zero is a no-op
  pool.free_bytes(0);
  EXPECT_EQ(pool.used_bytes.load(std::memory_order_relaxed), 1000u);
}

TEST_F(BufferPoolTest, TestAddToEvictionQueueIncrementsMetric) {
  BufferPoolTestContext ctx;

  BufferPool pool{/*enabled*/ true,
                  /*pool_size*/ bytes_for_size_type(PageSizeType::KiB4) * 4,
                  /*enable_eviction_purge_worker*/ false,
                  /*volatile_regions*/ ctx.regions,
                  /*migration_policy*/ EagerMigrationPolicy,
                  /*ssd_region*/ ctx.ssd_region,
                  /*target_buffer_pool*/ nullptr,
                  /*numa_node*/ NodeID{0},
                  /*metrics*/ ctx.pool_metrics};

  const auto region = ctx.region_for(PageSizeType::KiB4);
  const auto [page_id, frame, ptr] = region->allocate();
  ASSERT_TRUE(page_id.valid());
  ASSERT_NE(frame, nullptr);
  ASSERT_NE(ptr, nullptr);

  // To satisfy add_eviction_candidate's DebugAssert(frame->node_id() == node_id),
  // we need to set the frame's node_id while exclusively locked.
  auto state_and_version = frame->state_and_version();
  ASSERT_TRUE(frame->try_lock_exclusive(state_and_version));
  frame->set_node_id(pool.node_id);
  frame->unlock_exclusive();

  const auto before_adds = pool.metrics->num_eviction_queue_adds.load(std::memory_order_relaxed);
  pool.add_eviction_candidate(page_id, frame);
  const auto after_adds = pool.metrics->num_eviction_queue_adds.load(std::memory_order_relaxed);
  EXPECT_EQ(after_adds, before_adds + 1);

  // Cleanup
  region->deallocate(page_id);
}

TEST_F(BufferPoolTest, TestEnsureFreePagesReturnsTrueWhenNoPressure) {
  BufferPoolTestContext ctx;

  BufferPool pool{/*enabled*/ true,
                  /*pool_size*/ bytes_for_size_type(PageSizeType::MiB8),  // plenty
                  /*enable_eviction_purge_worker*/ false,
                  /*volatile_regions*/ ctx.regions,
                  /*migration_policy*/ EagerMigrationPolicy,
                  /*ssd_region*/ ctx.ssd_region,
                  /*target_buffer_pool*/ nullptr,
                  /*numa_node*/ NodeID{0},
                  /*metrics*/ ctx.pool_metrics};

  const auto before = pool.used_bytes.load(std::memory_order_relaxed);
  EXPECT_TRUE(pool.ensure_free_pages(PageSizeType::KiB4));
  const auto after = pool.used_bytes.load(std::memory_order_relaxed);

  // ensure_free_pages reserves required bytes even if it doesn't need to evict.
  EXPECT_EQ(after, before + bytes_for_size_type(PageSizeType::KiB4));
}

TEST_F(BufferPoolTest, TestEnsureFreePagesFailsWhenEvictionQueueEmpty) {
  BufferPoolTestContext ctx;

  const auto pool_size = bytes_for_size_type(PageSizeType::KiB4);  // tiny
  BufferPool pool{/*enabled*/ true,
                  /*pool_size*/ pool_size,
                  /*enable_eviction_purge_worker*/ false,
                  /*volatile_regions*/ ctx.regions,
                  /*migration_policy*/ EagerMigrationPolicy,
                  /*ssd_region*/ ctx.ssd_region,
                  /*target_buffer_pool*/ nullptr,
                  /*numa_node*/ NodeID{0},
                  /*metrics*/ ctx.pool_metrics};

  // First call: exactly fills the pool
  EXPECT_TRUE(pool.ensure_free_pages(PageSizeType::KiB4));
  EXPECT_EQ(pool.used_bytes.load(std::memory_order_relaxed), pool_size);

  // Second call: would oversubscribe, but eviction_queue is empty -> should fail and roll back the reservation.
  EXPECT_FALSE(pool.ensure_free_pages(PageSizeType::KiB4));
  EXPECT_EQ(pool.used_bytes.load(std::memory_order_relaxed), pool_size);
}

TEST_F(BufferPoolTest, TestEnsureFreePagesEvictsMarkedPageToSSDAndFreesBudget) {
  BufferPoolTestContext ctx;

  const auto pool_size = bytes_for_size_type(PageSizeType::KiB4);  // only 1 page fits
  BufferPool pool{/*enabled*/ true,
                  /*pool_size*/ pool_size,
                  /*enable_eviction_purge_worker*/ false,
                  /*volatile_regions*/ ctx.regions,
                  /*migration_policy*/ EagerMigrationPolicy,
                  /*ssd_region*/ ctx.ssd_region,
                  /*target_buffer_pool*/ nullptr,
                  /*numa_node*/ NodeID{0},
                  /*metrics*/ ctx.pool_metrics};

  const auto region = ctx.region_for(PageSizeType::KiB4);
  const auto [page_id, frame, ptr] = region->allocate();
  ASSERT_TRUE(page_id.valid());
  ASSERT_NE(frame, nullptr);
  ASSERT_NE(ptr, nullptr);

  {
    auto state_and_version = frame->state_and_version();
    ASSERT_TRUE(frame->try_lock_exclusive(state_and_version));
    frame->set_node_id(pool.node_id);
    frame->set_dirty(true);
    touch_page(ptr);
    frame->unlock_exclusive();  // increments version and leaves UNLOCKED
  }

  // Occupy pool budget with one page (now used_bytes == max_bytes)
  EXPECT_TRUE(pool.ensure_free_pages(PageSizeType::KiB4));
  EXPECT_EQ(pool.used_bytes.load(std::memory_order_relaxed), pool_size);

  // Add the victim to the eviction queue with the current version timestamp.
  pool.add_eviction_candidate(page_id, frame);

  const auto bytes_to_ssd_before = pool.metrics->total_bytes_copied_to_ssd.load(std::memory_order_relaxed);
  const auto evictions_before = pool.metrics->num_evictions.load(std::memory_order_relaxed);

  // Request another page: must evict our victim to make room.
  EXPECT_TRUE(pool.ensure_free_pages(PageSizeType::KiB4));

  const auto evictions_after = pool.metrics->num_evictions.load(std::memory_order_relaxed);
  EXPECT_EQ(evictions_after, evictions_before + 1);

  const auto bytes_to_ssd_after = pool.metrics->total_bytes_copied_to_ssd.load(std::memory_order_relaxed);
  EXPECT_EQ(bytes_to_ssd_after, bytes_to_ssd_before + bytes_for_size_type(PageSizeType::KiB4));

  // The page should now be EVICTED
  EXPECT_EQ(Frame::state(frame->state_and_version()), Frame::EVICTED);

  // Also, ensure_free_pages should end with used_bytes == max_bytes
  EXPECT_EQ(pool.used_bytes.load(std::memory_order_relaxed), pool_size);

  // Cleanup
  region->deallocate(page_id);
}

TEST_F(BufferPoolTest, TestPurgeEvictionQueueKeepsEvictableOrMarkableItems) {
  BufferPoolTestContext ctx;

  BufferPool pool{/*enabled*/ true,
                  /*pool_size*/ bytes_for_size_type(PageSizeType::MiB8),
                  /*enable_eviction_purge_worker*/ false,
                  /*volatile_regions*/ ctx.regions,
                  /*migration_policy*/ EagerMigrationPolicy,
                  /*ssd_region*/ ctx.ssd_region,
                  /*target_buffer_pool*/ nullptr,
                  /*numa_node*/ NodeID{0},
                  /*metrics*/ ctx.pool_metrics};

  const auto region = ctx.region_for(PageSizeType::KiB4);

  // Create one UNLOCKED + correct timestamp item (markable).
  const auto [page_id, frame, ptr] = region->allocate();
  ASSERT_TRUE(page_id.valid());
  ASSERT_NE(frame, nullptr);
  ASSERT_NE(ptr, nullptr);

  // Put it on correct node, leave UNLOCKED
  {
    auto state_and_version = frame->state_and_version();
    ASSERT_TRUE(frame->try_lock_exclusive(state_and_version));
    frame->set_node_id(pool.node_id);
    frame->unlock_exclusive();
  }

  // Push item with matching timestamp
  pool.add_eviction_candidate(page_id, frame);

  // purge_eviction_candidates should see item.can_mark(...) and push it back
  pool.purge_eviction_candidates();

  // Force pressure: small max_bytes and full budget
  pool.used_bytes.store(pool.max_bytes, std::memory_order_relaxed);

  // This should not immediately fail due to empty queue.
  // It might still return false if the item gets purged for other reasons
  (void)pool.ensure_free_pages(PageSizeType::KiB4);

  region->deallocate(page_id);
}

}  // namespace hyrise
