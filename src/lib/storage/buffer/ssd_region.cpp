#include "ssd_region.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

namespace hyrise {

// TODO: Properly support block device

SSDRegion::Mode SSDRegion::find_mode_or_fail(const std::filesystem::path& file_name) {
  // Debug helpers: we want actionable info when this fails in tests.
  const auto exists = std::filesystem::exists(file_name);
  const auto is_dir = std::filesystem::is_directory(file_name);
  const auto is_blk = std::filesystem::is_block_file(file_name);

#ifndef NDEBUG
  std::cerr << "[BM][SSDRegion] path=\"" << file_name.string() << "\""
            << " exists=" << exists << " is_directory=" << is_dir << " is_block_file=" << is_blk << "\n";
#endif

  if (is_dir) {
    return Mode::FILE_PER_SIZE_TYPE;
  }
  if (is_blk) {
    return Mode::BLOCK;
  }

#ifndef NDEBUG
  std::error_code ec;
  const auto st = std::filesystem::status(file_name, ec);
  std::cerr << "[BM][SSDRegion] status.type=" << static_cast<int>(st.type()) << " ec=" << ec.value()
            << " msg=\"" << ec.message() << "\"\n";
#endif
  Fail("SSDRegion backing path must be either a directory (on SSD) or a block device (raw SSD).");
}

int SSDRegion::open_file_descriptor_directory_file(const std::filesystem::path& file_name) {
#ifdef __APPLE__
  // macOS: no O_DIRECT. Keep simplest behavior (works, but uses OS cache).
  int flags = O_RDWR | O_CREAT;
  const int fd = ::open(file_name.string().c_str(), flags, 0666);
  if (fd < 0) {
    const auto e = errno;
    Fail("SSDRegion open failed for file " + file_name.string() + ": " + std::string(strerror(e)));
  }
  return fd;

#elif __linux__
  // Real SSD directory mode: use O_DIRECT to bypass page cache.
  // Caller must ensure filesystem supports O_DIRECT
  int flags = O_RDWR | O_CREAT | O_DIRECT;
  const int fd = ::open(file_name.string().c_str(), flags, 0666);
  if (fd < 0) {
    const auto e = errno;
    Fail("SSDRegion open failed for file " + file_name.string() + " with O_DIRECT: " + std::string(strerror(e)) +
         ". Ensure this directory is on an SSD filesystem that supports O_DIRECT.");
  }
  return fd;

#else
  int flags = O_RDWR | O_CREAT;
  const int fd = ::open(file_name.string().c_str(), flags, 0666);
  if (fd < 0) {
    const auto e = errno;
    Fail("SSDRegion open failed for file " + file_name.string() + ": " + std::string(strerror(e)));
  }
  return fd;
#endif
}

int SSDRegion::open_file_descriptor_block_device(const std::filesystem::path& path) {
#ifdef __APPLE__
  Fail("Block device mode not supported on macOS in this implementation.");

#elif __linux__
  // Block device: no O_CREAT, direct I/O.
  int flags = O_RDWR | O_DIRECT;
  const int fd = ::open(path.string().c_str(), flags);
  if (fd < 0) {
    const auto e = errno;
    Fail("SSDRegion open failed for block device " + path.string() + " with O_DIRECT: " + std::string(strerror(e)));
  }
  return fd;

#else
  int flags = O_RDWR;
  const int fd = ::open(path.string().c_str(), flags);
  if (fd < 0) {
    const auto e = errno;
    Fail("SSDRegion open failed for block device " + path.string() + ": " + std::string(strerror(e)));
  }
  return fd;
#endif
}

void SSDRegion::robust_pread_exact(int fd, void* buf, size_t len, uint64_t off) {
  auto* p = reinterpret_cast<std::byte*>(buf);
  size_t done = 0;

  while (done < len) {
    const auto n = ::pread(fd, p + done, len - done, static_cast<off_t>(off + done));
    if (n == 0) {
      Fail("SSDRegion short read (EOF). fd=" + std::to_string(fd) + " off=" + std::to_string(off) +
           " len=" + std::to_string(len) + " done=" + std::to_string(done));
    }
    if (n < 0) {
      if (errno == EINTR) continue;
      const auto e = errno;
      Fail("SSDRegion pread failed: " + std::string(strerror(e)));
    }
    done += static_cast<size_t>(n);
  }
}

void SSDRegion::robust_pwrite_exact(int fd, const void* buf, size_t len, uint64_t off) {
  const auto* p = reinterpret_cast<const std::byte*>(buf);
  size_t done = 0;

  while (done < len) {
    const auto n = ::pwrite(fd, p + done, len - done, static_cast<off_t>(off + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      const auto e = errno;
      Fail("SSDRegion pwrite failed: " + std::string(strerror(e)));
    }
    if (n == 0) {
      Fail("SSDRegion short write (0 progress). fd=" + std::to_string(fd) + " off=" + std::to_string(off) +
           " len=" + std::to_string(len) + " done=" + std::to_string(done));
    }
    done += static_cast<size_t>(n);
  }
}

uint64_t SSDRegion::compute_pos_bytes(const FileHandle& handle, const PageID& page_id) {
  const auto num_bytes = static_cast<uint64_t>(page_id.num_bytes());
  const auto idx = static_cast<uint64_t>(page_id.index);
  return handle.offset + num_bytes * idx;
}

void SSDRegion::ensure_capacity(const PageSizeType size_type, uint64_t required_end) {
  if (_mode != Mode::FILE_PER_SIZE_TYPE) {
    return;  // block devices don't have EOF semantics
  }

  const auto idx = static_cast<size_t>(size_type);
  auto current = _cached_sizes[idx].load(std::memory_order_relaxed);
  if (current >= required_end) {
    return;
  }

  std::lock_guard<std::mutex> lock(_resize_mutexes[idx]);
  current = _cached_sizes[idx].load(std::memory_order_relaxed);
  if (current >= required_end) {
    return;
  }

  // Grow with headroom to amortize truncations.
  // Use fairly large chunks to keep resize syscalls rare.
  constexpr uint64_t kChunk = 64ULL * 1024ULL * 1024ULL;  // 64 MiB
  const uint64_t new_size = ((required_end + kChunk - 1) / kChunk) * kChunk;

  const int fd = _file_handles[idx].fd;
  if (fd < 0) {
    Fail("SSDRegion ensure_capacity: invalid fd");
  }

  if (::ftruncate(fd, static_cast<off_t>(new_size)) != 0) {
    const auto e = errno;
    Fail("SSDRegion ftruncate failed (size=" + std::to_string(new_size) + "): " + std::string(strerror(e)));
  }

  _cached_sizes[idx].store(new_size, std::memory_order_relaxed);
}

std::array<SSDRegion::FileHandle, NUM_PAGE_SIZE_TYPES> SSDRegion::open_file_handles_in_directory(
    const std::filesystem::path& path) {
  DebugAssert(std::filesystem::is_directory(path), "SSDRegion path must be a directory");
  auto array = std::array<FileHandle, NUM_PAGE_SIZE_TYPES>{};

  const auto now = std::chrono::system_clock::now();
  const auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

  for (auto i = size_t{0}; i < NUM_PAGE_SIZE_TYPES; ++i) {
    const auto file_name =
        path / ("hyrise-buffer-pool-" + std::to_string(timestamp) + "-type-" + std::to_string(i) + ".bin");

    array[i].fd = open_file_descriptor_directory_file(file_name);
    array[i].backing_file_name = file_name;
    array[i].offset = 0;

    // Start at 0 and grow lazily.
    if (::ftruncate(array[i].fd, 0) != 0) {
      const auto e = errno;
      ::close(array[i].fd);
      Fail("SSDRegion ftruncate(0) failed for " + file_name.string() + ": " + std::string(strerror(e)));
    }

    _cached_sizes[i].store(0, std::memory_order_relaxed);
  }

  return array;
}

std::array<SSDRegion::FileHandle, NUM_PAGE_SIZE_TYPES> SSDRegion::open_file_handles_block(
    const std::filesystem::path& path) {
  DebugAssert(std::filesystem::is_block_file(path), "SSDRegion path must be a block device");
  auto array = std::array<FileHandle, NUM_PAGE_SIZE_TYPES>{};

#ifdef __APPLE__
  Fail("Block device mode not supported on macOS in this implementation.");
#else
  const int fd = open_file_descriptor_block_device(path);

  uint64_t block_size = 0;

#ifdef __linux__
  if (ioctl(fd, BLKGETSIZE64, &block_size) < 0) {
    const auto e = errno;
    ::close(fd);
    Fail("Failed to get size of block device: " + std::string(strerror(e)));
  }
#else
  // best-effort fallback
  struct stat st {};
  if (fstat(fd, &st) != 0) {
    const auto e = errno;
    ::close(fd);
    Fail("Failed to fstat block device: " + std::string(strerror(e)));
  }
  block_size = static_cast<uint64_t>(st.st_size);
#endif

  const auto bytes_per_size_type = ((block_size / NUM_PAGE_SIZE_TYPES) / OS_PAGE_SIZE) * OS_PAGE_SIZE;
  if (bytes_per_size_type == 0) {
    ::close(fd);
    Fail("Block device too small to partition for NUM_PAGE_SIZE_TYPES");
  }

  for (auto i = size_t{0}; i < NUM_PAGE_SIZE_TYPES; ++i) {
    array[i].fd = fd;
    array[i].backing_file_name = path;
    array[i].offset = static_cast<uint64_t>(i) * bytes_per_size_type;
  }

  return array;
#endif
}

SSDRegion::SSDRegion(const std::filesystem::path& path, std::shared_ptr<BufferManagerMetrics> metrics)
    : _mode(find_mode_or_fail(path)),
      _metrics(std::move(metrics)) {
  for (auto& s : _cached_sizes) {
    s.store(0, std::memory_order_relaxed);
  }

  _file_handles = (_mode == Mode::FILE_PER_SIZE_TYPE) ? open_file_handles_in_directory(path)
                                                      : open_file_handles_block(path);
}

SSDRegion::~SSDRegion() {
  if (_mode == Mode::BLOCK) {
    // All entries share the same fd; close once.
    const int fd = _file_handles[0].fd;
    if (fd >= 0) {
      ::close(fd);
    }
    return;
  }

  // FILE_PER_SIZE_TYPE: close each file and remove it
  for (auto& h : _file_handles) {
    if (h.fd >= 0) {
      ::close(h.fd);
    }
    if (!h.backing_file_name.empty()) {
      std::error_code ec;
      std::filesystem::remove(h.backing_file_name, ec);
      // ignore errors in destructor
    }
  }
}

SSDRegion::Mode SSDRegion::get_mode() const {
  return _mode;
}

void SSDRegion::write_page(const PageID page_id, std::byte* data) {
  const auto size_type = page_id.size_type();
  const auto num_bytes = static_cast<size_t>(page_id.num_bytes());
  require_aligned_or_fail(data);

  const auto& handle = _file_handles[static_cast<size_t>(size_type)];
  const uint64_t pos = compute_pos_bytes(handle, page_id);

  ensure_capacity(size_type, pos + num_bytes);

  robust_pwrite_exact(handle.fd, data, num_bytes, pos);
  _metrics->total_bytes_copied_to_ssd.fetch_add(num_bytes, std::memory_order_relaxed);
}

void SSDRegion::read_page(const PageID page_id, std::byte* data) {
  const auto size_type = page_id.size_type();
  const auto num_bytes = static_cast<size_t>(page_id.num_bytes());
  require_aligned_or_fail(data);

  const auto& handle = _file_handles[static_cast<size_t>(size_type)];
  const uint64_t pos = compute_pos_bytes(handle, page_id);

  // Ensure the file is large enough; reading unwritten areas yields zeros on typical filesystems.
  ensure_capacity(size_type, pos + num_bytes);

  robust_pread_exact(handle.fd, data, num_bytes, pos);
  _metrics->total_bytes_copied_from_ssd.fetch_add(num_bytes, std::memory_order_relaxed);
}

size_t SSDRegion::memory_consumption() const {
  return sizeof(*this);
}

}  // namespace hyrise
