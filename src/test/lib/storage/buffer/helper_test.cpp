#include <cstddef>
#include <filesystem>
#include <memory>
#include <system_error>

#include "base_test.hpp"

#include "storage/buffer/helper.hpp"
#include "storage/buffer/metrics.hpp"
#include "storage/buffer/volatile_region.hpp"

namespace hyrise {

class HelperTest : public BaseTest {};

namespace {

struct HelperTestContext {
  std::byte* mapped_region{};
  std::shared_ptr<BufferManagerMetrics> metrics;
  std::array<std::shared_ptr<VolatileRegion>, NUM_PAGE_SIZE_TYPES> regions;

  HelperTestContext()
      : mapped_region(create_mapped_region()),
        metrics(std::make_shared<BufferManagerMetrics>()),
        regions(create_volatile_regions(mapped_region, metrics)) {}

  ~HelperTestContext() {
    if (mapped_region) {
      unmap_region(mapped_region);
      mapped_region = nullptr;
    }
  }

  std::shared_ptr<VolatileRegion> region_for(const PageSizeType size_type) {
    return regions[static_cast<size_t>(size_type)];
  }
};

}  // namespace

TEST_F(HelperTest, EvictionItemCanMarkAndCanEvictMatchStateAndVersion) {
  Frame frame;

  // Bring frame to UNLOCKED and bump version once:
  auto sv = frame.state_and_version();
  ASSERT_TRUE(frame.try_lock_exclusive(sv));
  frame.unlock_exclusive();

  const auto unlocked_sv = frame.state_and_version();
  ASSERT_EQ(Frame::state(unlocked_sv), Frame::UNLOCKED);

  const auto ts = Frame::version(unlocked_sv);

  const EvictionItem item{PageID{PageSizeType::KiB4, 0}, ts};

  // UNLOCKED + matching version => markable
  EXPECT_TRUE(item.can_mark(unlocked_sv));
  EXPECT_FALSE(item.can_evict(unlocked_sv));

  // MARKED + matching version => evictable
  auto sv2 = frame.state_and_version();
  ASSERT_TRUE(frame.try_mark(sv2));
  const auto marked_sv = frame.state_and_version();
  ASSERT_EQ(Frame::state(marked_sv), Frame::MARKED);

  EXPECT_FALSE(item.can_mark(marked_sv));
  EXPECT_TRUE(item.can_evict(marked_sv));

  // Version mismatch => neither markable nor evictable
  const EvictionItem wrong_ts{PageID{PageSizeType::KiB4, 0}, ts + 1};
  EXPECT_FALSE(wrong_ts.can_mark(unlocked_sv));
  EXPECT_FALSE(wrong_ts.can_evict(marked_sv));
}

TEST_F(HelperTest, CreateMappedRegionReturnsAlignedPointerAndCanBeUnmapped) {
  auto* region = create_mapped_region();
  ASSERT_NE(region, nullptr);

  // create_mapped_region aligns to bytes_for_size_type(MAX_PAGE_SIZE_TYPE)
  const auto align = bytes_for_size_type(MAX_PAGE_SIZE_TYPE);
  const auto addr = reinterpret_cast<std::uintptr_t>(region);
  EXPECT_EQ(addr % align, 0u);

  // Also must satisfy PAGE_ALIGNMENT invariants
  EXPECT_EQ(addr % PAGE_ALIGNMENT, 0u);

  unmap_region(region);
}

TEST_F(HelperTest, CreateVolatileRegionsAllocateReturnCorrectSizeTypeAndAlignedPtr) {
  HelperTestContext ctx;

  // Pick two different size types to sanity-check.
  for (const auto size_type : {MIN_PAGE_SIZE_TYPE, PageSizeType::KiB32}) {
    const auto region = ctx.region_for(size_type);
    ASSERT_NE(region, nullptr);

    const auto [page_id, frame, ptr] = region->allocate();
    ASSERT_TRUE(page_id.valid());
    ASSERT_NE(frame, nullptr);
    ASSERT_NE(ptr, nullptr);

    EXPECT_EQ(page_id.size_type(), size_type);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(ptr) % PAGE_ALIGNMENT, 0u);

    region->deallocate(page_id);
  }
}

}  // namespace hyrise
