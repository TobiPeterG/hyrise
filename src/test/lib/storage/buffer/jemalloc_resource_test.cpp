#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include "base_test.hpp"

#include "storage/buffer/buffer_manager.hpp"
#include "storage/buffer/helper.hpp"
#include "storage/buffer/jemalloc_resource.hpp"

namespace hyrise {

class JemallocMemoryResourceTest : public BaseTest {};

#ifndef HYRISE_WITH_JEMALLOC

TEST_F(JemallocMemoryResourceTest, JemallocNotEnabled) {
  GTEST_SKIP() << "Built without HYRISE_WITH_JEMALLOC";
}

#else

namespace {

static bool is_aligned(const void* p, const std::size_t alignment) {
  return (reinterpret_cast<std::uintptr_t>(p) % alignment) == 0;
}

}  // namespace

TEST_F(JemallocMemoryResourceTest, TestAllocateDeallocateBasic) {
  auto& mr = JemallocMemoryResource::get();

  constexpr std::size_t alignment = 64;
  constexpr std::size_t bytes = 128;

  auto* p = mr.allocate(bytes, alignment);
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(is_aligned(p, alignment));

  // For small allocations, we expect memory to come from BM mapping via extent hooks.
  const auto page_id = BufferManager::get().find_page(p);
  EXPECT_TRUE(page_id.valid());

  std::memset(p, 0xAB, bytes);

  mr.deallocate(p, bytes, alignment);
}

TEST_F(JemallocMemoryResourceTest, TestMultipleAllocationsAndWrite) {
  auto& mr = JemallocMemoryResource::get();

  constexpr std::size_t a1 = 16;
  constexpr std::size_t a2 = 128;

  constexpr std::size_t b1 = 37;
  constexpr std::size_t b2 = 4096;

  void* p1 = mr.allocate(b1, a1);
  void* p2 = mr.allocate(b2, a2);

  ASSERT_NE(p1, nullptr);
  ASSERT_NE(p2, nullptr);
  EXPECT_TRUE(is_aligned(p1, a1));
  EXPECT_TRUE(is_aligned(p2, a2));

  EXPECT_TRUE(BufferManager::get().find_page(p1).valid());
  EXPECT_TRUE(BufferManager::get().find_page(p2).valid());

  std::memset(p1, 0x11, b1);
  std::memset(p2, 0x22, b2);

  mr.deallocate(p2, b2, a2);
  mr.deallocate(p1, b1, a1);
}

TEST_F(JemallocMemoryResourceTest, TestIsEqual) {
  auto& mr = JemallocMemoryResource::get();
  EXPECT_TRUE(mr.is_equal(mr));

  auto* other = boost::container::pmr::new_delete_resource();
  EXPECT_FALSE(mr.is_equal(*other));
}

TEST_F(JemallocMemoryResourceTest, TestResetWithNoOutstandingAllocations) {
  auto& mr = JemallocMemoryResource::get();

  // Ensure we don't keep any outstanding allocations.
  {
    constexpr std::size_t alignment = 64;
    constexpr std::size_t bytes = 256;
    void* p = mr.allocate(bytes, alignment);
    ASSERT_NE(p, nullptr);
    mr.deallocate(p, bytes, alignment);
  }

  EXPECT_NO_THROW(mr.reset());
}

TEST_F(JemallocMemoryResourceTest, TestDrainDeferredBMFreesDoesNotCrash) {
  auto& mr = JemallocMemoryResource::get();

  // This should be safe to call even if nothing is pending
  EXPECT_NO_THROW(mr.drain_deferred_bm_frees());
}

#endif  // HYRISE_WITH_JEMALLOC

}  // namespace hyrise
