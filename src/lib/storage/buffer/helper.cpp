#include "storage/buffer/helper.hpp"

#include "storage/buffer/buffer_manager.hpp"
#include "storage/buffer/volatile_region.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <utility>

namespace hyrise {

bool EvictionItem::can_mark(Frame::StateVersionType state_and_version) const {
  // Candidate is still the same frame generation (version matches), and is currently not exclusively locked.
  // We require the frame to be UNLOCKED (pinned frames remain candidates but are skipped).
  if (Frame::state(state_and_version) != Frame::UNLOCKED) {
    return false;
  }
  return Frame::version(state_and_version) == timestamp;
}

bool EvictionItem::can_evict(Frame::StateVersionType state_and_version) const {
  if (!can_mark(state_and_version)) {
    return false;
  }
  return !Frame::is_referenced(state_and_version);
}

boost::container::pmr::memory_resource* get_buffer_manager_memory_resource() {
  return &BufferManager::get();
}

//----------------------------------------------------
// Helper Functions for Memory Mapping and Yielding
//----------------------------------------------------

std::byte* create_mapped_region() {
  const auto align = bytes_for_size_type(MAX_PAGE_SIZE_TYPE);
  const auto total = DEFAULT_RESERVED_VIRTUAL_MEMORY + align;

#ifdef __APPLE__
  const int flags = MAP_PRIVATE | MAP_ANON | MAP_NORESERVE;
#elif __linux__
  const int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
#endif

  auto raw = static_cast<std::byte*>(mmap(nullptr, total, PROT_READ | PROT_WRITE, flags, -1, 0));
  if (raw == MAP_FAILED) {
    Fail("Failed to map volatile pool region: " + std::string(strerror(errno)));
  }

  auto raw_u = reinterpret_cast<std::uintptr_t>(raw);
  auto aligned_u = (raw_u + (align - 1)) & ~(align - 1);
  auto aligned = reinterpret_cast<std::byte*>(aligned_u);

  const auto prefix = aligned - raw;
  const auto suffix = (raw + total) - (aligned + DEFAULT_RESERVED_VIRTUAL_MEMORY);

  if (prefix > 0) {
    munmap(raw, prefix);
  }
  if (suffix > 0) {
    munmap(aligned + DEFAULT_RESERVED_VIRTUAL_MEMORY, suffix);
  }

  return aligned;
}

std::array<std::shared_ptr<VolatileRegion>, NUM_PAGE_SIZE_TYPES> create_volatile_regions(
    std::byte* mapped_region, std::shared_ptr<BufferManagerMetrics> metrics) {
  DebugAssert(mapped_region != nullptr, "Region not properly mapped");
  auto array = std::array<std::shared_ptr<VolatileRegion>, NUM_PAGE_SIZE_TYPES>{};

  // Ensure that every region has the same amount of virtual memory
  // Round to the next multiple of the largest page size
  for (auto i = size_t{0}; i < NUM_PAGE_SIZE_TYPES; i++) {
    array[i] = std::make_shared<VolatileRegion>(
        magic_enum::enum_value<PageSizeType>(i), mapped_region + DEFAULT_RESERVED_VIRTUAL_MEMORY_PER_REGION * i,
        mapped_region + DEFAULT_RESERVED_VIRTUAL_MEMORY_PER_REGION * (i + 1), metrics);
  }

  return array;
}

void unmap_region(std::byte* region) {
  if (munmap(region, DEFAULT_RESERVED_VIRTUAL_MEMORY) < 0) {
    const auto error = errno;
    Fail("Failed to unmap volatile pool region: " + strerror(error));
  }
}

}  // namespace hyrise
