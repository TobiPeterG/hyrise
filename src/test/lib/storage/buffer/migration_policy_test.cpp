#include <cmath>
#include <thread>

#include "base_test.hpp"
#include "storage/buffer/migration_policy.hpp"

namespace hyrise {

class MigrationPolicyTest : public BaseTest {
 public:
  static constexpr auto iterations = 200;
};

namespace {

struct BypassCounts {
  size_t dram_read{};
  size_t dram_write{};
  size_t numa_read{};
  size_t numa_write{};
};

static BypassCounts bypass_counts_sequence_in_fresh_thread(const MigrationPolicy& policy) {
  BypassCounts counts{};
  std::thread t([&]() {
    for (auto i = 0; i < MigrationPolicyTest::iterations; ++i) {
      counts.dram_read += policy.bypass_dram_during_read() ? 1u : 0u;
    }
    for (auto i = 0; i < MigrationPolicyTest::iterations; ++i) {
      counts.dram_write += policy.bypass_dram_during_write() ? 1u : 0u;
    }
    for (auto i = 0; i < MigrationPolicyTest::iterations; ++i) {
      counts.numa_read += policy.bypass_numa_during_read() ? 1u : 0u;
    }
    for (auto i = 0; i < MigrationPolicyTest::iterations; ++i) {
      counts.numa_write += policy.bypass_numa_during_write() ? 1u : 0u;
    }
  });
  t.join();
  return counts;
}

template <typename Fn>
static size_t bypass_count_in_fresh_thread(const MigrationPolicy& policy, Fn&& fn) {
  size_t count = 0;
  std::thread t([&]() {
    for (auto i = 0; i < MigrationPolicyTest::iterations; ++i) {
      count += fn(policy) ? 1u : 0u;
    }
  });
  t.join();
  return count;
}

static void expect_count_near(const size_t count, const double expected_ratio, const size_t n,
                              const double sigma_factor = 4.0) {

  const double p = std::clamp(1.0 - expected_ratio, 0.0, 1.0);
  const double mean = p * static_cast<double>(n);
  const double var = n * p * (1.0 - p);
  const double stddev = std::sqrt(var);

  const double lo = mean - sigma_factor * stddev;
  const double hi = mean + sigma_factor * stddev;

  EXPECT_GE(static_cast<double>(count), lo);
  EXPECT_LE(static_cast<double>(count), hi);
}

}  // namespace

TEST_F(MigrationPolicyTest, TestLazyMigrationPolicyDeterministicAndPlausible) {
  const auto seed = int64_t{1648481924};
  const auto policy =
      MigrationPolicy{/*dram_read*/ 0.2, /*dram_write*/ 0.2, /*numa_read*/ 0.2, /*numa_write*/ 0.2, seed};

  const auto counts_a = bypass_counts_sequence_in_fresh_thread(policy);
  const auto counts_b = bypass_counts_sequence_in_fresh_thread(policy);

  EXPECT_EQ(counts_a.dram_read, counts_b.dram_read);
  EXPECT_EQ(counts_a.dram_write, counts_b.dram_write);
  EXPECT_EQ(counts_a.numa_read, counts_b.numa_read);
  EXPECT_EQ(counts_a.numa_write, counts_b.numa_write);

  // with ratio 0.2, bypass is ~80% => ~160/200
  expect_count_near(counts_a.dram_read, /*ratio*/ 0.2, iterations);
  expect_count_near(counts_a.dram_write, /*ratio*/ 0.2, iterations);
  expect_count_near(counts_a.numa_read, /*ratio*/ 0.2, iterations);
  expect_count_near(counts_a.numa_write, /*ratio*/ 0.2, iterations);
}

TEST_F(MigrationPolicyTest, TestEagerMigrationPolicyAlwaysMigrates) {
  const auto seed = int64_t{1477473};
  const auto policy =
      MigrationPolicy{/*dram_read*/ 1.0, /*dram_write*/ 1.0, /*numa_read*/ 1.0, /*numa_write*/ 1.0, seed};

  const auto counts = bypass_counts_sequence_in_fresh_thread(policy);

  // With ratio==1.0, bypass condition rand > 1.0 is always false (and rand==0 special-case does not bypass because rand>0).
  EXPECT_EQ(counts.dram_read, 0u);
  EXPECT_EQ(counts.dram_write, 0u);
  EXPECT_EQ(counts.numa_read, 0u);
  EXPECT_EQ(counts.numa_write, 0u);
}

TEST_F(MigrationPolicyTest, TestCustomMigrationPolicyDeterministicAndOrdered) {
  const auto seed = int64_t{1648102924};
  const auto policy =
      MigrationPolicy{/*dram_read*/ 0.13, /*dram_write*/ 0.3, /*numa_read*/ 0.6, /*numa_write*/ 0.9, seed};

  const auto counts_a = bypass_counts_sequence_in_fresh_thread(policy);
  const auto counts_b = bypass_counts_sequence_in_fresh_thread(policy);

  EXPECT_EQ(counts_a.dram_read, counts_b.dram_read);
  EXPECT_EQ(counts_a.dram_write, counts_b.dram_write);
  EXPECT_EQ(counts_a.numa_read, counts_b.numa_read);
  EXPECT_EQ(counts_a.numa_write, counts_b.numa_write);

  // higher ratio => less bypass
  // ratios: 0.13 (most bypass), 0.3, 0.6, 0.9 (least bypass)
  EXPECT_GE(counts_a.dram_read, counts_a.dram_write);
  EXPECT_GE(counts_a.dram_write, counts_a.numa_read);
  EXPECT_GE(counts_a.numa_read, counts_a.numa_write);

  expect_count_near(counts_a.dram_read, 0.13, iterations);
  expect_count_near(counts_a.dram_write, 0.3, iterations);
  expect_count_near(counts_a.numa_read, 0.6, iterations);
  expect_count_near(counts_a.numa_write, 0.9, iterations);
}

TEST_F(MigrationPolicyTest, TestThreadLocalRngIsInitializedOnlyOncePerThread) {
  const auto seed_a = int64_t{1234567};
  const auto seed_b = int64_t{7654321};

  const auto policy_a =
      MigrationPolicy{/*dram_read*/ 0.2, /*dram_write*/ 0.2, /*numa_read*/ 0.2, /*numa_write*/ 0.2, seed_a};
  const auto policy_b =
      MigrationPolicy{/*dram_read*/ 0.2, /*dram_write*/ 0.2, /*numa_read*/ 0.2, /*numa_write*/ 0.2, seed_b};

  size_t b_after_a_count = 0;

  std::thread t_same([&]() {
    // Initialize RNG with A and advance it.
    for (auto i = 0; i < 50; ++i) {
      (void)policy_a.bypass_dram_during_read();
    }

    for (auto i = 0; i < iterations; ++i) {
      b_after_a_count += policy_b.bypass_dram_during_read() ? 1u : 0u;
    }
  });
  t_same.join();

  const auto b_fresh_thread_count =
      bypass_count_in_fresh_thread(policy_b, [](const auto& p) { return p.bypass_dram_during_read(); });

  EXPECT_NE(b_after_a_count, b_fresh_thread_count)
      << "MigrationPolicy RNG appears to be re-seeded per policy in the same thread. "
      << "If this change was intentional, update/remove this test and consider how it affects determinism in other tests.";
}

}  // namespace hyrise
