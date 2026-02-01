#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <magic_enum.hpp>

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

class YcsbTableOwner {
 public:
  YcsbTableOwner() = default;

  explicit YcsbTableOwner(boost::container::pmr::memory_resource* mr) : _mr(mr) {}

  YcsbTableOwner(const YcsbTableOwner&) = delete;
  YcsbTableOwner& operator=(const YcsbTableOwner&) = delete;

  YcsbTableOwner(YcsbTableOwner&& other) noexcept {
    *this = std::move(other);
  }

  YcsbTableOwner& operator=(YcsbTableOwner&& other) noexcept {
    if (this == &other)
      return *this;
    reset();
    _mr = other._mr;
    _table = std::move(other._table);
    other._mr = nullptr;
    other._table.clear();
    return *this;
  }

  ~YcsbTableOwner() {
    reset();
  }

  void set_memory_resource(boost::container::pmr::memory_resource* mr) {
    _mr = mr;
  }

  void reset() {
    if (!_mr) {
      _table.clear();
      return;
    }
    // Deallocate every tuple exactly as allocated: size + alignment
    for (auto& t : _table) {
      if (t.ptr) {
        _mr->deallocate(t.ptr, static_cast<size_t>(t.size), CACHE_LINE_SIZE);
        t.ptr = nullptr;
      }
    }
    _table.clear();
    _table.shrink_to_fit();
  }

  YCSBTable& table() {
    return _table;
  }

  const YCSBTable& table() const {
    return _table;
  }

 private:
  boost::container::pmr::memory_resource* _mr{nullptr};
  YCSBTable _table;
};

template <YCSBWorkload WL>
struct SharedState {
  // Params
  std::string eviction_strategy_name;
  YCSBPolicyVariant policy_variant{YCSBPolicyVariant::Lazy};
  bool force_no_numa{false};

  // Data
  YcsbTableOwner table_owner;
  YCSBOperations operations;
  uint64_t operations_per_thread{0};

  hdr_histogram* global_hist{nullptr};

  // Sync
  int threads_expected{0};
  bool setup_done{false};
  int merged_threads{0};
  int teardown_threads{0};

  std::mutex m;
  std::condition_variable cv;
  std::mutex hist_m;

  void reset_for_next() {
    eviction_strategy_name.clear();
    policy_variant = YCSBPolicyVariant::Lazy;
    force_no_numa = false;
    table_owner.reset();
    operations.clear();
    operations.shrink_to_fit();
    operations_per_thread = 0;
    global_hist = nullptr;
    setup_done = false;
    merged_threads = 0;
    teardown_threads = 0;
  }
};

// Google Benchmark normally runs benchmarks sequentially, so a single shared slot works.
namespace {
std::mutex g_shared_mutex;
std::shared_ptr<void> g_shared_any;
}  // namespace

template <YCSBWorkload WL>
static std::shared_ptr<SharedState<WL>> shared_get_or_create(int threads_expected) {
  std::lock_guard<std::mutex> lk(g_shared_mutex);
  if (!g_shared_any) {
    auto s = std::make_shared<SharedState<WL>>();
    s->threads_expected = threads_expected;
    g_shared_any = s;
    return s;
  }
  auto s = std::static_pointer_cast<SharedState<WL>>(g_shared_any);
  if (s->threads_expected != threads_expected) {
    s = std::make_shared<SharedState<WL>>();
    s->threads_expected = threads_expected;
    g_shared_any = s;
  }
  return s;
}

template <YCSBWorkload WL>
static void shared_clear() {
  std::lock_guard<std::mutex> lk(g_shared_mutex);
  g_shared_any.reset();
}

static inline uint64_t execute_ycsb_action_rng(const YCSBTable& table, BufferManager& buffer_manager,
                                               const YSCBOperation& operation, std::mt19937_64& rng) {
  const auto [key, op_type] = operation;
  const auto [tuple_size, ptr] = table[key];

  const auto page_id = buffer_manager.find_page(ptr);
  const auto page_size_bytes = bytes_for_size_type(page_id.size_type());
  const auto num_cachelines = page_size_bytes / CACHE_LINE_SIZE;

  std::uniform_int_distribution<size_t> dist(0, (num_cachelines > 0 ? num_cachelines - 1 : 0));
  const auto offset = dist(rng) * CACHE_LINE_SIZE;

  switch (op_type) {
    case YSCBOperationType::Lookup: {
      buffer_manager.pin_shared(page_id, AccessIntent::Read);
      simulate_cacheline_load(ptr + offset);
      buffer_manager.unpin_shared(page_id);
      return CACHE_LINE_SIZE;
    }
    case YSCBOperationType::Update: {
      buffer_manager.pin_exclusive(page_id);
      simulate_cacheline_nontemporal_store(ptr + offset);
      buffer_manager.unpin_exclusive(page_id);
      return CACHE_LINE_SIZE;
    }
    case YSCBOperationType::Scan: {
      buffer_manager.pin_shared(page_id, AccessIntent::Read);
      simulate_scan(ptr, page_size_bytes);
      buffer_manager.unpin_shared(page_id);
      return page_size_bytes;
    }
    default:
      Fail("Operation not supported");
  }
}

template <YCSBWorkload WL>
static void run_one_instance(benchmark::State& state, const std::string& eviction_strategy_name,
                             const YCSBPolicyVariant policy_variant, const bool force_no_numa) {
  constexpr auto DEFAULT_DRAM_BUFFER_POOL_SIZE = 2UL * GB;
  constexpr auto DEFAULT_NUMA_BUFFER_POOL_SIZE = 4UL * GB;
  constexpr auto NUM_OPERATIONS = 10ULL * 1000ULL * 1000ULL;

  const int threads = state.threads();
  auto shared = shared_get_or_create<WL>(threads);

  {
    std::unique_lock<std::mutex> lk(shared->m);
    if (state.thread_index() == 0) {
      shared->reset_for_next();
      shared->threads_expected = threads;

      shared->eviction_strategy_name = eviction_strategy_name;
      shared->policy_variant = policy_variant;
      shared->force_no_numa = force_no_numa;

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

      // Our benchmark guarantees all allocations have been freed in previous teardown.
      Hyrise::get().buffer_manager = BufferManager(config);
      auto& bm = Hyrise::get().buffer_manager;

      const auto database_size = static_cast<size_t>(state.range(0)) * GB;

      shared->table_owner.set_memory_resource(&bm);
      shared->table_owner.table() = generate_ycsb_table(&bm, database_size);
      shared->operations = generate_ycsb_operations<WL, NUM_OPERATIONS>(shared->table_owner.table().size(), 0.9f);
      shared->operations_per_thread = shared->operations.size() / static_cast<uint64_t>(threads);

      init_histogram(&shared->global_hist);

      micro_benchmark_clear_cache();

      shared->setup_done = true;
      shared->cv.notify_all();
    } else {
      shared->cv.wait(lk, [&] { return shared->setup_done; });
    }
  }

  auto& bm = Hyrise::get().buffer_manager;

  const auto start = static_cast<uint64_t>(state.thread_index()) * shared->operations_per_thread;
  const auto end = start + shared->operations_per_thread;

  // Per-thread histogram
  hdr_histogram* local_hist = nullptr;
  init_histogram(&local_hist);

  // Per-thread RNG (deterministic, avoids contention)
  std::mt19937_64 rng(static_cast<uint64_t>(SEED) ^
                      (0x9e3779b97f4a7c15ULL + static_cast<uint64_t>(state.thread_index())));

  uint64_t bytes_processed = 0;

  for (auto _ : state) {
    for (uint64_t i = start; i < end; ++i) {
      const auto& op = shared->operations[i];
      const auto t0 = std::chrono::high_resolution_clock::now();
      bytes_processed += execute_ycsb_action_rng(shared->table_owner.table(), bm, op, rng);
      const auto t1 = std::chrono::high_resolution_clock::now();

      const auto latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
      hdr_record_value(local_hist, latency_ns);
    }
    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(static_cast<int64_t>(shared->operations_per_thread));
  state.SetBytesProcessed(static_cast<int64_t>(bytes_processed));

  // Merge
  {
    std::lock_guard<std::mutex> lk(shared->hist_m);
    hdr_add(shared->global_hist, local_hist);
  }
  hdr_close(local_hist);

  {
    std::unique_lock<std::mutex> lk(shared->m);
    shared->merged_threads++;
    if (shared->merged_threads == shared->threads_expected) {
      shared->cv.notify_all();
    } else {
      shared->cv.wait(lk, [&] { return shared->merged_threads == shared->threads_expected; });
    }
  }

  if (state.thread_index() == 0) {
    state.counters["cache_hit_rate"] = bm.metrics()->hit_rate();
    state.counters["latency_mean"] = hdr_mean(shared->global_hist);
    state.counters["latency_stddev"] = hdr_stddev(shared->global_hist);
    state.counters["latency_median"] = hdr_value_at_percentile(shared->global_hist, 50.0);
    state.counters["latency_min"] = hdr_min(shared->global_hist);
    state.counters["latency_max"] = hdr_max(shared->global_hist);
    state.counters["latency_95percentile"] = hdr_value_at_percentile(shared->global_hist, 95.0);
    state.counters["bytes_written_to_ssd"] = bm.metrics()->total_bytes_copied_to_ssd.load();
    state.counters["bytes_read_from_ssd"] = bm.metrics()->total_bytes_copied_from_ssd.load();

    // Eviction strategy diagnostics
    const auto m = bm.metrics();
    const auto dram = m->dram_buffer_pool_metrics;
    const auto numa = m->numa_buffer_pool_metrics;

    const auto load = [](const std::atomic_uint64_t& a) {
      return static_cast<double>(a.load(std::memory_order_relaxed));
    };

    const auto dram_inspections = load(dram->num_eviction_candidate_inspections);
    const auto numa_inspections = load(numa->num_eviction_candidate_inspections);

    state.counters["evict_inspections_total"] = dram_inspections + numa_inspections;
    state.counters["evict_inspections_dram"] = dram_inspections;
    state.counters["evict_inspections_numa"] = numa_inspections;

    state.counters["evict_requeues_pinned_total"] =
        load(dram->num_eviction_requeues_pinned) + load(numa->num_eviction_requeues_pinned);
    state.counters["evict_requeues_ref_total"] =
        load(dram->num_eviction_requeues_referenced) + load(numa->num_eviction_requeues_referenced);
    state.counters["evict_requeues_lockfail_total"] =
        load(dram->num_eviction_requeues_lock_failed) + load(numa->num_eviction_requeues_lock_failed);

    state.counters["evict_failures_total"] = load(dram->num_eviction_failures) + load(numa->num_eviction_failures);

    state.counters["evict_episodes_total"] =
        load(dram->num_oversubscription_episodes) + load(numa->num_oversubscription_episodes);

    state.counters["evictions_total"] = load(dram->num_evictions) + load(numa->num_evictions);
    state.counters["evict_queue_adds_total"] =
        load(dram->num_eviction_queue_adds) + load(numa->num_eviction_queue_adds);
    state.counters["evict_queue_purged_total"] =
        load(dram->num_eviction_queue_items_purged) + load(numa->num_eviction_queue_items_purged);

    // derived ratios
    const auto evictions_total = state.counters["evictions_total"];
    if (evictions_total > 0.0) {
      state.counters["evict_inspections_per_eviction"] = state.counters["evict_inspections_total"] / evictions_total;
      state.counters["evict_requeues_pinned_per_eviction"] =
          state.counters["evict_requeues_pinned_total"] / evictions_total;
      state.counters["evict_requeues_ref_per_eviction"] = state.counters["evict_requeues_ref_total"] / evictions_total;
      state.counters["evict_requeues_lockfail_per_eviction"] =
          state.counters["evict_requeues_lockfail_total"] / evictions_total;
    }
  }

  {
    std::unique_lock<std::mutex> lk(shared->m);
    shared->teardown_threads++;
    if (shared->teardown_threads == shared->threads_expected) {
      // last thread cleans up:
      shared->table_owner.reset();
      shared->operations.clear();
      shared->operations.shrink_to_fit();
      hdr_close(shared->global_hist);
      shared->global_hist = nullptr;
      shared->setup_done = false;

      lk.unlock();
      shared_clear<WL>();
    }
  }
}

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

  auto* b = benchmark::RegisterBenchmark(name.c_str(),
                                         [strategy_name, policy_variant, force_no_numa](benchmark::State& state) {
                                           run_one_instance<WL>(state, strategy_name, policy_variant, force_no_numa);
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
  if (strategy_names.empty())
    return;

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
