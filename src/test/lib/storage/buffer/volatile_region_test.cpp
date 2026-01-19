#include <cstddef>
#include <cstring>
#include <memory>

#include "base_test.hpp"

#include "storage/buffer/helper.hpp"
#include "storage/buffer/metrics.hpp"
#include "storage/buffer/volatile_region.hpp"
#include "types.hpp"

#if HYRISE_NUMA_SUPPORT
#include <numa.h>
#endif

namespace hyrise {

class VolatileRegionTest : public BaseTest {};

namespace {

struct RegionContext {
  std::byte* mapped_region{};
  std::shared_ptr<BufferManagerMetrics> metrics;
  std::array<std::shared_ptr<VolatileRegion>, NUM_PAGE_SIZE_TYPES> regions;

  explicit RegionContext() : mapped_region(create_mapped_region()), metrics(std::make_shared<BufferManagerMetrics>()) {
    regions = create_volatile_regions(mapped_region, metrics);
  }

  ~RegionContext() {
    if (mapped_region) {
      unmap_region(mapped_region);
      mapped_region = nullptr;
    }
  }

  std::shared_ptr<VolatileRegion> region_for(const PageSizeType size_type) {
    return regions[static_cast<size_t>(size_type)];
  }
};

static void write_one_byte(std::byte* ptr) {
  // Use volatile to avoid the compiler optimizing the access away
  volatile std::byte* v = ptr;
  v[0] = std::byte{0xAB};
}

}  // namespace

TEST_F(VolatileRegionTest, TestAllocateDeallocateReusesSlotsDeterministically) {
  RegionContext ctx;

  const auto size_type = PageSizeType::KiB32;
  const auto region = ctx.region_for(size_type);

  // Allocate three pages; with a fresh region and find_first(), these should be indices 0,1,2.
  const auto [page_id_0, frame_0, ptr_0] = region->allocate();
  const auto [page_id_1, frame_1, ptr_1] = region->allocate();
  const auto [page_id_2, frame_2, ptr_2] = region->allocate();

  ASSERT_TRUE(page_id_0.valid());
  ASSERT_TRUE(page_id_1.valid());
  ASSERT_TRUE(page_id_2.valid());

  EXPECT_EQ(page_id_0.size_type(), size_type);
  EXPECT_EQ(page_id_1.size_type(), size_type);
  EXPECT_EQ(page_id_2.size_type(), size_type);

  EXPECT_EQ(page_id_0.index, 0u);
  EXPECT_EQ(page_id_1.index, 1u);
  EXPECT_EQ(page_id_2.index, 2u);

  EXPECT_NE(ptr_0, nullptr);
  EXPECT_NE(ptr_1, nullptr);
  EXPECT_NE(ptr_2, nullptr);
  EXPECT_NE(ptr_0, ptr_1);
  EXPECT_NE(ptr_0, ptr_2);
  EXPECT_NE(ptr_1, ptr_2);

  // Touch memory to ensure it's writable after allocate().
  EXPECT_NO_THROW(std::memset(ptr_0, 0x11, page_id_0.num_bytes()));
  EXPECT_NO_THROW(std::memset(ptr_1, 0x22, page_id_1.num_bytes()));
  EXPECT_NO_THROW(std::memset(ptr_2, 0x33, page_id_2.num_bytes()));

  // Deallocate two pages in a non-sorted order (2 and 0).
  region->deallocate(page_id_2);
  region->deallocate(page_id_0);

  // Next allocations should return the smallest free index first (0), then (2).
  const auto [page_id_a, frame_a, ptr_a] = region->allocate();
  const auto [page_id_b, frame_b, ptr_b] = region->allocate();

  EXPECT_EQ(page_id_a.index, 0u);
  EXPECT_EQ(page_id_b.index, 2u);
  EXPECT_NE(ptr_a, nullptr);
  EXPECT_NE(ptr_b, nullptr);

  // Cleanup remaining allocations.
  region->deallocate(page_id_1);
  region->deallocate(page_id_a);
  region->deallocate(page_id_b);

  (void)frame_0;
  (void)frame_1;
  (void)frame_2;
  (void)frame_a;
  (void)frame_b;
}

TEST_F(VolatileRegionTest, TestFreeIncrementsMetricsAndProtectsAgain) {
  RegionContext ctx;

  const auto size_type = PageSizeType::KiB32;
  const auto region = ctx.region_for(size_type);

  const auto before = ctx.metrics->num_madvice_free_calls.load(std::memory_order_relaxed);

  const auto [page_id, frame, ptr] = region->allocate();
  ASSERT_TRUE(page_id.valid());
  ASSERT_NE(ptr, nullptr);

  // Make sure page is writable now.
  EXPECT_NO_THROW(write_one_byte(ptr));

  // free() should madvise + protect again, and increment metrics.
  region->free(page_id);

  const auto after = ctx.metrics->num_madvice_free_calls.load(std::memory_order_relaxed);
  EXPECT_EQ(after, before + 1);

  // After free(), the page should be protected again (PROT_NONE) when ENABLE_MPROTECT is true.
#if ENABLE_MPROTECT && GTEST_HAS_DEATH_TEST
  ASSERT_DEATH_IF_SUPPORTED(
      {
        write_one_byte(ptr);
      },
      "");
#endif

  // Return slot.
  region->deallocate(page_id);

  (void)frame;
}

TEST_F(VolatileRegionTest, TestDeallocateProtectsAgain) {
  RegionContext ctx;

  const auto size_type = PageSizeType::KiB32;
  const auto region = ctx.region_for(size_type);

  const auto [page_id, frame, ptr] = region->allocate();
  ASSERT_TRUE(page_id.valid());
  ASSERT_NE(ptr, nullptr);

  // Writable after allocate.
  EXPECT_NO_THROW(write_one_byte(ptr));

  region->deallocate(page_id);

#if ENABLE_MPROTECT && GTEST_HAS_DEATH_TEST
  ASSERT_DEATH_IF_SUPPORTED(
      {
        write_one_byte(ptr);
      },
      "");
#endif

  (void)frame;
}

TEST_F(VolatileRegionTest, TestMbindUpdatesFrameNodeIdWhenLocked) {
#if !HYRISE_NUMA_SUPPORT
  GTEST_SKIP() << "NUMA support not compiled in";
#endif

#if HYRISE_NUMA_SUPPORT
  if (numa_available() < 0) {
    GTEST_SKIP() << "NUMA not available at runtime";
  }
  if (numa_max_node() < 1) {
    GTEST_SKIP() << "Need at least 2 NUMA nodes to run this test (nodes 0 and 1)";
  }

  RegionContext ctx;

  const auto size_type = PageSizeType::KiB32;
  const auto region = ctx.region_for(size_type);

  const auto before = ctx.metrics->num_numa_tonode_memory_calls.load(std::memory_order_relaxed);

  const auto [page_id, frame, ptr] = region->allocate();
  ASSERT_TRUE(page_id.valid());
  ASSERT_NE(frame, nullptr);
  ASSERT_NE(ptr, nullptr);

  auto state_and_version = frame->state_and_version();
  ASSERT_TRUE(frame->try_lock_exclusive(state_and_version));

  const auto target_node = NodeID{1};
  region->mbind_to_numa_node(page_id, target_node);

  EXPECT_EQ(frame->node_id(), target_node);

  frame->unlock_exclusive();

  const auto after = ctx.metrics->num_numa_tonode_memory_calls.load(std::memory_order_relaxed);
  EXPECT_EQ(after, before + 1);

  region->deallocate(page_id);
#endif
}

}  // namespace hyrise
