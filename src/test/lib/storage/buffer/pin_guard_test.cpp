#include <memory>

#include "base_test.hpp"

#include <boost/container/vector.hpp>

#include "storage/buffer/buffer_manager.hpp"
#include "storage/buffer/buffer_pool_allocator.hpp"
#include "storage/buffer/frame.hpp"
#include "storage/buffer/helper.hpp"
#include "storage/buffer/pin_guard.hpp"
#include "types.hpp"

namespace hyrise {

class PinGuardTest : public BaseTest {};

namespace {

static bool is_pinned(const PageID page_id) {
  const auto state = BufferManager::get()._state(page_id);
  return state != Frame::UNLOCKED && state != Frame::EVICTED;
}

static bool is_unlocked(const PageID page_id) {
  const auto state = BufferManager::get()._state(page_id);
  return state == Frame::UNLOCKED;
}

}  // namespace

TEST_F(PinGuardTest, TestAllocatorPinGuardPinsAllocationAndUnpinsOnDestruction) {
  auto allocator = BufferPoolAllocator<std::byte>{get_buffer_manager_memory_resource()};

  const auto size = bytes_for_size_type(PageSizeType::KiB4);
  auto vector =
      std::make_unique<boost::container::vector<std::byte, BufferPoolAllocator<std::byte>>>(size, allocator);

  const auto page_id = BufferManager::get().find_page(vector->data());
  ASSERT_TRUE(page_id.valid());
  EXPECT_TRUE(is_unlocked(page_id));  // nothing pinned yet

  {
    auto pin_guard = std::make_unique<AllocatorPinGuard>(allocator);

    // Allocate another page while the guard is alive
    auto other =
        std::make_unique<boost::container::vector<std::byte, BufferPoolAllocator<std::byte>>>(size, allocator);
    const auto other_page_id = BufferManager::get().find_page(other->data());
    ASSERT_TRUE(other_page_id.valid());

    EXPECT_TRUE(is_pinned(other_page_id));

    pin_guard = nullptr;

    EXPECT_TRUE(is_unlocked(other_page_id));
  }

  vector = nullptr;
}

TEST_F(PinGuardTest, TestSharedReadPinGuardPinsAndUnpins) {
  auto allocator = BufferPoolAllocator<std::byte>{get_buffer_manager_memory_resource()};
  auto vector = boost::container::vector<std::byte, BufferPoolAllocator<std::byte>>{bytes_for_size_type(PageSizeType::KiB4),
                                                                                    allocator};

  const auto page_id = BufferManager::get().find_page(vector.data());
  ASSERT_TRUE(page_id.valid());

  EXPECT_TRUE(is_unlocked(page_id));

  {
    auto pin_guard = SharedReadPinGuard{vector};
    EXPECT_TRUE(is_pinned(page_id));
  }

  EXPECT_TRUE(is_unlocked(page_id));
}

TEST_F(PinGuardTest, TestUnsafeSharedWritePinGuardPinsAndUnpins) {
  auto allocator = BufferPoolAllocator<std::byte>{get_buffer_manager_memory_resource()};
  auto vector = boost::container::vector<std::byte, BufferPoolAllocator<std::byte>>{bytes_for_size_type(PageSizeType::KiB4),
                                                                                    allocator};

  const auto page_id = BufferManager::get().find_page(vector.data());
  ASSERT_TRUE(page_id.valid());

  EXPECT_TRUE(is_unlocked(page_id));

  {
    auto pin_guard = UnsafeSharedWritePinGuard{vector};
    EXPECT_TRUE(is_pinned(page_id));
  }

  EXPECT_TRUE(is_unlocked(page_id));
}

TEST_F(PinGuardTest, TestSharedReadPinGuardPinsVectorAndStringPages) {
  auto allocator = BufferPoolAllocator<pmr_string>{get_buffer_manager_memory_resource()};
  auto vector = pmr_vector<pmr_string>{2, allocator};

  // Make sure strings are long enough to avoid SSO so they allocate memory (and thus can belong to BM pages).
  vector[0] = "Hello hello hello hello hello hello hello hello hello hello hello hello hello hello";
  vector[1] = "World world world world world world world world world world world world world world";

  const auto vec_page_id = BufferManager::get().find_page(vector.data());
  ASSERT_TRUE(vec_page_id.valid());

  const auto s0_page_id = BufferManager::get().find_page(vector[0].data());
  const auto s1_page_id = BufferManager::get().find_page(vector[1].data());

  EXPECT_TRUE(is_unlocked(vec_page_id));
  if (s0_page_id.valid()) EXPECT_TRUE(is_unlocked(s0_page_id));
  if (s1_page_id.valid()) EXPECT_TRUE(is_unlocked(s1_page_id));

  {
    auto pin_guard = SharedReadPinGuard{vector};

    EXPECT_TRUE(is_pinned(vec_page_id));
    if (s0_page_id.valid()) EXPECT_TRUE(is_pinned(s0_page_id));
    if (s1_page_id.valid()) EXPECT_TRUE(is_pinned(s1_page_id));
  }

  EXPECT_TRUE(is_unlocked(vec_page_id));
  if (s0_page_id.valid()) EXPECT_TRUE(is_unlocked(s0_page_id));
  if (s1_page_id.valid()) EXPECT_TRUE(is_unlocked(s1_page_id));
}

}  // namespace hyrise
