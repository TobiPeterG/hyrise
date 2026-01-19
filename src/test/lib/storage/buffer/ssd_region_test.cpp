#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <random>
#include <vector>

#include "base_test.hpp"

#include "storage/buffer/helper.hpp"
#include "storage/buffer/metrics.hpp"
#include "storage/buffer/ssd_region.hpp"

namespace hyrise {

class SSDRegionTest : public BaseTest {
 protected:
  void SetUp() override {
    _ssd_dir = std::filesystem::path{test_data_path} / "ssd_region_test_dir";

    std::error_code ec;
    std::filesystem::remove_all(_ssd_dir, ec);
    ec.clear();

    std::filesystem::create_directories(_ssd_dir, ec);
    ASSERT_FALSE(ec) << "Failed to create test directory: " << ec.message();
    ASSERT_TRUE(std::filesystem::is_directory(_ssd_dir));
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(_ssd_dir, ec);
  }

  std::filesystem::path _ssd_dir;
};

namespace {

// Allocate a buffer that is aligned to PAGE_ALIGNMENT (512).
struct AlignedBuffer {
  explicit AlignedBuffer(const size_t bytes) : size(bytes) {
    // aligned_alloc requires size to be a multiple of alignment
    const auto rounded = ((bytes + PAGE_ALIGNMENT - 1) / PAGE_ALIGNMENT) * PAGE_ALIGNMENT;
    size = rounded;

#if defined(_ISOC11_SOURCE)
    ptr = static_cast<std::byte*>(std::aligned_alloc(PAGE_ALIGNMENT, size));
#else
    void* raw = nullptr;
    const auto rc = ::posix_memalign(&raw, PAGE_ALIGNMENT, size);
    ptr = (rc == 0) ? static_cast<std::byte*>(raw) : nullptr;
#endif

    Assert(ptr != nullptr, "Failed to allocate aligned memory");
    std::fill(ptr, ptr + size, std::byte{0});
  }

  ~AlignedBuffer() {
    std::free(ptr);
  }

  AlignedBuffer(const AlignedBuffer&) = delete;
  AlignedBuffer& operator=(const AlignedBuffer&) = delete;

  std::byte* data() { return ptr; }
  const std::byte* data() const { return ptr; }

  std::byte* ptr = nullptr;
  size_t size = 0;
};

}  // namespace

TEST_F(SSDRegionTest, TestWriteAndReadPagesOnRegularFile) {

  auto metrics = std::make_shared<BufferManagerMetrics>();
  auto region = std::make_unique<SSDRegion>(_ssd_dir, metrics);

  EXPECT_EQ(region->get_mode(), SSDRegion::Mode::FILE_PER_SIZE_TYPE);

  // Prepare three pages with different sizes and indices.
  const auto page_id_0 = PageID{PageSizeType::KiB8, 0};
  const auto page_id_1 = PageID{PageSizeType::KiB32, 1};
  const auto page_id_2 = PageID{PageSizeType::KiB16, 2};

  AlignedBuffer write0(page_id_0.num_bytes());
  AlignedBuffer write1(page_id_1.num_bytes());
  AlignedBuffer write2(page_id_2.num_bytes());

  write0.data()[0] = std::byte{0x01};
  write1.data()[0] = std::byte{0x02};
  write2.data()[0] = std::byte{0x03};

  // Write out-of-order to ensure offset calculation uses (size_type, index) correctly.
  region->write_page(page_id_0, write0.data());
  region->write_page(page_id_2, write2.data());
  region->write_page(page_id_1, write1.data());

  AlignedBuffer read0(page_id_0.num_bytes());
  AlignedBuffer read1(page_id_1.num_bytes());
  AlignedBuffer read2(page_id_2.num_bytes());

  region->read_page(page_id_1, read1.data());
  region->read_page(page_id_2, read2.data());
  region->read_page(page_id_0, read0.data());

  EXPECT_EQ(read0.data()[0], std::byte{0x01});
  EXPECT_EQ(read1.data()[0], std::byte{0x02});
  EXPECT_EQ(read2.data()[0], std::byte{0x03});

  // Stronger check: full buffer round-trip
  EXPECT_TRUE(std::equal(write0.data(), write0.data() + page_id_0.num_bytes(), read0.data()));
  EXPECT_TRUE(std::equal(write1.data(), write1.data() + page_id_1.num_bytes(), read1.data()));
  EXPECT_TRUE(std::equal(write2.data(), write2.data() + page_id_2.num_bytes(), read2.data()));
}

TEST_F(SSDRegionTest, TestWriteFailsWithUnalignedData) {
  auto metrics = std::make_shared<BufferManagerMetrics>();
  auto region = std::make_unique<SSDRegion>(_ssd_dir, metrics);

  const auto page_id = PageID{PageSizeType::KiB8, 0};

  // Allocate aligned and then deliberately misalign by 1 byte.
  AlignedBuffer buf(page_id.num_bytes() + PAGE_ALIGNMENT);
  auto* unaligned = buf.data() + 1;

  EXPECT_ANY_THROW(region->write_page(page_id, unaligned));
}

TEST_F(SSDRegionTest, TestReadFailsWithUnalignedData) {
  auto metrics = std::make_shared<BufferManagerMetrics>();
  auto region = std::make_unique<SSDRegion>(_ssd_dir, metrics);

  const auto page_id = PageID{PageSizeType::KiB8, 0};

  // Allocate aligned and then deliberately misalign by 1 byte.
  AlignedBuffer buf(page_id.num_bytes() + PAGE_ALIGNMENT);
  auto* unaligned = buf.data() + 1;

  EXPECT_ANY_THROW(region->read_page(page_id, unaligned));
}

// TODO: Test on block device, GTEST_SKIP if no block

TEST_F(SSDRegionTest, TestBackingFilesRemovedAfterDestruction) {
  auto metrics = std::make_shared<BufferManagerMetrics>();

  {
    auto region = std::make_unique<SSDRegion>(_ssd_dir, metrics);
    ASSERT_EQ(region->get_mode(), SSDRegion::Mode::FILE_PER_SIZE_TYPE);

    // SSDRegion creates one file per PageSizeType in the directory.
    bool has_any_file = false;
    for (const auto& entry : std::filesystem::directory_iterator(_ssd_dir)) {
      has_any_file = true;
      break;
    }
    EXPECT_TRUE(has_any_file) << "Expected SSDRegion to create backing files in the directory";
  }

  // Destructor should remove the backing files it created.
  bool has_any_file_after = false;
  for (const auto& entry : std::filesystem::directory_iterator(_ssd_dir)) {
    has_any_file_after = true;
    break;
  }
  EXPECT_FALSE(has_any_file_after) << "Expected SSDRegion destructor to remove created backing files";
}

}  // namespace hyrise
