#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <future>
#include <random>
#include <thread>
#include <filesystem>
#include <system_error>

#include "base_test.hpp"

#include "concurrency/commit_context.hpp"
#include "storage/buffer/buffer_manager.hpp"
#include "storage/buffer/eviction_strategy_registry.hpp"
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

}  // namespace

class BufferManagerStressTest : public BaseTest, public ::testing::WithParamInterface<std::string> {
 protected:
  void SetUp() override {}
};

TEST_P(BufferManagerStressTest, TestPinAndUnpins) {
  const auto ssd_path =
      std::filesystem::temp_directory_path() / ("hyrise_stress_test_" + gtest_param_name(GetParam()));

  // Ensure SSD path exists before BufferManager/SSDRegion checks it.
  std::error_code ec;  // added
  std::filesystem::create_directories(ssd_path, ec);
  ASSERT_FALSE(ec) << "Failed to create SSD directory \"" << ssd_path.string() << "\": " << ec.message();  // added
  ASSERT_TRUE(std::filesystem::is_directory(ssd_path))
      << "SSD path is not a directory: \"" << ssd_path.string() << "\"";

  auto config = BufferManager::Config{};
  config.dram_buffer_pool_size = 1 << 20;
  config.numa_buffer_pool_size = 0;
  config.memory_node = NodeID{0};
  config.cpu_node = NodeID{0};
  config.enable_numa = false;
  config.ssd_path = ssd_path;
  config.enable_eviction_purge_worker = false;
  config.eviction_strategy = GetParam();

  auto bm = BufferManager{config};

  constexpr auto NUM_REQUESTS = 20000;
  constexpr auto SEED = 18731283;
  constexpr auto PAGE_IDX_MAX = 50;
  constexpr auto RW_RATIO = 0.3;

  struct PageRequest {
    PageID page_id;
    AccessIntent access_intent;
    std::chrono::microseconds access_time;
  };

  std::vector<PageRequest> requests{NUM_REQUESTS};
  std::mt19937_64 gen{SEED};

  std::uniform_int_distribution<size_t> page_idx_dist{0, PAGE_IDX_MAX};

  std::uniform_int_distribution<size_t> size_dist{2, 3};

  std::uniform_int_distribution<size_t> wait_dist{1, 3};
  std::uniform_real_distribution<double> rw_dist{0, 1};

  std::atomic<size_t> request_idx{0};

  std::generate(requests.begin(), requests.end(), [&]() {
    const auto size_enum_idx = size_dist(gen);
    const auto page_idx = page_idx_dist(gen);

    const auto size_type = magic_enum::enum_value<PageSizeType>(size_enum_idx);
    const auto page_id = PageID{size_type, static_cast<PageID::PageIDType>(page_idx)};

    auto access_intent = rw_dist(gen) < RW_RATIO ? AccessIntent::Read : AccessIntent::Write;
    auto access_time = std::chrono::milliseconds{wait_dist(gen)};
    return PageRequest{page_id, access_intent, access_time};
  });

  // Warmup pages
  for (auto& request : requests) {
    bm.pin_exclusive(request.page_id);
    bm.unpin_exclusive(request.page_id);
  }

  const auto run = [&]() {
    while (true) {
      const auto current = request_idx.fetch_add(1);
      if (current >= NUM_REQUESTS) {
        return;
      }

      auto& request = requests[current];
      if (request.access_intent == AccessIntent::Read) {
        bm.pin_shared(request.page_id, request.access_intent);
        std::this_thread::sleep_for(request.access_time);
        bm.unpin_shared(request.page_id);
      } else {
        bm.pin_exclusive(request.page_id);
        std::this_thread::sleep_for(request.access_time);
        bm.unpin_exclusive(request.page_id);
      }
    }
  };

  // Create the async objects and spawn them asynchronously (i.e., as their own threads)
  // Note that async has a bunch of issues:
  //  - https://stackoverflow.com/questions/12508653/what-is-the-issue-with-stdasync
  //  - Mastering the C++17 STL, pages 205f
  // TODO(anyone): Change this to proper threads+futures, or at least do not reuse this code.
  const auto num_threads = uint32_t{30};
  std::vector<std::future<void>> thread_futures;
  thread_futures.reserve(num_threads);

  for (auto thread_num = uint32_t{0}; thread_num < num_threads; ++thread_num) {
    // We want a future to the thread running, so we can kill it after a future.wait(timeout) or the test would freeze
    thread_futures.emplace_back(std::async(std::launch::async, run));
  }

  // Wait for completion or timeout (should not occur)
  for (auto& thread_future : thread_futures) {
    // We give this a lot of time, not because we usually need that long for 100 threads to finish, but because
    // sanitizers and other tools like valgrind sometimes bring a high overhead.
    if (thread_future.wait_for(std::chrono::seconds(180)) == std::future_status::timeout) {
      ASSERT_TRUE(false) << "At least one thread got stuck and did not commit.";
    }
    // Retrieve the future so that exceptions stored in its state are thrown
    thread_future.get();
  }

  // Cleanup temp SSD dir
  std::filesystem::remove_all(ssd_path, ec);
}

INSTANTIATE_TEST_SUITE_P(
    RegisteredEvictionStrategies, BufferManagerStressTest, ::testing::ValuesIn(registered_eviction_strategies()),
    [](const ::testing::TestParamInfo<std::string>& info) { return gtest_param_name(info.param); });

}  // namespace hyrise
