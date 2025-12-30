#pragma once

#include <boost/container/pmr/memory_resource.hpp>
#include <boost/move/utility.hpp>
#include "storage/buffer/buffer_pool_allocator_observer.hpp"

#include "utils/assert.hpp"

#ifndef NDEBUG
#include <iostream>
#include <type_traits>
#endif

#ifndef NDEBUG
#include <fstream>
#include <sstream>
#include <string>
#endif

namespace hyrise {

#ifndef NDEBUG
inline std::string debug_prot_of_addr(void* addr) {
  std::ifstream maps("/proc/self/maps");
  std::string line;
  const auto a = reinterpret_cast<uintptr_t>(addr);

  while (std::getline(maps, line)) {
    std::istringstream iss(line);
    std::string range, perms;
    if (!(iss >> range >> perms)) continue;

    const auto dash = range.find('-');
    if (dash == std::string::npos) continue;

    const auto start = std::stoull(range.substr(0, dash), nullptr, 16);
    const auto end   = std::stoull(range.substr(dash + 1), nullptr, 16);

    if (a >= start && a < end) return perms;   // e.g. "rw-p" or "---p"
  }
  return "<not-mapped>";
}
#endif

/**^
 * The BufferPoolAllocator is a custom, polymorphic allocator that uses the BufferManager to allocate and deallocate pages.
 *
 * TODO: Combine this allocator with scoped allocator to use same page like monotonic buffer resource for strings
*/
template <class T>
class BufferPoolAllocator {
 public:
  using value_type = T;

  BufferPoolAllocator() : _memory_resource(boost::container::pmr::new_delete_resource()) {
    DebugAssert(_memory_resource != nullptr, "_memory_resource is empty");
  }

  BufferPoolAllocator(boost::container::pmr::memory_resource* memory_resource,
                      std::shared_ptr<BufferPoolAllocatorObserver> observer = nullptr)
      : _memory_resource(memory_resource), _observer(observer) {
    DebugAssert(_memory_resource != nullptr, "_memory_resource is empty");
  }

  BufferPoolAllocator(const BufferPoolAllocator& other) noexcept {
    _memory_resource = other.memory_resource();
    _observer = other.current_observer();
    DebugAssert(_memory_resource != nullptr, "_memory_resource is empty");
  }

  template <class U>
  BufferPoolAllocator(const BufferPoolAllocator<U>& other) noexcept {
    _memory_resource = other.memory_resource();
    _observer = other.current_observer();
    DebugAssert(_memory_resource != nullptr, "_memory_resource is empty");
  }

  BufferPoolAllocator& operator=(const BufferPoolAllocator& other) noexcept {
    _memory_resource = other.memory_resource();
    _observer = other.current_observer();
    DebugAssert(_memory_resource != nullptr, "_memory_resource is empty");
    return *this;
  }

  template <class U>
  bool operator==(const BufferPoolAllocator<U>& other) const noexcept {
    return _memory_resource == other.memory_resource() && _observer.lock() == other.current_observer().lock();
  }

  template <class U>
  bool operator!=(const BufferPoolAllocator<U>& other) const noexcept {
    return _memory_resource != other.memory_resource() || _observer.lock() != other.current_observer().lock();
  }

  [[nodiscard]] T* allocate(std::size_t n) {
    auto* ptr = _memory_resource->allocate(sizeof(value_type) * n, alignof(T));

#ifndef NDEBUG
    // Keep noise low
    if constexpr (std::is_same_v<T, int>) {
      std::cerr << "[ALLOC int] ptr=" << ptr
                << " n=" << n
                << " bytes=" << (sizeof(value_type) * n)
                << " align=" << alignof(T)
                << " mr=" << static_cast<const void*>(_memory_resource)
                << "\n";
    }
#endif

#ifndef NDEBUG
    if constexpr (std::is_same_v<T, int>) {
      if (n == 8192) {
        std::cerr << "[ALLOC int] perms=" << debug_prot_of_addr(ptr) << "\n";
      }
    }
#endif

    if (auto observer = _observer.lock()) {
      observer->on_allocate(ptr);
    }
    return static_cast<T*>(ptr);
  }

  void deallocate(T* ptr, std::size_t n) {
    if (auto observer = _observer.lock()) {
      observer->on_deallocate(ptr);
    }
    _memory_resource->deallocate(ptr, sizeof(value_type) * n, alignof(T));
  }

  boost::container::pmr::memory_resource* memory_resource() const noexcept {
    return _memory_resource;
  }

  BufferPoolAllocator select_on_container_copy_construction() const noexcept {
    DebugAssert(_memory_resource != nullptr, "_memory_resource is empty");
    return BufferPoolAllocator(_memory_resource, _observer.lock());
  }

  void register_observer(std::shared_ptr<BufferPoolAllocatorObserver> observer) {
    if (!_observer.expired()) {
      Fail("An observer is already registered");
    }
    _observer = observer;
  }

  std::weak_ptr<BufferPoolAllocatorObserver> current_observer() const {
    return _observer;
  }

 private:
  boost::container::pmr::memory_resource* _memory_resource;
  std::weak_ptr<BufferPoolAllocatorObserver> _observer;
};

}  // namespace hyrise
