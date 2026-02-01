#pragma once

#include <tbb/concurrent_queue.h>

#include <bit>
#include <chrono>
#include <iostream>
#include <limits>
#include <magic_enum.hpp>

#include "boost/integer/static_log2.hpp"
#include "frame.hpp"
#include "metrics.hpp"
#include "strong_typedef.hpp"
#include "utils/assert.hpp"

#if HYRISE_NUMA_SUPPORT
#include <numa.h>
#endif

namespace hyrise {

inline std::size_t get_os_page_size() {
  return std::size_t(sysconf(_SC_PAGESIZE));
}

// TODO: Which sizes are useful?
#ifdef __APPLE__
constexpr size_t OS_PAGE_SIZE = 16384;
enum class PageSizeType { KiB16, KiB32, KiB64, KiB128, KiB256, KiB512, MiB1, MiB2, MiB4, MiB8 };
#elif __linux__
constexpr size_t OS_PAGE_SIZE = 4096;
enum class PageSizeType { KiB4, KiB8, KiB16, KiB32, KiB64, KiB128, KiB256, KiB512, MiB1, MiB2, MiB4, MiB8 };
#endif

// Get the number of bytes for a given PageSizeType
constexpr inline size_t bytes_for_size_type(const PageSizeType size) {
  return OS_PAGE_SIZE << static_cast<size_t>(size);
}

// Find the smallest PageSizeType that can hold the given bytes
constexpr PageSizeType find_fitting_page_size_type(const std::size_t bytes) {
  for (auto page_size_type : magic_enum::enum_values<PageSizeType>()) {
    if (bytes <= bytes_for_size_type(page_size_type)) {
      return page_size_type;
    }
  }
  Fail("Cannot fit value of " + std::to_string(bytes) + " bytes to a PageSizeType");
}

constexpr size_t NUM_PAGE_SIZE_TYPES = magic_enum::enum_count<PageSizeType>();

constexpr PageSizeType MIN_PAGE_SIZE_TYPE = magic_enum::enum_value<PageSizeType>(0);
constexpr PageSizeType MAX_PAGE_SIZE_TYPE = magic_enum::enum_value<PageSizeType>(NUM_PAGE_SIZE_TYPES - 1);
constexpr size_t PAGE_SIZE_TYPE_BITS = boost::static_log2<NUM_PAGE_SIZE_TYPES>::value + 1;

struct PageID {
  using PageIDType = uint64_t;
  PageIDType _valid : 1;
  PageIDType _size_type : PAGE_SIZE_TYPE_BITS;
  PageIDType index : (sizeof(PageIDType) * CHAR_BIT - PAGE_SIZE_TYPE_BITS - 1);

  PageSizeType size_type() const {
    return magic_enum::enum_value<PageSizeType>(this->_size_type);
  }

  size_t num_bytes() const {
    return bytes_for_size_type(size_type());
  }

  bool valid() const {
    return _valid;
  }

  auto operator<=>(const PageID&) const = default;

  PageID() = default;

  constexpr PageID(const PageSizeType size_type, const PageIDType index, bool valid = true)
      : _valid(valid), _size_type(static_cast<PageIDType>(size_type)), index(index) {}
};

inline std::ostream& operator<<(std::ostream& os, const PageID& page_id) {
  os << "PageID(valid=" << page_id.valid() << ", size_type=" << magic_enum::enum_name(page_id.size_type())
     << ", index=" << page_id.index << ")";
  return os;
}

static constexpr PageID INVALID_PAGE_ID = PageID{MIN_PAGE_SIZE_TYPE, 0, false};

// Pages need to be aligned to 512 in order to be used with O_DIRECT
constexpr size_t PAGE_ALIGNMENT = 512;

constexpr size_t MAX_REPEAT_COUNT = 100;

// How often old items should be evicted from the eviction queue
constexpr static std::chrono::milliseconds IDLE_EVICTION_QUEUE_PURGE = std::chrono::milliseconds(1000);

// Item for the Eviction Queue
struct EvictionItem {
  // The page to be evicted.
  PageID page_id;

  // Timestamp is the masked version (Frame::version) at enqueue time.
  uint64_t timestamp;

  // "can_mark" means "still the same frame generation and currently not exclusively locked"
  // "can_evict" means "valid + unlocked-like + not referenced"
  bool can_evict(Frame::StateVersionType state_and_version) const;

  // Check if the given frame can be marked for eviction
  bool can_mark(Frame::StateVersionType state_and_version) const;
};

using EvictionQueue = tbb::concurrent_queue<EvictionItem>;

// Enable or or disable mprotect calls for debugging purposes
constexpr bool ENABLE_MPROTECT = false;
constexpr size_t MAX_EVICTION_QUEUE_PURGES = 1024;

constexpr size_t DEFAULT_RESERVED_VIRTUAL_MEMORY = 1UL << 38;  // 256 GiB

constexpr size_t DEFAULT_RESERVED_VIRTUAL_MEMORY_PER_REGION =
    (DEFAULT_RESERVED_VIRTUAL_MEMORY / NUM_PAGE_SIZE_TYPES) / bytes_for_size_type(MAX_PAGE_SIZE_TYPE) *
    bytes_for_size_type(MAX_PAGE_SIZE_TYPE);

constexpr size_t INITIAL_SLOTS_PER_REGION = 10000000;

// Hints the buffer manager about the access intent of the caller. This influences the migration strategy
enum class AccessIntent { Read, Write };

boost::container::pmr::memory_resource* get_buffer_manager_memory_resource();

static inline void require_aligned_or_fail(const void* p) {
  if ((reinterpret_cast<std::uintptr_t>(p) % PAGE_ALIGNMENT) != 0) {
    Fail("SSDRegion: buffer must be aligned to " + std::to_string(PAGE_ALIGNMENT) + " bytes");
  }
}

// Busy waiting with backoff
inline void yield(const size_t repeat) {
  if (repeat < 4) {
  } else if ((repeat < 32) || (repeat & 1)) {
    std::this_thread::yield();
  } else if (repeat < 1000000) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
  } else {
    Fail("Yield for too long. Something is blocking. Current state");
  }
};

std::byte* create_mapped_region();
class VolatileRegion;
struct BufferManagerMetrics;
std::array<std::shared_ptr<VolatileRegion>, NUM_PAGE_SIZE_TYPES> create_volatile_regions(
    std::byte* mapped_region, std::shared_ptr<BufferManagerMetrics> metrics);

void unmap_region(std::byte* region);

}  // namespace hyrise
