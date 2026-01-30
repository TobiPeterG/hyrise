#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "base_test.hpp"
#include "hyrise.hpp"
#include "scheduler/node_queue_scheduler.hpp"
#include "storage/buffer/eviction_strategy_registry.hpp"
#include "storage/buffer/helper.hpp"
#include "storage/buffer/pin_guard.hpp"

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

    // setenv is POSIX
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

}  // namespace

class BufferManagerStressTest : public BaseTest, public ::testing::WithParamInterface<std::string> {
 protected:
  void SetUp() override {
    // Build a per-test BM config so the test runs once per registered eviction strategy.
    _temp_dir = std::filesystem::temp_directory_path() / "hyrise_bm_stress_test";
    std::error_code ec;
    std::filesystem::create_directories(_temp_dir, ec);

    _ssd_dir = _temp_dir / "ssd";
    std::filesystem::create_directories(_ssd_dir, ec);

    _config_path = _temp_dir / ("bm_config_" + gtest_param_name(GetParam()) + ".json");

    // Keep this reasonably small but not tiny. The test uses pmr_vector<int> with random sizes.
    // We want enough headroom so that the test is not dominated by eviction.
    const auto dram_pool_size = static_cast<std::size_t>(bytes_for_size_type(PageSizeType::MiB8) * 64);

    auto json = nlohmann::json{};
    json["dram_buffer_pool_size"] = dram_pool_size;
    json["numa_buffer_pool_size"] = 0;  // unused when NUMA disabled
    json["ssd_path"] = _ssd_dir.string();

    // Migration policy and NUMA are irrelevant for this test
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

    // Reset after setting the env var so the new BufferManager uses the strategy under test.
    Hyrise::reset();
  }

  void TearDown() override {
    // Clean up after the test run
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

TEST_P(BufferManagerStressTest, TestVariousAllocationsReadWrites) {
  Hyrise::get().scheduler()->wait_for_all_tasks();
  const auto previous_scheduler = Hyrise::get().scheduler();
  Hyrise::get().set_scheduler(std::make_shared<NodeQueueScheduler>());
  std::vector<std::shared_ptr<AbstractTask>> jobs;

  constexpr auto num_vectors = 100;

  std::vector<int> vector_sizes(num_vectors);
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(bytes_for_size_type(MIN_PAGE_SIZE_TYPE) / sizeof(int),
                                      bytes_for_size_type(MAX_PAGE_SIZE_TYPE) / sizeof(int));
  std::generate(vector_sizes.begin(), vector_sizes.end(), [&]() { return dis(gen); });

  // Create various pmr_vectors of different sizes, creation, shuffling, sorting and verification happens
  // in different scopes to test locking mechanisms
  for (int i = 0; i < num_vectors; i++) {
    jobs.emplace_back(std::make_shared<JobTask>([&vector_sizes, i]() {
      pmr_vector<int> vector = [&vector_sizes, i]() {
        auto allocator = PolymorphicAllocator<int>{get_buffer_manager_memory_resource()};
        auto pin_guard = AllocatorPinGuard{allocator};
        pmr_vector<int> vector(vector_sizes[i], allocator);
        std::iota(vector.begin(), vector.end(), i);
        std::reverse(vector.begin(), vector.end());
        return std::move(vector);
      }();

      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      {
        SharedReadPinGuard guard{vector};
        EXPECT_FALSE(std::is_sorted(vector.begin(), vector.end()));
      }
      {
        ExclusivePinGuard guard{vector};
        std::sort(vector.begin(), vector.end());
      }
      {
        SharedReadPinGuard guard{vector};

        EXPECT_EQ(*vector.begin(), i);
        EXPECT_EQ(*(vector.end() - 1), vector_sizes[i] + i - 1);

        EXPECT_TRUE(std::is_sorted(vector.begin(), vector.end()));
      }
    }));

    jobs.back()->schedule();
  }

  Hyrise::get().scheduler()->wait_for_tasks(jobs);

  // Restore scheduler to not influence other tests.
  Hyrise::get().set_scheduler(previous_scheduler);
}

INSTANTIATE_TEST_SUITE_P(
    RegisteredEvictionStrategies, BufferManagerStressTest, ::testing::ValuesIn(registered_eviction_strategies()),
    [](const ::testing::TestParamInfo<std::string>& info) { return gtest_param_name(info.param); });

}  // namespace hyrise
