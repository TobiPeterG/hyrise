#include "buffer_manager.hpp"
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#ifndef NDEBUG
#include <execinfo.h>
#include <atomic>
#endif
#include <fstream>
#include <iostream>
#include <utility>
#include "hyrise.hpp"
#include "storage/buffer/jemalloc_resource.hpp"
#include "storage/buffer/ssd_region.hpp"
#include "storage/buffer/volatile_region.hpp"
#include "utils/assert.hpp"

#if HYRISE_NUMA_SUPPORT
#include <numa.h>
#endif

#ifndef NDEBUG
#include <fstream>
#include <sstream>
#endif

#if defined(HYRISE_WITH_JEMALLOC) && !defined(NDEBUG)
namespace hyrise {
bool& jemalloc_extent_hooks_tls_flag();
}  // namespace hyrise
#endif

namespace hyrise {

// TODO: On Mac, we should use MSYNC to see if a page is still in memory or not to avoid loading from disk
// TODO: Incluse page size in mihration desction -> large page size should be normalized

#ifndef NDEBUG
namespace {
std::mutex s_live_pages_mutex;
// TODO: Check again
// Global epoch that advances whenever a new BufferManager mapping is installed.
// This prevents false positives when the BM mapping base changes but (size_type, index) repeats.
std::atomic<uint64_t> s_mapping_epoch{0};

static void bump_mapping_epoch() {
  s_mapping_epoch.fetch_add(1, std::memory_order_relaxed);
}

struct LivePageKey {
  uint64_t epoch;
  uint8_t size_type;
  uint64_t index;
};

struct LivePageKeyHash {
  size_t operator()(const LivePageKey& k) const noexcept {
    // Simple mix; collisions are fine because unordered_set also checks equality.
    size_t h = std::hash<uint64_t>{}(k.epoch);
    h ^= (static_cast<size_t>(k.size_type) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
    h ^= (std::hash<uint64_t>{}(k.index) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
    return h;
  }
};

struct LivePageKeyEq {
  bool operator()(const LivePageKey& a, const LivePageKey& b) const noexcept {
    return a.epoch == b.epoch && a.size_type == b.size_type && a.index == b.index;
  }
};

std::unordered_set<LivePageKey, LivePageKeyHash, LivePageKeyEq> s_live_pages;

static LivePageKey live_page_key(const PageID& page_id) {
  return LivePageKey{s_mapping_epoch.load(std::memory_order_relaxed), static_cast<uint8_t>(page_id._size_type),
                     static_cast<uint64_t>(page_id.index)};
}
}  // namespace
#endif

//----------------------------------------------------
// Config
//----------------------------------------------------

BufferManager::Config BufferManager::Config::from_env() {
  if (const auto json_path = std::getenv("HYRISE_BUFFER_MANAGER_CONFIG_JSON_PATH")) {
    std::ifstream json_file(json_path);
    if (!json_file.is_open()) {
      Fail("Failed to open HYRISE_BUFFER_MANAGER_CONFIG_JSON_PATH file");
    }

    nlohmann::json json;
    try {
      json_file >> json;
    } catch (const std::exception& e) {
      Fail("Failed to parse HYRISE_BUFFER_MANAGER_CONFIG_JSON_PATH file: " + std::string(e.what()));
    }

    auto config = BufferManager::Config{};
    config.dram_buffer_pool_size = json.value("dram_buffer_pool_size", config.dram_buffer_pool_size);
    config.numa_buffer_pool_size = json.value("numa_buffer_pool_size", config.numa_buffer_pool_size);

    if (std::filesystem::is_block_file(json.value("ssd_path", config.ssd_path)) ||
        std::filesystem::is_directory(json.value("ssd_path", config.ssd_path))) {
      config.ssd_path = json.value("ssd_path", config.ssd_path);
    } else {
      Fail("ssd_path is neither a block device nor a directory");
    }
    auto migration_policy_json = json.value("migration_policy", nlohmann::json{});
    config.migration_policy = MigrationPolicy{
        migration_policy_json.value("dram_read_ratio", config.migration_policy.get_dram_read_ratio()),
        migration_policy_json.value("dram_write_ratio", config.migration_policy.get_dram_write_ratio()),
        migration_policy_json.value("numa_read_ratio", config.migration_policy.get_numa_read_ratio()),
        migration_policy_json.value("numa_write_ratio", config.migration_policy.get_numa_write_ratio())};
    config.enable_eviction_purge_worker =
        json.value("enable_eviction_purge_worker", config.enable_eviction_purge_worker);
    config.memory_node = static_cast<NodeID>(json.value("memory_node", static_cast<int64_t>(config.memory_node)));
    config.cpu_node = static_cast<NodeID>(json.value("cpu_node", static_cast<int64_t>(config.cpu_node)));
    config.enable_numa = json.value("enable_numa", config.enable_numa);

    return config;
  } else {
    Fail("HYRISE_BUFFER_MANAGER_CONFIG_JSON_PATH not found in environment");
  }
}

nlohmann::json BufferManager::Config::to_json() const {
  auto json = nlohmann::json{};
  json["dram_buffer_pool_size"] = dram_buffer_pool_size;
  json["numa_buffer_pool_size"] = numa_buffer_pool_size;
  json["ssd_path"] = ssd_path;
  json["migration_policy"]["dram_read_ratio"] = migration_policy.get_dram_read_ratio();
  json["migration_policy"]["dram_write_ratio"] = migration_policy.get_dram_write_ratio();
  json["migration_policy"]["numa_read_ratio"] = migration_policy.get_numa_read_ratio();
  json["migration_policy"]["numa_write_ratio"] = migration_policy.get_numa_write_ratio();
  json["enable_eviction_purge_worker"] = enable_eviction_purge_worker;
  json["memory_node"] = static_cast<int64_t>(memory_node);
  return json;
}

//----------------------------------------------------
// BufferManager
//----------------------------------------------------

BufferManager::BufferManager() : BufferManager(Config::from_env()) {}

BufferManager::BufferManager(const Config config)
    : _config(config),
      _mapped_region(create_mapped_region()),
      _metrics(std::make_shared<BufferManagerMetrics>()),
      _volatile_regions(create_volatile_regions(_mapped_region, _metrics)),
      _ssd_region(std::make_shared<SSDRegion>(config.ssd_path, _metrics)),
      _primary_buffer_pool(std::make_shared<BufferPool>(
          true, config.dram_buffer_pool_size, config.enable_eviction_purge_worker, _volatile_regions,
          config.migration_policy, _ssd_region, nullptr, config.cpu_node, _metrics->dram_buffer_pool_metrics)),
      _secondary_buffer_pool(std::make_shared<BufferPool>(config.enable_numa, config.numa_buffer_pool_size,
                                                          config.enable_eviction_purge_worker, _volatile_regions,
                                                          config.migration_policy, _ssd_region, _primary_buffer_pool,
                                                          config.memory_node, _metrics->numa_buffer_pool_metrics)) {
#ifndef NDEBUG
  bump_mapping_epoch();
#endif

  if (config.enable_numa) {
#if HYRISE_NUMA_SUPPORT
    if (numa_available() < 0) {
      Fail("BufferManager configured with enable_numa=true, but NUMA is not available on this system.");
    }

    const auto max_node = numa_max_node();
    if (max_node < 1) {
#ifndef NDEBUG
      {
        void* addrs[64];
        const auto n = ::backtrace(addrs, 64);
        char** syms = ::backtrace_symbols(addrs, n);
        std::cerr << "[BM] enable_numa=true but numa_max_node=" << max_node << " -- constructor backtrace:\n";
        if (syms) {
          for (int i = 0; i < n; ++i)
            std::cerr << "  " << syms[i] << "\n";
          std::free(syms);
        }
      }
#endif
      Fail(
          "BufferManager configured with enable_numa=true, but system has fewer than 2 NUMA nodes "
          "(need nodes 0 and 1). numa_max_node=" +
          std::to_string(max_node));
    }

    if (static_cast<int>(config.cpu_node) > max_node) {
      Fail("BufferManager configured with enable_numa=true, but cpu_node=" + std::to_string(config.cpu_node) +
           " does not exist. numa_max_node=" + std::to_string(max_node));
    }

    if (static_cast<int>(config.memory_node) > max_node) {
      Fail("BufferManager configured with enable_numa=true, but memory_node=" + std::to_string(config.memory_node) +
           " does not exist. numa_max_node=" + std::to_string(max_node));
    }
#else
    Fail(
        "BufferManager configured with enable_numa=true, but Hyrise was built without NUMA support "
        "(HYRISE_NUMA_SUPPORT=0).");
#endif

    Assert(config.cpu_node != config.memory_node, "CPU and memory node must be different when NUMA is enabled");
  } else {
    Assert(config.cpu_node == config.memory_node, "CPU and memory node must be identical when NUMA is disabled");
  }
}

BufferManager::~BufferManager() {
#ifdef HYRISE_WITH_JEMALLOC
  // Drain deferred frees while tracking structures are still alive.
  JemallocMemoryResource::get().drain_deferred_bm_frees();
#endif

#ifndef NDEBUG
  _dump_tracked_allocations();
#endif
  unmap_region(_mapped_region);
}

BufferManager& BufferManager::operator=(BufferManager&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  // Release our current mapping first
  if (_mapped_region) {
    unmap_region(_mapped_region);
    _mapped_region = nullptr;
  }

  _config = std::move(other._config);
  _metrics = std::move(other._metrics);
  _volatile_regions = std::move(other._volatile_regions);
  _ssd_region = std::move(other._ssd_region);
  _primary_buffer_pool = std::move(other._primary_buffer_pool);
  _secondary_buffer_pool = std::move(other._secondary_buffer_pool);

  // Take ownership of the mapping
  _mapped_region = other._mapped_region;
  other._mapped_region = nullptr;

#ifndef NDEBUG
  bump_mapping_epoch();
#endif

  return *this;
}

BufferManager& BufferManager::get() {
  return Hyrise::get().buffer_manager;
}

#ifndef NDEBUG
void BufferManager::_track_allocation(void* ptr, std::size_t requested_bytes, const PageID& page_id) {
  if (!ptr) {
    return;
  }

  std::vector<void*> addrs;
  addrs.resize(32);
  const auto n = ::backtrace(addrs.data(), static_cast<int>(addrs.size()));
  if (n > 0) {
    addrs.resize(static_cast<size_t>(n));
  } else {
    addrs.clear();
  }

  std::lock_guard<std::mutex> lock(_alloc_track_mutex);
  _allocations_by_ptr[ptr] = AllocationRecord{requested_bytes, page_id, std::move(addrs)};
}

void BufferManager::_untrack_allocation(void* ptr) {
  if (!ptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(_alloc_track_mutex);
  _allocations_by_ptr.erase(ptr);
}

void BufferManager::_dump_tracked_allocations() const {
  std::lock_guard<std::mutex> lock(_alloc_track_mutex);
  if (_allocations_by_ptr.empty()) {
    return;
  }

  std::cerr << "[BM] WARNING: BufferManager destroyed while allocations are still alive. count="
            << _allocations_by_ptr.size() << "\n";

  size_t i = 0;
  for (const auto& [ptr, rec] : _allocations_by_ptr) {
    std::cerr << "[BM]  leak[" << i++ << "] ptr=" << ptr << " requested_bytes=" << rec.requested_bytes
              << " page_id=" << rec.page_id << "\n";

    if (!rec.backtrace_addrs.empty()) {
      char** symbols = ::backtrace_symbols(rec.backtrace_addrs.data(), static_cast<int>(rec.backtrace_addrs.size()));
      if (symbols) {
        // Skip frame 0/1 (this function + track helper) for readability
        for (size_t f = 2; f < rec.backtrace_addrs.size(); ++f) {
          std::cerr << "    " << symbols[f] << "\n";
        }
        std::free(symbols);
      } else {
        std::cerr << "    (backtrace_symbols failed)\n";
      }
    } else {
      std::cerr << "    (no backtrace)\n";
    }
  }
}
#endif

#ifndef NDEBUG
static std::string _prot_of_addr(void* addr) {
  std::ifstream maps("/proc/self/maps");
  std::string line;
  auto a = reinterpret_cast<uintptr_t>(addr);

  while (std::getline(maps, line)) {
    // format: start-end perms offset dev inode pathname
    std::istringstream iss(line);
    std::string range, perms;
    if (!(iss >> range >> perms))
      continue;

    auto dash = range.find('-');
    if (dash == std::string::npos)
      continue;

    auto start = std::stoull(range.substr(0, dash), nullptr, 16);
    auto end = std::stoull(range.substr(dash + 1), nullptr, 16);

    if (a >= start && a < end)
      return perms;  // e.g. "rw-p" or "---p"
  }
  return "<not-mapped>";
}
#endif

#ifndef NDEBUG
std::string BufferManager::debug_perms(void* addr) const {
  return _prot_of_addr(addr);
}
#endif

// TODO: This can take several templates to improve branching
void BufferManager::make_resident(const PageID page_id, const AccessIntent access_intent,
                                  const Frame::StateVersionType state_before_exclusive) {
  // TODO: retake the desiscion here if something
  // TODO: What happens for the allocate case? Inpret allocate as a write regarding mig policy -> new method for pin
  // Check if the page was freshly allocated by checking the version. In this case, we want to use either DRAM or NUMA
  const auto version = Frame::version(state_before_exclusive);
  const auto is_evicted = Frame::state(state_before_exclusive) == Frame::EVICTED;

  // Case 1: The page is already on DRAM. This is the easy case.
  if (!is_evicted && Frame::node_id(state_before_exclusive) == _primary_buffer_pool->node_id) {
    _metrics->total_hits.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  auto region = get_region(page_id);
  auto frame = region->get_frame(page_id);  // Required for updating node placement consistently

  if (!_secondary_buffer_pool->enabled) {
    // Case 3: The page is not on DRAM and we don't have it on another memory node, so we need to load it from SSD
    region->unprotect_page(page_id);
    DebugAssert(Frame::node_id(state_before_exclusive) == _primary_buffer_pool->node_id, "Not on DRAM node");
    for (auto repeat = size_t{0}; repeat < MAX_REPEAT_COUNT; ++repeat) {
      if (!_primary_buffer_pool->ensure_free_pages(page_id.size_type())) {
        yield(repeat);
        continue;
      }
      _ssd_region->read_page(page_id, region->get_page(page_id));

      // Required: page is now resident on DRAM
      frame->set_node_id(_primary_buffer_pool->node_id);

      increment_counter(_metrics->total_misses);
      increment_counter(_metrics->total_bytes_copied_from_ssd_to_dram, page_id.num_bytes());
      return;
    }
    Fail(
        "Could not allocate page on DRAM. Try increasing the buffer pool size.");  // TODO: missing quite some item in queue
  }

  // Case 4: The page is evicted anyways, decide if numa should be bypassed or not and load the page
  if (is_evicted) {
    region->unprotect_page(page_id);

    for (auto repeat = size_t{0}; repeat < MAX_REPEAT_COUNT; ++repeat) {
      const auto bypass_numa =
          !_secondary_buffer_pool->enabled ||
          (access_intent == AccessIntent::Read && _config.migration_policy.bypass_numa_during_read()) ||
          (access_intent == AccessIntent::Write && _config.migration_policy.bypass_numa_during_write());
      if (bypass_numa) {
        // Case 4.1: We bypass NUMA and load directly into DRAM
        if (!_primary_buffer_pool->ensure_free_pages(page_id.size_type())) {
          yield(repeat);
          continue;
        }

        // Only perform NUMA syscalls when NUMA is enabled at runtime.
        if (_secondary_buffer_pool->enabled) {
          region->mbind_to_numa_node(page_id, _primary_buffer_pool->node_id);
        }

        // Required
        frame->set_node_id(_primary_buffer_pool->node_id);

        increment_counter(_metrics->total_bytes_copied_from_ssd_to_dram, page_id.num_bytes());
      } else {
        // Case 4.2: We bypass load the page into NUMA
        if (!_secondary_buffer_pool->ensure_free_pages(page_id.size_type())) {
          yield(repeat);
          continue;
        }

        // Only perform NUMA syscalls when NUMA is enabled at runtime.
        if (_secondary_buffer_pool->enabled) {
          region->mbind_to_numa_node(page_id, _secondary_buffer_pool->node_id);
        }

        // Required
        frame->set_node_id(_secondary_buffer_pool->node_id);

        increment_counter(_metrics->total_bytes_copied_from_ssd_to_numa, page_id.num_bytes());
      }
      _ssd_region->read_page(page_id, region->get_page(page_id));
      increment_counter(_metrics->total_misses);
      return;
    }
    Fail("Could not allocate page on DRAM or NUMA for an evicted page. Try increasing the buffer pool sizes.");
  }

  // Case 5: thepage should be one numa, check if we want to bypass
  DebugAssert(Frame::node_id(state_before_exclusive) == _secondary_buffer_pool->node_id, "Should be on NUMA");
  for (auto repeat = size_t{0}; repeat < MAX_REPEAT_COUNT; ++repeat) {
    const auto bypass_dram =
        (access_intent == AccessIntent::Read && _config.migration_policy.bypass_dram_during_read()) ||
        (access_intent == AccessIntent::Write && _config.migration_policy.bypass_dram_during_write());

    if (bypass_dram) {
      // Case 5.1: Do nothing, stay on NUMA
      increment_counter(_metrics->total_hits);
      return;
    } else {
      // Case 5.2: Migrate to DRAM
      if (!_primary_buffer_pool->ensure_free_pages(page_id.size_type())) {
        yield(repeat);
        continue;
      }
      _secondary_buffer_pool->free_bytes(bytes_for_size_type(page_id.size_type()));

      // Only perform NUMA syscalls when NUMA is enabled at runtime.
      if (_secondary_buffer_pool->enabled) {
        region->mbind_to_numa_node(page_id, _primary_buffer_pool->node_id);
      }

      // Required: page is now on DRAM (prevents wrong refunds / double refunds later)
      frame->set_node_id(_primary_buffer_pool->node_id);

      increment_counter(_metrics->total_hits);
      increment_counter(_metrics->total_bytes_copied_from_numa_to_dram, page_id.num_bytes());
      return;
    }
  }
  Fail("Could not allocate page on DRAM. Try increasing the buffer pool size.");
}

void BufferManager::pin_shared(const PageID page_id, const AccessIntent accessIntent) {
  DebugAssert(page_id.valid(), "Invalid page id");

  increment_counter(_metrics->total_pins);
  increment_counter(_metrics->current_pins);

  const auto frame = get_region(page_id)->get_frame(page_id);
  // TODO: Another, make_resident?
  for (auto repeat = size_t{0};; ++repeat) {
    auto state_and_version = frame->state_and_version();

    switch (Frame::state(state_and_version)) {
      case Frame::LOCKED: {
        break;
      }
      // case Frame::MARKED: TODO
      case Frame::EVICTED: {
        if (frame->try_lock_exclusive(state_and_version)) {
          make_resident(page_id, accessIntent, state_and_version);
          frame->unlock_exclusive();
        }
        break;
      }
      default: {
        // TODO: Still call make resident here? we actually have many reads now
        // and we should leverage the mechanism, addtional reads would benefit
        if (frame->try_lock_shared(state_and_version)) {
          return;
        }
        break;
      }
    }
    yield(repeat);
  }
  Fail("Could not pin page for read");
}

void BufferManager::pin_exclusive(const PageID page_id) {
  DebugAssert(page_id.valid(), "Invalid page id");

  increment_counter(_metrics->total_pins);
  increment_counter(_metrics->current_pins);

  const auto frame = get_region(page_id)->get_frame(page_id);
  // TODO: Use another make_resident?
  for (auto repeat = size_t{0};; ++repeat) {
    auto state_and_version = frame->state_and_version();
    switch (Frame::state(state_and_version)) {
      case Frame::EVICTED: {
        if (frame->try_lock_exclusive(state_and_version)) {
          make_resident(page_id, AccessIntent::Write, state_and_version);
          return;
        }
        break;
      }
      case Frame::MARKED:
      case Frame::UNLOCKED: {
        if (frame->try_lock_exclusive(state_and_version)) {
          make_resident(page_id, AccessIntent::Write, state_and_version);
          return;
        }
        break;
      }
    }
    yield(repeat);
  }
  Fail("Could not pin page for write");
}

void BufferManager::unpin_shared(const PageID page_id) {
  DebugAssert(page_id.valid(), "Invalid page id");

  increment_counter(_metrics->current_pins, -1);
  auto frame = get_region(page_id)->get_frame(page_id);
  if (frame->unlock_shared()) {
    add_to_eviction_queue(page_id, frame);
  }
}

void BufferManager::unpin_exclusive(const PageID page_id) {
  DebugAssert(page_id.valid(), "Invalid page id");

  increment_counter(_metrics->current_pins, -1);
  auto frame = get_region(page_id)->get_frame(page_id);
  frame->unlock_exclusive();
  add_to_eviction_queue(page_id, frame);
}

void BufferManager::set_dirty(const PageID page_id) {
  DebugAssert(page_id.valid(), "Invalid page id");

  get_region(page_id)->get_frame(page_id)->set_dirty(true);
}

size_t BufferManager::reserved_bytes_dram_buffer_pool() const {
  return _primary_buffer_pool->used_bytes.load(std::memory_order_relaxed);
};

size_t BufferManager::reserved_bytes_numa_buffer_pool() const {
  return _secondary_buffer_pool->used_bytes.load(std::memory_order_relaxed);
};

size_t BufferManager::free_bytes_dram_node() const {
  return _primary_buffer_pool->free_bytes_node();
};

size_t BufferManager::free_bytes_numa_node() const {
  return _secondary_buffer_pool->free_bytes_node();
};

size_t BufferManager::total_bytes_dram_node() const {
  return _primary_buffer_pool->total_bytes_node();
};

size_t BufferManager::total_bytes_numa_node() const {
  return _secondary_buffer_pool->total_bytes_node();
};

BufferManager::Config BufferManager::config() const {
  return _config;
}

Frame::StateVersionType BufferManager::_state(const PageID page_id) {
  const auto frame = _volatile_regions[static_cast<uint64_t>(page_id.size_type())]->get_frame(page_id);
  return Frame::state(frame->state_and_version());
}

PageID BufferManager::find_page(const void* ptr) const {
  if (!_mapped_region || !ptr) {
    return PageID{MIN_PAGE_SIZE_TYPE, 0, false};
  }

  const auto base = reinterpret_cast<const std::byte*>(_mapped_region);
  const auto p = reinterpret_cast<const std::byte*>(ptr);

  // Reject pointers outside our reserved mapping
  if (p < base || p >= base + DEFAULT_RESERVED_VIRTUAL_MEMORY) {
    return PageID{MIN_PAGE_SIZE_TYPE, 0, false};
  }

  const auto offset = std::ptrdiff_t{p - base};
  const auto region_idx = offset / DEFAULT_RESERVED_VIRTUAL_MEMORY_PER_REGION;

  const auto valid = region_idx >= 0 && region_idx < static_cast<std::ptrdiff_t>(NUM_PAGE_SIZE_TYPES);
  if (!valid) {
    return PageID{MIN_PAGE_SIZE_TYPE, 0, false};
  }

  const auto region_idx_u = static_cast<uint64_t>(region_idx);
  const auto page_size = bytes_for_size_type(MIN_PAGE_SIZE_TYPE) * (uint64_t{1} << region_idx_u);

  const auto region_offset = offset % DEFAULT_RESERVED_VIRTUAL_MEMORY_PER_REGION;
  const auto page_idx = region_offset / static_cast<std::ptrdiff_t>(page_size);

  const auto size_type = magic_enum::enum_value<PageSizeType>(region_idx);
  return PageID{size_type, static_cast<PageID::PageIDType>(page_idx), true};
}

void BufferManager::add_to_eviction_queue(const PageID page_id, Frame* frame) {
  if (frame->node_id() == _primary_buffer_pool->node_id) {
    _primary_buffer_pool->add_to_eviction_queue(page_id, frame);
  } else if (frame->node_id() == _secondary_buffer_pool->node_id) {
    DebugAssert(_secondary_buffer_pool->enabled, "Pool has to be enabled");
    _secondary_buffer_pool->add_to_eviction_queue(page_id, frame);
  } else {
    Fail("Cannot find buffer pool for given memory node " + std::to_string(frame->node_id()));
  }
}

void* BufferManager::do_allocate(std::size_t bytes, std::size_t alignment) {
  // TODO: Check again
  if (bytes == 0) {
    bytes = 1;
  }

  const auto size_type = find_fitting_page_size_type(bytes);
  auto region = _volatile_regions[static_cast<uint64_t>(size_type)];
  const auto [page_id, frame, ptr] = region->allocate();

#ifndef NDEBUG
  {
    std::lock_guard<std::mutex> lock(s_live_pages_mutex);
    const auto key = live_page_key(page_id);
    if (!s_live_pages.insert(key).second) {
      Fail("BufferManager::do_allocate handed out an already-live page_id=" +
           std::string(magic_enum::enum_name(page_id.size_type())) + " idx=" + std::to_string(page_id.index));
    }
  }
#endif

  increment_counter(_metrics->num_allocs);
  increment_counter(_metrics->total_allocated_bytes, bytes_for_size_type(size_type));

  auto state_and_version = frame->state_and_version();
  if (!frame->try_lock_exclusive(state_and_version)) {
    Fail("Could not lock page for exclusive access during allocation.");
  }

  region->unprotect_page(page_id);

  // Use either NUMA or DRAM for allocation
  // TODO: Which one?
  auto buffer_pool =
      (_secondary_buffer_pool && _secondary_buffer_pool->enabled && _config.migration_policy.bypass_dram_during_write())
          ? _secondary_buffer_pool
          : _primary_buffer_pool;

  for (auto repeat = size_t{0}; repeat < MAX_REPEAT_COUNT; ++repeat) {
    if (!buffer_pool->ensure_free_pages(page_id.size_type())) {
      yield(repeat);
      continue;
    }

    // Only perform NUMA syscalls when NUMA is enabled at runtime.
    if (_secondary_buffer_pool && _secondary_buffer_pool->enabled) {
      region->mbind_to_numa_node(page_id, buffer_pool->node_id);
    }

    // Required: make accounting and deallocation consistent
    frame->set_node_id(buffer_pool->node_id);

#ifndef NDEBUG
    bool in_extent_hooks_inner = false;
#if defined(HYRISE_WITH_JEMALLOC)
    in_extent_hooks_inner = jemalloc_extent_hooks_tls_flag();
#endif
    if (!in_extent_hooks_inner) {
      _track_allocation(ptr, bytes, page_id);
    }
#endif

#ifndef NDEBUG
    volatile std::byte* probe = ptr;
    probe[0] = std::byte{0xAB};
#endif

    frame->unlock_exclusive();
    return ptr;
  }

  frame->unlock_exclusive_and_set_evicted();
  region->deallocate(page_id);

  Fail("Could not allocate page on NUMA or DRAM. Try increasing buffer pool sizes.");
}

void BufferManager::do_deallocate(void* p, std::size_t bytes, std::size_t alignment) {
  // TODO: Check again
  if (!p) {
    return;
  }

#ifndef NDEBUG
  _untrack_allocation(p);
#endif

  const auto page_id = find_page(p);
  if (!page_id.valid()) {
    Fail("BufferManager::do_deallocate called with pointer not belonging to mapped region");
  }

#ifndef NDEBUG
  {
    std::lock_guard<std::mutex> lock(s_live_pages_mutex);
    const auto key = live_page_key(page_id);

    if (s_live_pages.erase(key) == 0) {
      // If we're deallocating exactly the base of a BM page with the full page size,
      // this can happen during reset/drain of deferred frees (jemalloc extent hooks)
      // or after mapping-epoch changes. Treat as non-fatal in debug.
      const auto expected_base = page_base_ptr(page_id);
      const auto expected_size = bytes_for_size_type(page_id.size_type());

      if (p == expected_base && bytes == expected_size) {
        std::cerr << "[BM] WARNING: do_deallocate saw untracked page-base free (likely deferred/reset). "
                  << "page_id=" << page_id << " ptr=" << p << " bytes=" << bytes << "\n";
      } else {
        std::ostringstream oss;
        oss << "BufferManager::do_deallocate detected double-free or invalid free for page_id=" << page_id
            << " ptr=" << p << " bytes_arg=" << bytes;
        Fail(oss.str());
      }
    }
  }
#endif

  auto region = get_region(page_id);
  auto frame = region->get_frame(page_id);

  auto state_and_version = frame->state_and_version();
  while (Frame::state(state_and_version) != Frame::LOCKED) {
    if (frame->try_lock_exclusive(state_and_version)) {
      break;
    }
    yield(0);
    state_and_version = frame->state_and_version();
  }

  const auto num_bytes = bytes_for_size_type(page_id.size_type());

  // pmr::memory_resource::deallocate() is called with the originally requested size, not necessarily the backing size.
  // We allocate whole pages, so the deallocation size may be smaller than the page size.
  DebugAssert(bytes <= num_bytes, "do_deallocate size mismatch: arg bytes=" + std::to_string(bytes) +
                                      " but page size=" + std::to_string(num_bytes) +
                                      " size_type=" + std::string(magic_enum::enum_name(page_id.size_type())));

  if (frame->node_id() == _primary_buffer_pool->node_id) {
    _primary_buffer_pool->free_bytes(num_bytes);
  } else if (_secondary_buffer_pool && _secondary_buffer_pool->enabled &&
             frame->node_id() == _secondary_buffer_pool->node_id) {
    _secondary_buffer_pool->free_bytes(num_bytes);
  } else {
    Fail("Cannot find buffer pool for given memory node " + std::to_string(frame->node_id()));
  }

  region->free(page_id);
  region->deallocate(page_id);
  frame->unlock_exclusive_and_set_evicted();

  increment_counter(_metrics->num_deallocs);
}

bool BufferManager::do_is_equal(const boost::container::pmr::memory_resource& other) const noexcept {
  return this == &other;
}

std::shared_ptr<VolatileRegion> BufferManager::get_region(const PageID page_id) {
  return _volatile_regions[static_cast<uint64_t>(page_id.size_type())];
}

std::byte* BufferManager::_get_page_ptr(const PageID page_id) {
  return get_region(page_id)->get_page(page_id);
}

std::shared_ptr<BufferManagerMetrics> BufferManager::metrics() {
  return _metrics;
}

size_t BufferManager::memory_consumption() const {
  const auto volatile_regions_bytes =
      std::accumulate(_volatile_regions.begin(), _volatile_regions.end(), 0,
                      [](auto acc, auto region) { return acc + region->memory_consumption(); });
  const auto ssd_region_bytes = _ssd_region->memory_consumption();
  const auto buffer_pool_bytes = _primary_buffer_pool->memory_consumption();
  const auto secondary_buffer_pool_bytes = _secondary_buffer_pool->memory_consumption();
  const auto metrics_bytes = sizeof(*_metrics);
  return sizeof(*this) + volatile_regions_bytes + ssd_region_bytes + buffer_pool_bytes + secondary_buffer_pool_bytes +
         metrics_bytes;
}

size_t BufferManager::pool_size() const {
  return _primary_buffer_pool->max_bytes + (_secondary_buffer_pool->enabled ? _secondary_buffer_pool->max_bytes : 0);
}

// TODO: Required?
std::byte* BufferManager::page_base_ptr(const PageID page_id) const {
  DebugAssert(page_id.valid(), "Invalid page id");
  DebugAssert(_mapped_region != nullptr, "BufferManager mapping not initialized");

  const auto region_idx =
      static_cast<size_t>(page_id.size_type());  // relies on enum order matching regions (as in find_page)
  const auto page_size = bytes_for_size_type(page_id.size_type());

  const auto region_base = _mapped_region + region_idx * DEFAULT_RESERVED_VIRTUAL_MEMORY_PER_REGION;
  return region_base + static_cast<size_t>(page_id.index) * page_size;
}

}  // namespace hyrise
