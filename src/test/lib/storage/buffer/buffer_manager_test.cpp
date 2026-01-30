#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "base_test.hpp"
#include "storage/buffer/buffer_manager.hpp"
#include "storage/buffer/eviction_strategy_registry.hpp"
#include "storage/buffer/helper.hpp"

#if HYRISE_NUMA_SUPPORT
#include <numa.h>
#endif

namespace hyrise {

/**
 * We subclass BufferManager ONLY to inspect the node_id of frames
 * after allocation. This mirrors how other buffer tests in Hyrise
 * already inspect internals.
 */
class TestBufferManager final : public BufferManager {
 public:
  using BufferManager::BufferManager;

  Frame* frame(const PageID page_id) {
    return get_region(page_id)->get_frame(page_id);
  }
};

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

class BufferManagerMigrationPolicyTest : public BaseTest, public ::testing::WithParamInterface<std::string> {
 protected:
  void SetUp() override {
    // Always set up the SSD directory, regardless of NUMA availability.
    // Individual tests that REQUIRE NUMA will skip themselves.
    _ssd_dir = std::filesystem::path{test_data_path} / "buffer_manager_migration_policy_test";

    std::error_code ec;
    std::filesystem::remove_all(_ssd_dir, ec);
    ec.clear();

    std::filesystem::create_directories(_ssd_dir, ec);
    ASSERT_FALSE(ec) << "Failed to create SSD directory: " << ec.message();
    ASSERT_TRUE(std::filesystem::is_directory(_ssd_dir));
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(_ssd_dir, ec);
  }

  std::filesystem::path _ssd_dir;
};

static void require_two_numa_nodes_or_skip() {
#if HYRISE_NUMA_SUPPORT
  if (numa_available() < 0) {
    GTEST_SKIP() << "NUMA not available on this system.";
  }
  const auto max_node = numa_max_node();  // highest node id
  if (max_node < 1) {
    GTEST_SKIP() << "Need at least 2 NUMA nodes (0 and 1), but system has max_node=" << max_node;
  }
#else
  GTEST_SKIP() << "Built without NUMA support (HYRISE_NUMA_SUPPORT=0).";
#endif
}

#define REQUIRE_TWO_NUMA_NODES_OR_RETURN()        \
  do {                                            \
    require_two_numa_nodes_or_skip();             \
    if (::testing::Test::IsSkipped()) return;     \
  } while (false)

static TestBufferManager create_buffer_manager(const std::filesystem::path& ssd_dir, const size_t buffer_pool_size,
                                               const MigrationPolicy& policy, const std::string& eviction_strategy) {
  auto config = BufferManager::Config{};
  config.dram_buffer_pool_size = buffer_pool_size;
  config.numa_buffer_pool_size = buffer_pool_size;
  config.ssd_path = ssd_dir;

  config.enable_eviction_purge_worker = false;
  config.enable_numa = true;

  config.cpu_node = NodeID{0};
  config.memory_node = NodeID{1};

  config.migration_policy = policy;

  config.eviction_strategy = eviction_strategy;

  return TestBufferManager{config};
}

static TestBufferManager create_buffer_manager_no_numa(const std::filesystem::path& ssd_dir,
                                                       const size_t buffer_pool_size,
                                                       const MigrationPolicy& policy,
                                                       const std::string& eviction_strategy) {
  auto config = BufferManager::Config{};
  config.dram_buffer_pool_size = buffer_pool_size;
  config.numa_buffer_pool_size = buffer_pool_size;  // unused when NUMA disabled
  config.ssd_path = ssd_dir;

  config.enable_eviction_purge_worker = false;

  // NUMA disabled: ctor asserts cpu_node == memory_node
  config.enable_numa = false;
  config.cpu_node = NodeID{0};
  config.memory_node = NodeID{0};

  config.migration_policy = policy;

  config.eviction_strategy = eviction_strategy;

  return TestBufferManager{config};
}

/**
 * Allocate + deallocate repeatedly and count how many allocations
 * ended up on a specific NUMA node.
 *
 * Note: We cannot use ASSERT_* here because this function returns size_t.
 * ASSERT_* expands to "return ..." which is ill-formed in non-void functions.
 */
static size_t count_allocations_on_node(TestBufferManager& bm, const NodeID node, const size_t iterations) {
  size_t count = 0;

  constexpr auto alignment = PAGE_ALIGNMENT;
  constexpr auto size_type = PageSizeType::KiB8;
  const auto bytes = bytes_for_size_type(size_type);

  for (size_t i = 0; i < iterations; ++i) {
    void* ptr = bm.allocate(bytes, alignment);
    EXPECT_NE(ptr, nullptr);
    if (!ptr) {
      break;
    }

    const auto page_id = bm.find_page(ptr);
    EXPECT_TRUE(page_id.valid());
    if (!page_id.valid()) {
      bm.deallocate(ptr, bytes, alignment);
      break;
    }

    auto* frame = bm.frame(page_id);
    EXPECT_NE(frame, nullptr);
    if (!frame) {
      bm.deallocate(ptr, bytes, alignment);
      break;
    }

    if (frame->node_id() == node) {
      ++count;
    }

    bm.deallocate(ptr, bytes, alignment);
  }

  return count;
}

/**
 * IMPORTANT:
 * MigrationPolicy::random() uses a static thread_local generator seeded once per thread.
 * If we run multiple policies in the same thread, later policies will NOT get their own
 * deterministic sequences even if they have different seeds.
 *
 * To keep this deterministic and stable, run each policy in its own std::thread.
 */
static size_t count_allocations_on_node_in_fresh_thread(const std::filesystem::path& ssd_dir, const size_t pool_size,
                                                        const MigrationPolicy& policy, const NodeID node,
                                                        const size_t iterations,
                                                        const std::string& eviction_strategy) {
  size_t result = 0;

  std::thread t([&]() {
    auto bm = create_buffer_manager(ssd_dir, pool_size, policy, eviction_strategy);
    result = count_allocations_on_node(bm, node, iterations);
  });

  t.join();
  return result;
}

static size_t count_allocations_on_dram_when_no_numa(const std::filesystem::path& ssd_dir, const size_t pool_size,
                                                     const MigrationPolicy& policy, const size_t iterations,
                                                     const std::string& eviction_strategy) {
  auto bm = create_buffer_manager_no_numa(ssd_dir, pool_size, policy, eviction_strategy);

  constexpr auto alignment = PAGE_ALIGNMENT;
  constexpr auto size_type = PageSizeType::KiB8;
  const auto bytes = bytes_for_size_type(size_type);

  size_t on_dram = 0;

  for (size_t i = 0; i < iterations; ++i) {
    void* ptr = bm.allocate(bytes, alignment);
    EXPECT_NE(ptr, nullptr);
    if (!ptr) {
      break;
    }

    const auto page_id = bm.find_page(ptr);
    EXPECT_TRUE(page_id.valid());
    if (!page_id.valid()) {
      bm.deallocate(ptr, bytes, alignment);
      break;
    }

    auto* frame = bm.frame(page_id);
    EXPECT_NE(frame, nullptr);
    if (!frame) {
      bm.deallocate(ptr, bytes, alignment);
      break;
    }

    if (frame->node_id() == NodeID{0}) {
      ++on_dram;
    }

    bm.deallocate(ptr, bytes, alignment);
  }

  // Stronger invariant: NUMA pool must not be used when NUMA is disabled.
  EXPECT_EQ(bm.reserved_bytes_numa_buffer_pool(), 0u);

  return on_dram;
}

TEST_P(BufferManagerMigrationPolicyTest, TestAllocationPlacementRespectsMigrationPolicyExtremes) {
  REQUIRE_TWO_NUMA_NODES_OR_RETURN();

  constexpr size_t iterations = 200;
  const auto pool_size = 2 * bytes_for_size_type(MAX_PAGE_SIZE_TYPE);

  /**
   * Important:
   * MigrationPolicy::bypass_dram_during_write() returns:
   *
   *   rand > ratio
   *
   * Therefore:
   *   ratio = 0.0  → always bypass DRAM
   *   ratio = 1.0  → never bypass DRAM
   *
   * We do NOT assume which pool is primary vs secondary.
   * We only assert that the two extremes behave DIFFERENTLY
   * and deterministically.
   */

  const MigrationPolicy policy_ratio_0{/*dram_read*/ 0.0,
                                       /*dram_write*/ 0.0,
                                       /*numa_read*/ 0.0,
                                       /*numa_write*/ 0.0,
                                       /*seed*/ 42};

  const MigrationPolicy policy_ratio_1{/*dram_read*/ 0.0,
                                       /*dram_write*/ 1.0,
                                       /*numa_read*/ 0.0,
                                       /*numa_write*/ 0.0,
                                       /*seed*/ 42};

  const NodeID dram_node{0};

  const auto r0_on_dram =
      count_allocations_on_node_in_fresh_thread(_ssd_dir, pool_size, policy_ratio_0, dram_node, iterations, GetParam());
  const auto r1_on_dram =
      count_allocations_on_node_in_fresh_thread(_ssd_dir, pool_size, policy_ratio_1, dram_node, iterations, GetParam());

  // Extremes must differ
  EXPECT_NE(r0_on_dram, r1_on_dram) << "dram_write_ratio=0 and 1 should result in opposite allocation behavior";

  // Each extreme must be deterministic (all allocations end up on one node)
  EXPECT_TRUE(r0_on_dram == 0 || r0_on_dram == iterations)
      << "dram_write_ratio=0 should produce deterministic placement, got on_dram=" << r0_on_dram;

  EXPECT_TRUE(r1_on_dram == 0 || r1_on_dram == iterations)
      << "dram_write_ratio=1 should produce deterministic placement, got on_dram=" << r1_on_dram;
}

TEST_P(BufferManagerMigrationPolicyTest, TestAllocationPlacementVariesWithIntermediateDramWriteRatios) {
  REQUIRE_TWO_NUMA_NODES_OR_RETURN();

  // Use a few more iterations so that intermediate ratios have a good chance to yield both outcomes.
  constexpr size_t iterations = 1000;
  const auto pool_size = 2 * bytes_for_size_type(MAX_PAGE_SIZE_TYPE);

  const NodeID dram_node{0};

  // Keep other ratios at 0 so we primarily test the dram_write knob.
  // Use different seeds so we don't accidentally test the same RNG sequence pattern.
  const auto run = [&](const double dram_write_ratio, const int64_t seed) {
    const MigrationPolicy policy{/*dram_read*/ 0.0,
                                 /*dram_write*/ dram_write_ratio,
                                 /*numa_read*/ 0.0,
                                 /*numa_write*/ 0.0,
                                 /*seed*/ seed};
    return count_allocations_on_node_in_fresh_thread(_ssd_dir, pool_size, policy, dram_node, iterations, GetParam());
  };

  const auto on_dram_r10 = run(1.0, 101);
  const auto on_dram_r75 = run(0.75, 102);
  const auto on_dram_r50 = run(0.50, 103);
  const auto on_dram_r25 = run(0.25, 104);
  const auto on_dram_r00 = run(0.0, 105);

  // Extremes should be deterministic.
  EXPECT_TRUE(on_dram_r00 == 0 || on_dram_r00 == iterations)
      << "dram_write_ratio=0 expected deterministic placement, got on_dram=" << on_dram_r00;

  EXPECT_TRUE(on_dram_r10 == 0 || on_dram_r10 == iterations)
      << "dram_write_ratio=1 expected deterministic placement, got on_dram=" << on_dram_r10;

  // Intermediate ratios should be mixed.
  const auto expect_mixed = [&](const double ratio, const size_t on_dram) {
    EXPECT_GT(on_dram, 0u) << "dram_write_ratio=" << ratio << " should sometimes place on DRAM";
    EXPECT_LT(on_dram, iterations) << "dram_write_ratio=" << ratio << " should sometimes place off-DRAM";
  };

  expect_mixed(0.25, on_dram_r25);
  expect_mixed(0.50, on_dram_r50);
  expect_mixed(0.75, on_dram_r75);

  // Monotonic trend: larger ratio should yield >= number of allocations on DRAM.
  EXPECT_LE(on_dram_r00, on_dram_r25);
  EXPECT_LE(on_dram_r25, on_dram_r50);
  EXPECT_LE(on_dram_r50, on_dram_r75);
  EXPECT_LE(on_dram_r75, on_dram_r10);
}

TEST_P(BufferManagerMigrationPolicyTest, TestAllocationPlacementWithoutNumaAlwaysUsesDram) {
  // This test intentionally does NOT require real NUMA support.
  // It verifies that in DRAM+SSD mode (NUMA disabled), allocations always use DRAM
  // regardless of MigrationPolicy ratios.

  constexpr size_t iterations = 500;
  const auto pool_size = 2 * bytes_for_size_type(MAX_PAGE_SIZE_TYPE);

  const MigrationPolicy policy_ratio_0{/*dram_read*/ 0.0,
                                       /*dram_write*/ 0.0,
                                       /*numa_read*/ 0.0,
                                       /*numa_write*/ 0.0,
                                       /*seed*/ 42};

  const MigrationPolicy policy_ratio_1{/*dram_read*/ 0.0,
                                       /*dram_write*/ 1.0,
                                       /*numa_read*/ 0.0,
                                       /*numa_write*/ 0.0,
                                       /*seed*/ 42};

  const auto r0_on_dram =
      count_allocations_on_dram_when_no_numa(_ssd_dir, pool_size, policy_ratio_0, iterations, GetParam());
  const auto r1_on_dram =
      count_allocations_on_dram_when_no_numa(_ssd_dir, pool_size, policy_ratio_1, iterations, GetParam());

  EXPECT_EQ(r0_on_dram, iterations);
  EXPECT_EQ(r1_on_dram, iterations);
}

INSTANTIATE_TEST_SUITE_P(
    RegisteredEvictionStrategies, BufferManagerMigrationPolicyTest,
    ::testing::ValuesIn(registered_eviction_strategies()),
    [](const ::testing::TestParamInfo<std::string>& info) { return gtest_param_name(info.param); });

}  // namespace hyrise
