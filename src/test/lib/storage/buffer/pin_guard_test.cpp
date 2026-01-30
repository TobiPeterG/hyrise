#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "base_test.hpp"
#include "hyrise.hpp"

#include <boost/container/vector.hpp>
#include <nlohmann/json.hpp>

#include "storage/buffer/buffer_manager.hpp"
#include "storage/buffer/buffer_pool_allocator.hpp"
#include "storage/buffer/eviction_strategy_registry.hpp"
#include "storage/buffer/frame.hpp"
#include "storage/buffer/helper.hpp"
#include "storage/buffer/pin_guard.hpp"
#include "types.hpp"

namespace hyrise {

namespace {

// GTest parameter names must match [A-Za-z0-9_]+.
// We normalize strategy names similarly to the registry: lowercase + separators to '_'.
static std::string gtest_param_name(std::string s) {
  std::string out;
  out.reserve(s.size());

  bool last_was_sep = false;
  for (const auto ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    if (std::isalnum(c)) {
      out.push_back(static_cast<char>(std::tolower(c)));
      last_was_sep = false;
    } else {
      if (!out.empty() && !last_was_sep) {
        out.push_back('_');
        last_was_sep = true;
      }
    }
  }

  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }

  if (out.empty()) {
    out = "unknown";
  }
  return out;
}

static std::vector<std::string> registered_eviction_strategies() {
  return EvictionStrategyRegistry::instance().available_names();
}

class EnvVarGuard {
 public:
  EnvVarGuard(const std::string& key, const std::string& value) : _key(key) {
    const auto* old = std::getenv(_key.c_str());
    if (old) {
      _had_old = true;
      _old_value = old;
    }
    ::setenv(_key.c_str(), value.c_str(), 1);
  }

  ~EnvVarGuard() {
    if (_had_old) {
      ::setenv(_key.c_str(), _old_value.c_str(), 1);
    } else {
      ::unsetenv(_key.c_str());
    }
  }

  EnvVarGuard(const EnvVarGuard&) = delete;
  EnvVarGuard& operator=(const EnvVarGuard&) = delete;

 private:
  std::string _key;
  bool _had_old{false};
  std::string _old_value;
};

static bool is_pinned(const PageID page_id) {
  const auto state = BufferManager::get()._state(page_id);
  return state != Frame::UNLOCKED && state != Frame::EVICTED;
}

static bool is_unlocked(const PageID page_id) {
  const auto state = BufferManager::get()._state(page_id);
  return state == Frame::UNLOCKED;
}

}  // namespace

class PinGuardTest : public BaseTest, public ::testing::WithParamInterface<std::string> {
 protected:
  void SetUp() override {
    // Ensure a fresh Hyrise/BufferManager instance per strategy.
    _temp_dir = std::filesystem::temp_directory_path() / ("hyrise_pin_guard_test_" + gtest_param_name(GetParam()));
    std::error_code ec;
    std::filesystem::create_directories(_temp_dir, ec);

    _ssd_dir = _temp_dir / "ssd";
    std::filesystem::create_directories(_ssd_dir, ec);

    _config_path = _temp_dir / "buffer_manager.json";

    // Give enough memory so we don't depend on eviction timing in these tests.
    const auto dram_pool_size = static_cast<std::size_t>(bytes_for_size_type(PageSizeType::MiB8) * 8);

    auto json = nlohmann::json{};
    json["dram_buffer_pool_size"] = dram_pool_size;
    json["numa_buffer_pool_size"] = 0;
    json["ssd_path"] = _ssd_dir.string();

    json["migration_policy"]["dram_read_ratio"] = 1.0;
    json["migration_policy"]["dram_write_ratio"] = 1.0;
    json["migration_policy"]["numa_read_ratio"] = 1.0;
    json["migration_policy"]["numa_write_ratio"] = 1.0;

    json["eviction_strategy"] = GetParam();
    json["enable_eviction_purge_worker"] = false;
    json["memory_node"] = 0;
    json["cpu_node"] = 0;
    json["enable_numa"] = false;

    {
      std::ofstream out(_config_path);
      Assert(out.is_open(), "Failed to open temp BM config file for writing");
      out << json.dump(2);
    }

    _env_guard = std::make_unique<EnvVarGuard>("HYRISE_BUFFER_MANAGER_CONFIG_JSON_PATH", _config_path.string());

    Hyrise::reset();
  }

  void TearDown() override {
    _env_guard = nullptr;

    std::error_code ec;
    std::filesystem::remove_all(_temp_dir, ec);
  }

 private:
  std::filesystem::path _temp_dir;
  std::filesystem::path _ssd_dir;
  std::filesystem::path _config_path;
  std::unique_ptr<EnvVarGuard> _env_guard;
};

TEST_P(PinGuardTest, TestAllocatorPinGuardPinsAllocationAndUnpinsOnDestruction) {
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

TEST_P(PinGuardTest, TestSharedReadPinGuardPinsAndUnpins) {
  auto allocator = BufferPoolAllocator<std::byte>{get_buffer_manager_memory_resource()};
  auto vector =
      boost::container::vector<std::byte, BufferPoolAllocator<std::byte>>{bytes_for_size_type(PageSizeType::KiB4),
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

TEST_P(PinGuardTest, TestUnsafeSharedWritePinGuardPinsAndUnpins) {
  auto allocator = BufferPoolAllocator<std::byte>{get_buffer_manager_memory_resource()};
  auto vector =
      boost::container::vector<std::byte, BufferPoolAllocator<std::byte>>{bytes_for_size_type(PageSizeType::KiB4),
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

TEST_P(PinGuardTest, TestSharedReadPinGuardPinsVectorAndStringPages) {
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

INSTANTIATE_TEST_SUITE_P(
    RegisteredEvictionStrategies, PinGuardTest, ::testing::ValuesIn(registered_eviction_strategies()),
    [](const ::testing::TestParamInfo<std::string>& info) { return gtest_param_name(info.param); });

}  // namespace hyrise
