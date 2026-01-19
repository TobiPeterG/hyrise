#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

#include "base_test.hpp"

#include "storage/buffer/buffer_manager.hpp"
#include "storage/buffer/helper.hpp"
#include "storage/buffer/memory_resource.hpp"

namespace hyrise {

class LinearBufferResourceTest : public BaseTest {};

TEST_F(LinearBufferResourceTest, TestConstructorAndReset) {
  auto& resource = LinearBufferResource::get();

  // Reset should clear the thread-local state
  resource.reset();
  EXPECT_EQ(resource.remaining_storage(), 0u);

  // First small allocation should create a new internal page and reduce remaining by 1
  const auto page_bytes = bytes_for_size_type(LinearBufferResource::PAGE_SIZE_TYPE);

  auto* first_ptr = resource.allocate(1u, 1u);
  ASSERT_NE(first_ptr, nullptr);
  EXPECT_EQ(resource.remaining_storage(), page_bytes - 1u);

  // Reset clears bookkeeping (does not free the BM allocation by design)
  resource.reset();
  EXPECT_EQ(resource.remaining_storage(), 0u);

  // TODO: We need a cleaner way
  BufferManager::get().deallocate(first_ptr, page_bytes, alignof(std::max_align_t));
}

TEST_F(LinearBufferResourceTest, TestRemainingStorageAndAlignmentWaste) {
  auto& resource = LinearBufferResource::get();
  resource.reset();

  const auto page_bytes = bytes_for_size_type(LinearBufferResource::PAGE_SIZE_TYPE);

  // First allocation: consumes 1 byte at offset 0
  auto* page_base = resource.allocate(1u, 1u);
  ASSERT_NE(page_base, nullptr);
  EXPECT_EQ(resource.remaining_storage(), page_bytes - 1u);

  // With current position == 1, requesting 4-byte alignment should waste 3 bytes
  std::size_t wasted = 0;
  (void)resource.remaining_storage(4u, wasted);
  EXPECT_EQ(wasted, 3u);

  // Allocate 4 bytes aligned to 4 -> consumes wasted(3) + bytes(4)
  auto* p = resource.allocate(4u, 4u);
  ASSERT_NE(p, nullptr);

  // Remaining should have decreased by 7 total compared to after first allocation
  EXPECT_EQ(resource.remaining_storage(), page_bytes - 1u - 3u - 4u);

  // Clean up
  resource.reset();
  // TODO: We need a cleaner way
  BufferManager::get().deallocate(page_base, page_bytes, alignof(std::max_align_t));
}

TEST_F(LinearBufferResourceTest, TestDeallocateIsNoOpForSubAllocations) {
  auto& resource = LinearBufferResource::get();
  resource.reset();

  const auto page_bytes = bytes_for_size_type(LinearBufferResource::PAGE_SIZE_TYPE);

  // First small alloc -> creates a page; ptr is page base
  auto* page_base = resource.allocate(8u, 1u);
  ASSERT_NE(page_base, nullptr);

  const auto remaining_before = resource.remaining_storage();

  // A second small alloc will be a sub-allocation from the current page
  auto* sub_ptr = resource.allocate(16u, 1u);
  ASSERT_NE(sub_ptr, nullptr);

  const auto remaining_after_alloc = resource.remaining_storage();
  EXPECT_LT(remaining_after_alloc, remaining_before);

  // Deallocate of sub-allocation should be a no-op
  // TODO: We need a centralized way to deallocate these pages
  resource.deallocate(sub_ptr, 16u, 1u);
  EXPECT_EQ(resource.remaining_storage(), remaining_after_alloc);

  // Clean up
  resource.reset();
  // TODO: We need a cleaner way
  BufferManager::get().deallocate(page_base, page_bytes, alignof(std::max_align_t));
}

TEST_F(LinearBufferResourceTest, TestDirectAllocationsAreFreed) {
  auto& resource = LinearBufferResource::get();
  resource.reset();

  // Pick a size that triggers fills_page():
  // bytes_for_size_type(KiB64) * 0.85 exceeds the 80% threshold of the fitting page size type.
  const auto big_bytes = static_cast<std::size_t>(bytes_for_size_type(PageSizeType::KiB64) * 0.85);

  auto metrics = BufferManager::get().metrics();
  const auto allocs_before = metrics->num_allocs.load(std::memory_order_relaxed);
  const auto deallocs_before = metrics->num_deallocs.load(std::memory_order_relaxed);

  auto* big_ptr = resource.allocate(big_bytes, 8u);
  ASSERT_NE(big_ptr, nullptr);

  const auto allocs_after = metrics->num_allocs.load(std::memory_order_relaxed);
  EXPECT_GE(allocs_after, allocs_before + 1) << "Expected BufferManager allocation for fills_page() path";

  // This must free again
  resource.deallocate(big_ptr, big_bytes, 8u);

  const auto deallocs_after = metrics->num_deallocs.load(std::memory_order_relaxed);
  EXPECT_GE(deallocs_after, deallocs_before + 1) << "Expected BufferManager deallocation for fills_page() path";
}

}  // namespace hyrise
