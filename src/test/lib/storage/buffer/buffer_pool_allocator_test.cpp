#include <algorithm>
#include <memory>
#include <vector>

#include "base_test.hpp"

#include <boost/container/vector.hpp>

#include "storage/buffer/buffer_pool_allocator.hpp"
#include "storage/buffer/buffer_pool_allocator_observer.hpp"

// For pmr_vector / pmr_string + pin guards in your codebase
#include "storage/buffer/memory_resource.hpp"
#include "storage/buffer/pin_guard.hpp"

namespace hyrise {

class BufferPoolAllocatorTest : public BaseTest {};


TEST_F(BufferPoolAllocatorTest, TestAllocateAndDeallocateIntVector) {
  auto allocator = BufferPoolAllocator<int>();

  auto data = std::make_unique<boost::container::vector<int, BufferPoolAllocator<int>>>(5, allocator);
  (*data)[0] = 1;
  (*data)[1] = 2;
  (*data)[2] = 3;
  (*data)[3] = 4;
  (*data)[4] = 5;

  EXPECT_EQ((*data)[0], 1);
  EXPECT_EQ((*data)[1], 2);
  EXPECT_EQ((*data)[2], 3);
  EXPECT_EQ((*data)[3], 4);
  EXPECT_EQ((*data)[4], 5);

  data = nullptr;
}

TEST_F(BufferPoolAllocatorTest, TestPolymorphism) {
  auto int_allocator = BufferPoolAllocator<int>();
  BufferPoolAllocator<float> other_allocator = int_allocator;

  auto ptr = other_allocator.allocate(1);

  static_assert(std::is_same_v<decltype(ptr), float*>, "allocate() should return float* for BufferPoolAllocator<float>");

  ASSERT_NE(ptr, nullptr);
  *ptr = 3.5f;
  EXPECT_FLOAT_EQ(*ptr, 3.5f);

  other_allocator.deallocate(ptr, 1);
}

TEST_F(BufferPoolAllocatorTest, TestAllocateAndDeallocateStringVector) {
  auto& linear_resource = LinearBufferResource::get();

  auto allocator = BufferPoolAllocator<pmr_string>(&linear_resource);

  auto vector = pmr_vector<pmr_string>(140, allocator);

  {
    // Pin the vector pages while writing
    UnsafeSharedWritePinGuard guard{vector};

    vector[0] = "Hello";
    vector[1] = "World";
    vector[2] = "Hallo World with a really long string so that we reach the limit of SSO";
  }

  EXPECT_EQ(vector[0], "Hello");
  EXPECT_EQ(vector[1], "World");
  EXPECT_EQ(vector[2], "Hallo World with a really long string so that we reach the limit of SSO");
  linear_resource.reset();
}

TEST_F(BufferPoolAllocatorTest, TestObserver) {
  struct TestObserver : public BufferPoolAllocatorObserver {
    void on_allocate(const void* ptr) override { allocated_ptrs.push_back(ptr); }

    void on_deallocate(const void* ptr) override {
      allocated_ptrs.erase(std::remove(allocated_ptrs.begin(), allocated_ptrs.end(), ptr), allocated_ptrs.end());
    }

    std::vector<const void*> allocated_ptrs;
  };

  auto observer = std::make_shared<TestObserver>();

  auto allocator = BufferPoolAllocator<size_t>();
  allocator.register_observer(observer);

  EXPECT_EQ(observer->allocated_ptrs.size(), 0u);

  auto* ptr = allocator.allocate(1);
  ASSERT_NE(ptr, nullptr);

  ASSERT_EQ(observer->allocated_ptrs.size(), 1u);
  EXPECT_EQ(observer->allocated_ptrs[0], static_cast<const void*>(ptr));

  allocator.deallocate(ptr, 1);
  EXPECT_EQ(observer->allocated_ptrs.size(), 0u);
}

TEST_F(BufferPoolAllocatorTest, TestConstructorWithMemoryResource) {
  class LogResource final : public boost::container::pmr::memory_resource {
   public:
    struct Entry {
      void* ptr;
      std::size_t bytes;
      std::size_t alignment;
    };

    std::vector<Entry> allocations;

   private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
      void* p = ::operator new(bytes, std::align_val_t(alignment));
      allocations.push_back(Entry{p, bytes, alignment});
      return p;
    }

    void do_deallocate(void* p, std::size_t /*bytes*/, std::size_t alignment) override {
      allocations.erase(std::remove_if(allocations.begin(), allocations.end(),
                                       [&](const Entry& e) { return e.ptr == p; }),
                        allocations.end());

      ::operator delete(p, std::align_val_t(alignment));
    }

    bool do_is_equal(const boost::container::pmr::memory_resource& other) const noexcept override {
      return this == &other;
    }
  };

  auto test_resource = LogResource{};
  auto allocator = BufferPoolAllocator<size_t>(&test_resource);

  EXPECT_EQ(test_resource.allocations.size(), 0u);

  // BufferPoolAllocator allocates sizeof(T)*n with alignment alignof(T)
  auto* ptr = allocator.allocate(16);
  ASSERT_NE(ptr, nullptr);

  ASSERT_EQ(test_resource.allocations.size(), 1u);
  EXPECT_EQ(test_resource.allocations[0].ptr, static_cast<void*>(ptr));
  EXPECT_EQ(test_resource.allocations[0].bytes, sizeof(size_t) * 16);
  EXPECT_EQ(test_resource.allocations[0].alignment, alignof(size_t));

  allocator.deallocate(ptr, 16);
  EXPECT_EQ(test_resource.allocations.size(), 0u);
}

}  // namespace hyrise
