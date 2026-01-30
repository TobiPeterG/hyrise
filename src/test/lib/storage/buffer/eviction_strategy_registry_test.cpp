#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "base_test.hpp"

#include "storage/buffer/buffer_pool.hpp"
#include "storage/buffer/eviction_strategy_registry.hpp"
#include "storage/buffer/helper.hpp"
#include "storage/buffer/metrics.hpp"
#include "storage/buffer/ssd_region.hpp"
#include "storage/buffer/volatile_region.hpp"

namespace hyrise {

class EvictionStrategyRegistryTest : public BaseTest {};

namespace {

struct RegistryTestContext {
  std::byte* mapped_region{};
  std::shared_ptr<BufferManagerMetrics> bm_metrics;
  std::array<std::shared_ptr<VolatileRegion>, NUM_PAGE_SIZE_TYPES> regions;

  std::filesystem::path ssd_dir;
  std::shared_ptr<SSDRegion> ssd_region;

  std::shared_ptr<BufferPoolMetrics> pool_metrics;

  RegistryTestContext()
      : mapped_region(create_mapped_region()),
        bm_metrics(std::make_shared<BufferManagerMetrics>()),
        regions(create_volatile_regions(mapped_region, bm_metrics)),
        ssd_dir(std::filesystem::temp_directory_path() / "hyrise_eviction_registry_test_ssd"),
        pool_metrics(std::make_shared<BufferPoolMetrics>()) {
    std::error_code ec;
    std::filesystem::create_directories(ssd_dir, ec);
    ssd_region = std::make_shared<SSDRegion>(ssd_dir, bm_metrics);
  }

  ~RegistryTestContext() {
    if (mapped_region) {
      unmap_region(mapped_region);
      mapped_region = nullptr;
    }

    std::error_code ec;
    std::filesystem::remove_all(ssd_dir, ec);
  }
};

static std::string pick_any_registered_strategy() {
  const auto names = EvictionStrategyRegistry::instance().available_names();
  Assert(!names.empty(), "No eviction strategies registered (tests require at least one)");
  return names.front();
}

}  // namespace

TEST_F(EvictionStrategyRegistryTest, AvailableNamesIsNonEmptyAndSortedUnique) {
  const auto names = EvictionStrategyRegistry::instance().available_names();
  ASSERT_FALSE(names.empty());

  // available_names() sorts and uniques.
  for (size_t i = 1; i < names.size(); ++i) {
    EXPECT_LT(names[i - 1], names[i]);
  }
}

TEST_F(EvictionStrategyRegistryTest, CreateUnknownStrategyFails) {
  RegistryTestContext ctx;

  const auto existing = pick_any_registered_strategy();

  BufferPool pool{/*enabled*/ true,
                  /*pool_size*/ bytes_for_size_type(PageSizeType::KiB4) * 4,
                  /*enable_eviction_purge_worker*/ false,
                  /*volatile_regions*/ ctx.regions,
                  /*migration_policy*/ EagerMigrationPolicy,
                  /*ssd_region*/ ctx.ssd_region,
                  /*target_buffer_pool*/ nullptr,
                  /*eviction_strategy_name*/ existing,
                  /*numa_node*/ NodeID{0},
                  /*metrics*/ ctx.pool_metrics};

  EXPECT_ANY_THROW((void)EvictionStrategyRegistry::instance().create("definitely_not_a_strategy__", pool));
}

TEST_F(EvictionStrategyRegistryTest, RegisterStrategyWithAliasesAndCreateViaDifferentSpellings) {
  RegistryTestContext ctx;

  const auto existing = pick_any_registered_strategy();

  BufferPool pool{/*enabled*/ true,
                  /*pool_size*/ bytes_for_size_type(PageSizeType::KiB4) * 4,
                  /*enable_eviction_purge_worker*/ false,
                  /*volatile_regions*/ ctx.regions,
                  /*migration_policy*/ EagerMigrationPolicy,
                  /*ssd_region*/ ctx.ssd_region,
                  /*target_buffer_pool*/ nullptr,
                  /*eviction_strategy_name*/ existing,
                  /*numa_node*/ NodeID{0},
                  /*metrics*/ ctx.pool_metrics};

  auto& reg = EvictionStrategyRegistry::instance();

  // Use a unique canonical name to avoid collisions across repeated test runs / other suites.
  const std::string canon = "Registry Test Strategy 9f3c1a";
  const std::vector<std::string> aliases = {"registry-test-strategy-9f3c1a", "REGISTRY_TEST_STRATEGY_9f3c1a"};

  const auto ok = reg.register_strategy(
      canon, [](BufferPool&) -> std::unique_ptr<EvictionStrategy> { return nullptr; }, aliases);
  ASSERT_TRUE(ok);

  // Canonical name should show up
  const auto names = reg.available_names();
  ASSERT_TRUE(std::find(names.begin(), names.end(), canon) != names.end());

  // Creating by canonical / alias / differently separated spellings should work and yield nullptr
  EXPECT_NO_THROW({
    auto s = reg.create(canon, pool);
    EXPECT_EQ(s, nullptr);
  });

  EXPECT_NO_THROW({
    auto s = reg.create("registry_test_strategy_9f3c1a", pool);  // normalized canonical
    EXPECT_EQ(s, nullptr);
  });

  EXPECT_NO_THROW({
    auto s = reg.create("registry-test-strategy-9f3c1a", pool);  // alias
    EXPECT_EQ(s, nullptr);
  });

  EXPECT_NO_THROW({
    auto s = reg.create("REGISTRY TEST STRATEGY 9f3c1a", pool);  // canonical with spaces/upper
    EXPECT_EQ(s, nullptr);
  });


  // The registry is intentionally idempotent if the same canonical is registered again.
  // So this must return true
  EXPECT_TRUE(reg.register_strategy(
      canon, [](BufferPool&) -> std::unique_ptr<EvictionStrategy> { return nullptr; }));

  // But registering a different canonical that normalizes to the same name must fail.
  // This collides with the existing canonical_norm "registry_test_strategy_9f3c1a".
  EXPECT_FALSE(reg.register_strategy(
      "registry_test_strategy_9f3c1a", [](BufferPool&) -> std::unique_ptr<EvictionStrategy> { return nullptr; }));

  // Alias collisions should fail if a different canonical tries to claim an existing alias.
  EXPECT_FALSE(reg.register_strategy(
      "Some Other Canonical 9f3c1a",
      [](BufferPool&) -> std::unique_ptr<EvictionStrategy> { return nullptr; },
      {"registry-test-strategy-9f3c1a"}));
}

}  // namespace hyrise
