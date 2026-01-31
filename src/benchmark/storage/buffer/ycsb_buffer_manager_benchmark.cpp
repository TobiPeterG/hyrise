#include <memory>
#include <new>
#include <vector>

#include <random>
#include "benchmark/benchmark.h"
#include "buffer_benchmark_utils.hpp"
#include "hdr/hdr_histogram.h"
#include "hyrise.hpp"
#include "storage/buffer/buffer_manager.hpp"
#include "storage/buffer/eviction_strategy_registry.hpp"
#include "storage/buffer/jemalloc_resource.hpp"
#include "storage/buffer/zipfian_int_distribution.hpp"

namespace hyrise {

/**
 * Bechmark idea:
 * Sacle read ops with difedderne page size
 * Scale read ops with single page size and different DRAM size ratios
 * Use zipfian skews to test different hit and miss rates for single pahe size and diffeent dram size ratios
 *
 * Partly taken from https://github.com/hpides/viper/tree/master
 *
 */

// Because we want dynamic registration (one per eviction strategy), we use a
// lightweight harness object that provides SetUp/TearDown and holds state.

enum class YCSBPolicyVariant {
  Lazy,
  Eager,
  DramOnly,
  NumaOnly,
};

inline const char* to_string(const YCSBPolicyVariant v) {
  switch (v) {
    case YCSBPolicyVariant::Lazy:
      return "LazyMigrationPolicy";
    case YCSBPolicyVariant::Eager:
      return "EagerMigrationPolicy";
    case YCSBPolicyVariant::DramOnly:
      return "DramOnlyMigrationPolicy";
    case YCSBPolicyVariant::NumaOnly:
      return "NumaOnlyMigrationPolicy";
  }
  return "UnknownPolicy";
}

inline MigrationPolicy to_migration_policy(const YCSBPolicyVariant v) {
  switch (v) {
    case YCSBPolicyVariant::Lazy:
      return LazyMigrationPolicy;
    case YCSBPolicyVariant::Eager:
      return EagerMigrationPolicy;
    case YCSBPolicyVariant::DramOnly:
      return DramOnlyMigrationPolicy;
    case YCSBPolicyVariant::NumaOnly:
      return NumaOnlyMigrationPolicy;
  }
  return LazyMigrationPolicy;
}

template <YCSBWorkload WL>
class YCSBBenchmarkHarness {
 public:
  constexpr static auto PAGE_SIZE_TYPE = MIN_PAGE_SIZE_TYPE;
  constexpr static auto DEFAULT_DRAM_BUFFER_POOL_SIZE = 2UL * GB;
  constexpr static auto DEFAULT_NUMA_BUFFER_POOL_SIZE = 4UL * GB;

  constexpr static auto NUM_OPERATIONS = 10 * 1000 * 1000;

  YCSBTable table;
  YCSBOperations operations;
  hdr_histogram* latency_histogram{};
  std::mutex latency_histogram_mutex;
  BufferManager& buffer_manager = Hyrise::get().buffer_manager;
  uint64_t operations_per_thread{};

  // Runtime parameters: set by benchmark registration
  std::string eviction_strategy_name;
  YCSBPolicyVariant policy_variant{YCSBPolicyVariant::Lazy};

  // If true, we force enable_numa=false and cpu_node==memory_node==0 (DRAM+SSD only).
  bool force_no_numa{false};

  void SetUp(const ::benchmark::State& state) {
    if (state.thread_index() == 0) {
      auto config = BufferManager::Config::from_env();
      config.dram_buffer_pool_size = DEFAULT_DRAM_BUFFER_POOL_SIZE;
      config.numa_buffer_pool_size = DEFAULT_NUMA_BUFFER_POOL_SIZE;

      // Select migration policy
      config.migration_policy = to_migration_policy(policy_variant);

      if (force_no_numa) {
        // Explicitly disable NUMA and satisfy BufferManager invariant: cpu_node == memory_node
        config.enable_numa = false;
        config.cpu_node = NodeID{0};
        config.memory_node = NodeID{0};

        // Make sure we are really on the DRAM-only path
        config.migration_policy = DramOnlyMigrationPolicy;
      } else {
        config.cpu_node = NodeID{0};
        config.memory_node = NodeID{2};

        config.enable_numa = (policy_variant != YCSBPolicyVariant::DramOnly);
      }

      // runtime-selected eviction strategy
      config.eviction_strategy = eviction_strategy_name;

      Hyrise::get().buffer_manager = BufferManager(config);

      auto database_size = state.range(0) * GB;
      table = generate_ycsb_table(&buffer_manager, database_size);
      operations = generate_ycsb_operations<WL, NUM_OPERATIONS>(table.size(), 0.9);
      operations_per_thread = operations.size() / state.threads();
      init_histogram(&latency_histogram);
    }
  }

  void TearDown(const ::benchmark::State& state) {
    if (state.thread_index() == 0) {
      hdr_close(latency_histogram);
      latency_histogram = nullptr;
    }
  }

  void warmup(benchmark::State& state) {
    auto start = state.thread_index() * operations_per_thread;
    auto end = start + operations_per_thread;
    for (auto i = start; i < end; ++i) {
      const auto op = operations[i];
      execute_ycsb_action(table, buffer_manager, op);
    }
  }
};

namespace {

constexpr int kMinThreads = 1;
constexpr int kMaxThreads = 48;
constexpr int kThreadStep = 2;

constexpr int kMinDbGiB = 1;
constexpr int kMaxDbGiB = 8;
constexpr int kDbStep = 1;

constexpr int kIterations = 1;
constexpr int kRepetitions = 3;

// Helper to configure the same argument grid for all registered benchmarks.
inline void configure_common(benchmark::internal::Benchmark* b) {
  b->DenseThreadRange(kMinThreads, kMaxThreads, kThreadStep)
      ->Iterations(kIterations)
      ->Repetitions(kRepetitions)
      ->UseRealTime()
      ->DenseRange(kMinDbGiB, kMaxDbGiB, kDbStep);
}

template <YCSBWorkload WL>
void register_one_ycsb_benchmark_for_strategy_and_policy(const std::string& strategy_name,
                                                         const YCSBPolicyVariant policy_variant,
                                                         const bool force_no_numa) {
  // Benchmark name:
  //   NUMA-capable:   BM_ycsb/<WL>/<PolicyName>/<StrategyName>
  //   NUMA-disabled:  BM_ycsb_no_numa/<WL>/<StrategyName>
  std::string name;
  if (force_no_numa) {
    name = std::string{"BM_ycsb_no_numa/"} + std::string(magic_enum::enum_name(WL)) + "/" + strategy_name;
  } else {
    name = std::string{"BM_ycsb/"} + std::string(magic_enum::enum_name(WL)) + "/" + to_string(policy_variant) + "/" +
           strategy_name;
  }

  auto* b = benchmark::RegisterBenchmark(
      name.c_str(),
      [strategy_name, policy_variant, force_no_numa](benchmark::State& state) {
        YCSBBenchmarkHarness<WL> harness;
        harness.eviction_strategy_name = strategy_name;
        harness.policy_variant = policy_variant;
        harness.force_no_numa = force_no_numa;

        harness.SetUp(state);
        run_ycsb(harness, state);
        harness.TearDown(state);
      });

  configure_common(b);
}

template <YCSBPolicyVariant PolicyVariant>
void register_all_workloads_for_strategy(const std::string& strategy_name) {
  register_one_ycsb_benchmark_for_strategy_and_policy<YCSBWorkload::UpdateHeavy>(strategy_name, PolicyVariant, false);
  register_one_ycsb_benchmark_for_strategy_and_policy<YCSBWorkload::ReadMostly>(strategy_name, PolicyVariant, false);
  register_one_ycsb_benchmark_for_strategy_and_policy<YCSBWorkload::Scan>(strategy_name, PolicyVariant, false);
}

void register_all_workloads_no_numa_for_strategy(const std::string& strategy_name) {
  // Policy is forced to DramOnly inside the harness when force_no_numa=true
  register_one_ycsb_benchmark_for_strategy_and_policy<YCSBWorkload::UpdateHeavy>(strategy_name,
                                                                                YCSBPolicyVariant::DramOnly, true);
  register_one_ycsb_benchmark_for_strategy_and_policy<YCSBWorkload::ReadMostly>(strategy_name,
                                                                                YCSBPolicyVariant::DramOnly, true);
  register_one_ycsb_benchmark_for_strategy_and_policy<YCSBWorkload::Scan>(strategy_name, YCSBPolicyVariant::DramOnly,
                                                                          true);
}

void register_all_ycsb_benchmarks_for_all_strategies() {
  const auto strategy_names = EvictionStrategyRegistry::instance().available_names();

  // If there are no strategies registered, registering nothing is less confusing
  // than registering a broken benchmark.
  if (strategy_names.empty()) {
    return;
  }

  for (const auto& strategy_name : strategy_names) {
    // NUMA-capable family
    register_all_workloads_for_strategy<YCSBPolicyVariant::Lazy>(strategy_name);
    register_all_workloads_for_strategy<YCSBPolicyVariant::Eager>(strategy_name);
    register_all_workloads_for_strategy<YCSBPolicyVariant::DramOnly>(strategy_name);
    register_all_workloads_for_strategy<YCSBPolicyVariant::NumaOnly>(strategy_name);

    // Additional DRAM+SSD-only family (NUMA disabled)
    register_all_workloads_no_numa_for_strategy(strategy_name);
  }
}

// Trigger registration at static initialization time so benchmarks show up when
// running --benchmark_list_tests.
struct YCSBBenchmarkRegistration {
  YCSBBenchmarkRegistration() {
    register_all_ycsb_benchmarks_for_all_strategies();
  }
};

static YCSBBenchmarkRegistration ycsb_registration;

}  // namespace

}  // namespace hyrise
