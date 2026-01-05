#include "jemalloc_resource.hpp"
#ifdef HYRISE_WITH_JEMALLOC
#include <jemalloc/jemalloc.h>
#endif
#include <cstddef>
#include <cstdint>
#include <string>
#include "hyrise.hpp"
#include "utils/assert.hpp"

#ifdef HYRISE_WITH_JEMALLOC

#ifdef HYRISE_WITH_JEMALLOC
#include <mutex>
#include <unordered_map>
#ifndef NDEBUG
#include <iostream>
#include <sstream>
#endif
#endif

namespace {
// TODO: Mostly likely not needed
// from jemalloc internals (see arena_types.h)
struct arena_config_s {
  /* extent hooks to be used for the arena */
  extent_hooks_t* extent_hooks;

  bool metadata_use_hooks = false;
};

using arena_config_t = struct arena_config_s;

}  // namespace
#endif

namespace hyrise {

  // TODO: Required?
#ifdef HYRISE_WITH_JEMALLOC
bool& jemalloc_extent_hooks_tls_flag() {
  static thread_local bool v = false;
  return v;
}
#endif

#ifdef HYRISE_WITH_JEMALLOC


// TODO: Required?

struct ExtentRec {
  size_t size;          // current extent size (changes on split/merge)
  size_t alignment;
  unsigned arena_index;

  void* page_base;      // BufferManager page base this extent belongs to
  size_t page_size;     // BufferManager page size in bytes

  bool live = true;     // IMPORTANT: never erase in hooks; mark dead instead
};

static std::mutex s_extents_mutex;
static std::unordered_map<void*, ExtentRec> s_extents;          // extent addr -> metadata
static std::unordered_map<void*, size_t> s_page_live_extents;   // page_base -> number of live extents on this BM page
constexpr size_t kPendingFreeCap = 1u << 16;

// TODO: Required?
struct PendingFreeQueue {
  // Store both base + size so draining does not need to scan s_extents (which may contain stale/dead records).
  struct Item {
    void* base{};
    size_t size{};
  };

  Item items[kPendingFreeCap]{};
  std::atomic<size_t> head{0};
  std::atomic<size_t> tail{0};

  void push(void* base, size_t size) {
    const auto t = tail.load(std::memory_order_relaxed);
    const auto n = (t + 1) % kPendingFreeCap;
    if (n == head.load(std::memory_order_acquire)) {
      // full -> drop (safe fallback: BM page will be returned later or leak until exit)
      return;
    }
    items[t] = Item{base, size};
    tail.store(n, std::memory_order_release);
  }

  bool pop(void*& base_out, size_t& size_out) {
    const auto h = head.load(std::memory_order_relaxed);
    if (h == tail.load(std::memory_order_acquire)) return false;
    base_out = items[h].base;
    size_out = items[h].size;
    head.store((h + 1) % kPendingFreeCap, std::memory_order_release);
    return true;
  }
};

static PendingFreeQueue s_pending_page_frees;

// TODO: Required?
static void drain_deferred_bm_frees() {
  void* page_base = nullptr;
  size_t page_size = 0;

  while (s_pending_page_frees.pop(page_base, page_size)) {
    if (!page_base || page_size == 0) continue;

    // Validate against the CURRENT BufferManager mapping. The pending queue is global and can contain
    // stale pointers from previous mappings / resets. Never call BM::deallocate on those.
    auto& bm = Hyrise::get().buffer_manager;
    const auto page_id = bm.find_page(page_base);
    if (!page_id.valid()) {
      // stale pointer -> ignore
      continue;
    }

    // Ensure this is actually a base pointer of that page in the current mapping.
    if (bm.page_base_ptr(page_id) != page_base) {
      continue;
    }

    // Use the page size recorded at enqueue time (avoids scanning s_extents).
    bm.deallocate(page_base, page_size);
  }
}

#ifndef NDEBUG
static void debug_extent_msg(const char* what, void* addr, size_t hook_size, const ExtentRec* rec) {
  std::ostringstream oss;
  oss << "[BM][JEMALLOC] " << what
      << " addr=" << addr
      << " hook_size=" << hook_size;
  if (rec) {
    oss << " tracked_size=" << rec->size
        << " tracked_align=" << rec->alignment
        << " arena=" << rec->arena_index;
  }
  oss << "\n";
  std::cerr << oss.str();
}
#endif

static bool find_extent_rec(void* addr, ExtentRec& rec_out) {
  std::lock_guard<std::mutex> lock(s_extents_mutex);
  const auto it = s_extents.find(addr);
  if (it == s_extents.end()) return false;
  rec_out = it->second;
  return true;
}

// Checks whether addr is inside the BufferManager reserved virtual memory mapping.
// We cannot call BufferManager::find_page here safely if Hyrise/BufferManager might be destructing,
// so we do a conservative range check based on BufferManager's mapping base pointer.
static bool _addr_in_buffer_manager_mapping(void* addr) {
  if (!addr) {
    return false;
  }
  const auto& bm = Hyrise::get().buffer_manager;
  const auto page_id = bm.find_page(addr);
  return page_id.valid();
}

// TODO: Required?
static bool _page_info_for_addr(void* addr, void*& page_base_out, size_t& page_size_out) {
  if (!addr) return false;

  const auto& bm = Hyrise::get().buffer_manager;
  const auto page_id = bm.find_page(addr);
  if (!page_id.valid()) return false;

  page_base_out = bm.page_base_ptr(page_id);
  page_size_out = bytes_for_size_type(page_id.size_type());
  return true;
}

// TODO: Required?
static void _track_new_extent_unlocked(void* extent_addr, size_t extent_size, size_t alignment, unsigned arena_index) {
  void* page_base = nullptr;
  size_t page_size = 0;
  const bool ok = _page_info_for_addr(extent_addr, page_base, page_size);
  Assert(ok, "Trying to track extent not belonging to BufferManager mapping");

#ifndef NDEBUG
  if (s_extents.find(extent_addr) != s_extents.end()) {
    std::cerr << "[BM][JEMALLOC] WARNING: extent_alloc returned an address already tracked: " << extent_addr << "\n";
  }
#endif

  s_extents[extent_addr] = ExtentRec{extent_size, alignment, arena_index, page_base, page_size};
  s_page_live_extents[page_base] += 1;
}

// TODO: Required?
static bool _untrack_extent_and_maybe_free_page_unlocked(void* extent_addr) {
  const auto it = s_extents.find(extent_addr);
  if (it == s_extents.end()) return false;

  auto& rec = it->second;

  // Never free nodes inside jemalloc hooks.
  if (!rec.live) return false;
  rec.live = false;

  auto pit = s_page_live_extents.find(rec.page_base);
  if (pit == s_page_live_extents.end()) {
    // Should not happen; keep going without crashing inside hooks.
    return true;
  }

  if (pit->second == 0) {
    // Already at 0; keep going without crashing inside hooks.
    return true;
  }

  pit->second -= 1;

  // If last subextent is gone, defer page return to a safe context.
  if (pit->second == 0) {
    s_pending_page_frees.push(rec.page_base, rec.page_size);
  }

  return true;
}

// Fallback to jemalloc's default extent hooks (usually arena 0).
// Needed because our hooks intentionally decline extents larger than MAX_PAGE_SIZE_TYPE.
// jemalloc may allocate those via the default allocator and will still call our dalloc/destroy later.
// TODO: Required?
static extent_hooks_t* s_fallback_hooks = nullptr;

static extent_hooks_t* _get_fallback_hooks() {
  if (s_fallback_hooks) {
    return s_fallback_hooks;
  }

  // Query default hooks from arena 0
  size_t sz = sizeof(s_fallback_hooks);
  Assert(mallctl("arena.0.extent_hooks", &s_fallback_hooks, &sz, nullptr, 0) == 0,
         "mallctl arena.0.extent_hooks failed");
  Assert(s_fallback_hooks, "arena.0.extent_hooks returned nullptr");

  return s_fallback_hooks;
}

// TODO: Check again
static void* extent_alloc(extent_hooks_t* /*extent_hooks*/, void* new_addr, size_t size, size_t alignment, bool* zero,
                          bool* commit, unsigned arena_index) {
  if (size > bytes_for_size_type(MAX_PAGE_SIZE_TYPE)) {
    auto* fallback = _get_fallback_hooks();
    return fallback->alloc ? fallback->alloc(fallback, new_addr, size, alignment, zero, commit, arena_index) : nullptr;
  }

  if (commit) *commit = true;
  if (zero) *zero = false;

  // Mark this allocation as coming from jemalloc's extent hooks so BufferManager debug tracking
  // does not treat it like a user allocation (avoids false "leak" reports on shutdown/reset).
  struct ExtentHookGuard {
    ExtentHookGuard() { jemalloc_extent_hooks_tls_flag() = true; }
    ~ExtentHookGuard() { jemalloc_extent_hooks_tls_flag() = false; }
  } guard;

  auto* ptr = Hyrise::get().buffer_manager.allocate(size, alignment);
  if (!ptr) return nullptr;

  {
    std::lock_guard<std::mutex> lock(s_extents_mutex);
    _track_new_extent_unlocked(ptr, size, alignment, arena_index);
  }

#ifndef NDEBUG
  debug_extent_msg("extent_alloc", ptr, size, nullptr);
#endif

  return ptr;
}

// TODO: Check again
bool extent_dalloc(extent_hooks_t* /*extent_hooks*/, void* addr, size_t size, bool committed, unsigned arena_ind) {
#ifndef NDEBUG
  debug_extent_msg("extent_dalloc(raw)", addr, size, nullptr);
#endif

  if (_addr_in_buffer_manager_mapping(addr)) {
    std::lock_guard<std::mutex> lock(s_extents_mutex);
    const auto ok = _untrack_extent_and_maybe_free_page_unlocked(addr);

#ifndef NDEBUG
    if (!ok) {
      debug_extent_msg("extent_dalloc DOUBLE/UNKNOWN (ignored)", addr, size, nullptr);
    } else {
      debug_extent_msg("extent_dalloc", addr, size, nullptr);
    }
#endif
    return false;  // success
  }

  auto* fallback = _get_fallback_hooks();
  if (fallback->dalloc) return fallback->dalloc(fallback, addr, size, committed, arena_ind);
  return true;
}

// TODO: Check again
static void extent_destroy(extent_hooks_t* /*extent_hooks*/, void* addr, size_t size, bool committed, unsigned arena_ind) {
#ifndef NDEBUG
  debug_extent_msg("extent_destroy(raw)", addr, size, nullptr);
#endif

  if (_addr_in_buffer_manager_mapping(addr)) {
    std::lock_guard<std::mutex> lock(s_extents_mutex);
    const auto ok = _untrack_extent_and_maybe_free_page_unlocked(addr);

#ifndef NDEBUG
    if (!ok) {
      debug_extent_msg("extent_destroy DOUBLE/UNKNOWN (ignored)", addr, size, nullptr);
    } else {
      debug_extent_msg("extent_destroy", addr, size, nullptr);
    }
#endif
    return;
  }

  auto* fallback = _get_fallback_hooks();
  if (fallback->destroy) fallback->destroy(fallback, addr, size, committed, arena_ind);
}

// TODO: Check again
static bool extent_commit(extent_hooks_t* /*extent_hooks*/, void* /*addr*/, size_t /*size*/, size_t /*offset*/,
                          size_t /*length*/, unsigned /*arena_ind*/) {
  return false;
}

// TODO: Check again
static bool extent_purge_lazy(extent_hooks_t* /*extent_hooks*/, void* /*addr*/, size_t /*size*/, size_t /*offset*/,
                              size_t /*length*/, unsigned /*arena_ind*/) {
  return false;
}

// TODO: Check again
static bool extent_purge(extent_hooks_t* /*extent_hooks*/, void* /*addr*/, size_t /*size*/, size_t /*offset*/,
                         size_t /*length*/, unsigned /*arena_ind*/) {
  return false;
}

// TODO: Check again
static bool extent_split(extent_hooks_t* extent_hooks, void* addr, size_t size, size_t sizea, size_t sizeb,
                         bool committed, unsigned arena_ind) {
#ifndef NDEBUG
  debug_extent_msg("extent_split(raw)", addr, size, nullptr);
#endif

  // If this isn't ours, delegate to fallback if possible.
  if (!_addr_in_buffer_manager_mapping(addr)) {
    auto* fallback = _get_fallback_hooks();
    if (fallback && fallback->split) {
      return fallback->split(fallback, addr, size, sizea, sizeb, committed, arena_ind);
    }
    return true;  // failure / opt-out
  }

  std::lock_guard<std::mutex> lock(s_extents_mutex);

  const auto it = s_extents.find(addr);
  if (it == s_extents.end()) {
#ifndef NDEBUG
    debug_extent_msg("extent_split UNKNOWN (refuse)", addr, size, nullptr);
#endif
    return true;  // refuse split -> jemalloc must handle differently
  }

  const auto rec = it->second;

  // Sanity: expected sizes
  if (rec.size != size) {
#ifndef NDEBUG
    debug_extent_msg("extent_split size_mismatch (refuse)", addr, size, &rec);
#endif
    return true;
  }

  // Replace [addr -> size] with [addr -> sizea] and [addr + sizea -> sizeb]
  // Also update page refcount: one extent becomes two => +1 live extent for the page.
  s_extents.erase(it);

  s_extents[addr] = ExtentRec{sizea, rec.alignment, rec.arena_index, rec.page_base, rec.page_size};

  auto* addr_b = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(addr) + sizea);
  s_extents[addr_b] = ExtentRec{sizeb, rec.alignment, rec.arena_index, rec.page_base, rec.page_size};

  s_page_live_extents[rec.page_base] += 1;

#ifndef NDEBUG
  debug_extent_msg("extent_split", addr, size, &rec);
#endif

  return false;  // success
}

// TODO: Check again
static bool extent_merge(extent_hooks_t* extent_hooks, void* addr_a, size_t size_a, void* addr_b, size_t size_b,
                         bool committed, unsigned arena_ind) {
#ifndef NDEBUG
  debug_extent_msg("extent_merge(raw)", addr_a, size_a, nullptr);
#endif

  // If not ours, delegate to fallback if possible.
  if (!_addr_in_buffer_manager_mapping(addr_a) && !_addr_in_buffer_manager_mapping(addr_b)) {
    auto* fallback = _get_fallback_hooks();
    if (fallback && fallback->merge) {
      return fallback->merge(fallback, addr_a, size_a, addr_b, size_b, committed, arena_ind);
    }
    return true;  // failure / opt-out
  }

  std::lock_guard<std::mutex> lock(s_extents_mutex);

  const auto it_a = s_extents.find(addr_a);
  const auto it_b = s_extents.find(addr_b);
  if (it_a == s_extents.end() || it_b == s_extents.end()) {
#ifndef NDEBUG
    debug_extent_msg("extent_merge UNKNOWN (refuse)", addr_a, size_a, nullptr);
#endif
    return true;
  }

  const auto rec_a = it_a->second;
  const auto rec_b = it_b->second;

  if (rec_a.size != size_a || rec_b.size != size_b) {
#ifndef NDEBUG
    debug_extent_msg("extent_merge size_mismatch (refuse)", addr_a, size_a, &rec_a);
#endif
    return true;
  }

  // Basic adjacency check (jemalloc should only merge adjacent extents)
  const auto a_u = reinterpret_cast<std::uintptr_t>(addr_a);
  const auto b_u = reinterpret_cast<std::uintptr_t>(addr_b);
  if (a_u + size_a != b_u) {
#ifndef NDEBUG
    debug_extent_msg("extent_merge not_adjacent (refuse)", addr_a, size_a, &rec_a);
#endif
    return true;
  }

  // Must belong to the same BM page
  if (rec_a.page_base != rec_b.page_base || rec_a.page_size != rec_b.page_size) {
#ifndef NDEBUG
    debug_extent_msg("extent_merge cross_page (refuse)", addr_a, size_a, &rec_a);
#endif
    return true;
  }

  // Remove both and insert merged at addr_a; adjust page refcount: two extents become one => -1.
  s_extents.erase(it_a);
  s_extents.erase(it_b);
  s_extents[addr_a] = ExtentRec{size_a + size_b, rec_a.alignment, rec_a.arena_index, rec_a.page_base, rec_a.page_size};

  auto pit = s_page_live_extents.find(rec_a.page_base);
  Assert(pit != s_page_live_extents.end() && pit->second >= 2, "page live extent counter underflow on merge");
  pit->second -= 1;

#ifndef NDEBUG
  debug_extent_msg("extent_merge", addr_a, size_a + size_b, &rec_a);
#endif

  return false;  // success
}

static extent_hooks_t s_hooks{extent_alloc,      extent_dalloc, extent_destroy, extent_commit, nullptr,
                              extent_purge_lazy, extent_purge,  extent_split,   extent_merge};
#endif

// TODO: Required?
JemallocMemoryResource::JemallocMemoryResource() {
  create_arena();
}

// TODO: Required?
#ifdef HYRISE_WITH_JEMALLOC
void JemallocMemoryResource::drain_deferred_bm_frees() {
  ::hyrise::drain_deferred_bm_frees();  // if the static is in namespace hyrise
}
#endif

JemallocMemoryResource::~JemallocMemoryResource() {}

// TODO: Check again
void JemallocMemoryResource::create_arena() {
#ifdef HYRISE_WITH_JEMALLOC
  (void)_get_fallback_hooks();

  // Create a normal arena
  unsigned arena_id = 0;
  size_t sz = sizeof(arena_id);
  Assert(mallctl("arenas.create", &arena_id, &sz, nullptr, 0) == 0, "arenas.create failed");

  // Install extent hooks
  extent_hooks_t* hooks_ptr = &s_hooks;
  const std::string hooks_cmd = "arena." + std::to_string(arena_id) + ".extent_hooks";
  Assert(mallctl(hooks_cmd.c_str(), nullptr, nullptr, &hooks_ptr, sizeof(hooks_ptr)) == 0, "set extent_hooks failed");

  _arena_index = arena_id;

  // Make decay immediate to reduce retained dirty/muzzy pages (helps with mapping resets)
  ssize_t dirty_decay_ms = 0;
  const std::string dirty_cmd = "arena." + std::to_string(_arena_index) + ".dirty_decay_ms";
  Assert(mallctl(dirty_cmd.c_str(), nullptr, nullptr, &dirty_decay_ms, sizeof(dirty_decay_ms)) == 0,
         "setting dirty_decay_ms failed");

  ssize_t muzzy_decay_ms = 0;
  const std::string muzzy_cmd = "arena." + std::to_string(_arena_index) + ".muzzy_decay_ms";
  Assert(mallctl(muzzy_cmd.c_str(), nullptr, nullptr, &muzzy_decay_ms, sizeof(muzzy_decay_ms)) == 0,
         "setting muzzy_decay_ms failed");

  _mallocx_flags = MALLOCX_ARENA(_arena_index) | MALLOCX_TCACHE_NONE;
#endif
}

// TODO: Check again
void JemallocMemoryResource::reset() {
#ifdef HYRISE_WITH_JEMALLOC
  // Drain deferred BM frees while we're NOT inside jemalloc extent hooks.
  drain_deferred_bm_frees();

  // Flush per-thread tcache (even though TCACHE_NONE is set, this is harmless and helps if configs change)
  Assert(mallctl("thread.tcache.flush", nullptr, nullptr, nullptr, 0) == 0, "tcache.flush failed");

  // Detach *current* thread from this arena (best-effort; other threads may still be attached)
  {
    unsigned zero = 0;
    (void)mallctl("thread.arena", nullptr, nullptr, &zero, sizeof(zero));
  }

  // Ask jemalloc to purge unused pages for this arena (best-effort)
  {
    const std::string purge_cmd = "arena." + std::to_string(_arena_index) + ".purge";
    (void)mallctl(purge_cmd.c_str(), nullptr, nullptr, nullptr, 0);
  }

  // Try to destroy the arena to drop metadata + cached extents
  {
    const std::string destroy_cmd = "arena." + std::to_string(_arena_index) + ".destroy";
    const int rc = mallctl(destroy_cmd.c_str(), nullptr, nullptr, nullptr, 0);
    Assert(rc == 0, "arena.destroy failed (likely threads still associated via thread.arena)");
  }

  // Recreate fresh arena + hooks
  create_arena();
#endif
}

void* JemallocMemoryResource::do_allocate(std::size_t bytes, std::size_t alignment) {
#ifdef HYRISE_WITH_JEMALLOC
  if (auto ptr = mallocx(bytes, MALLOCX_ALIGN(alignment) | _mallocx_flags)) {
    return ptr;
  }
  Fail("Failed to allocate memory (" + std::to_string(bytes) + ")");
#else
  Fail("Jeamlloc is not supported");
#endif
}

void JemallocMemoryResource::do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) {
#ifdef HYRISE_WITH_JEMALLOC
  sdallocx(pointer, bytes, MALLOCX_ALIGN(alignment) | _mallocx_flags);
#else
  Fail("Jeamlloc is not supported");
#endif
}

bool JemallocMemoryResource::do_is_equal(const memory_resource& other) const noexcept {
  return &other == this;
}

}  // namespace hyrise
