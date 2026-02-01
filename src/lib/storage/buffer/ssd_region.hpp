#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>

#include "noncopyable.hpp"
#include "storage/buffer/helper.hpp"
#include "storage/buffer/metrics.hpp"

namespace hyrise {

// TODO: Use extra object called SSDHandle to avoid page directory lockups
// TODO: Use flag for O_DIRECT
/**
  * TODO
 * @brief Page wraps binary data to be written or read. It's aligned to 512 bytes in order to work with the O_DIRECT flag and SSDs. 
 * O_DIRECT is often used in databases when they implement their own caching/buffer management like in our case. The page size should also be a multiple of the OS' page size.
 */
class SSDRegion : public Noncopyable {
 public:
  enum class Mode { BLOCK, FILE_PER_SIZE_TYPE };

  SSDRegion(const std::filesystem::path& path, std::shared_ptr<BufferManagerMetrics> metrics);
  ~SSDRegion();

  SSDRegion(SSDRegion&& other) noexcept = default;
  SSDRegion& operator=(SSDRegion&& other) noexcept = default;

  void write_page(const PageID page_id, std::byte* data);
  void read_page(const PageID page_id, std::byte* data);

  Mode get_mode() const;

  size_t memory_consumption() const;

 private:
  struct FileHandle {
    int fd{-1};
    std::filesystem::path backing_file_name;  // only meaningful in FILE_PER_SIZE_TYPE
    uint64_t offset{0};                       // byte offset for block device partitioning
  };

  Mode _mode;
  std::array<FileHandle, NUM_PAGE_SIZE_TYPES> _file_handles{};
  std::shared_ptr<BufferManagerMetrics> _metrics;

  // Directory mode: cache file sizes and grow lazily to avoid EOF short reads.
  std::array<std::atomic<uint64_t>, NUM_PAGE_SIZE_TYPES> _cached_sizes{};
  std::array<std::mutex, NUM_PAGE_SIZE_TYPES> _resize_mutexes{};

  // Mode detection
  static Mode find_mode_or_fail(const std::filesystem::path& file_name);

  // Open helpers
  static int open_file_descriptor_directory_file(const std::filesystem::path& file_name);
  static int open_file_descriptor_block_device(const std::filesystem::path& path);

  std::array<FileHandle, NUM_PAGE_SIZE_TYPES> open_file_handles_in_directory(const std::filesystem::path& path);
  std::array<FileHandle, NUM_PAGE_SIZE_TYPES> open_file_handles_block(const std::filesystem::path& path);

  // I/O helpers
  static uint64_t compute_pos_bytes(const FileHandle& handle, const PageID& page_id);

  static void robust_pread_exact(int fd, void* buf, size_t len, uint64_t off);
  static void robust_pwrite_exact(int fd, const void* buf, size_t len, uint64_t off);

  // Ensure file size covers [0, required_end). No-op in block mode.
  void ensure_capacity(const PageSizeType size_type, uint64_t required_end);
};

}  // namespace hyrise
