#include "lru_eviction.hpp"

#include "storage/buffer/buffer_pool.hpp"
#include "storage/buffer/eviction_strategy_registry.hpp"

namespace hyrise {

namespace {
EvictionStrategyRegistrar s_lru_registrar{
    "lru",
    [](BufferPool& buffer_pool) { return std::make_unique<LruEviction>(buffer_pool); },
    {"LRU", "pure-lru", "pure_lru"}};
}  // namespace

LruEviction::LruEviction(BufferPool& buffer_pool) : EvictionStrategy(buffer_pool) {}

void LruEviction::_touch_locked(Iterator it) {
  // Splice node to the end (MRU) in O(1).
  _lru.splice(_lru.end(), _lru, it);
}

void LruEviction::_erase_locked(Iterator it) {
  _index.erase(it->page_id);
  _lru.erase(it);
}

void LruEviction::add_eviction_candidate(const PageID& page_id, Frame* const frame) {
  const auto current_state_and_version = frame->state_and_version();
  DebugAssert(frame->node_id() == _buffer_pool.node_id, "Memory node mismatch");
  increment_counter(_buffer_pool.metrics->num_eviction_queue_adds);

  const auto ts = Frame::version(current_state_and_version);

  std::lock_guard<std::mutex> lk(_mutex);

  // If already tracked, update timestamp and move to MRU.
  const auto it = _index.find(page_id);
  if (it != _index.end()) {
    it->second->timestamp = ts;
    _touch_locked(it->second);
    return;
  }

  _lru.push_back({page_id, ts});
  auto node_it = std::prev(_lru.end());
  _index.emplace(page_id, node_it);
}

void LruEviction::on_access(const PageID& page_id, Frame* const /*frame*/) {
  // Pure LRU: touch on every access.
  std::lock_guard<std::mutex> lk(_mutex);
  const auto it = _index.find(page_id);
  if (it == _index.end()) return;
  _touch_locked(it->second);
}

bool LruEviction::perform_evictions(const PageSizeType required_size) {
  const auto bytes_required = bytes_for_size_type(required_size);
  auto freed_bytes = size_t{0};

  _buffer_pool.reserve_bytes(bytes_required);

  bool counted_episode = false;

  while (_buffer_pool.used_bytes.load(std::memory_order_relaxed) > _buffer_pool.max_bytes) {
    if (!counted_episode) {
      increment_counter(_buffer_pool.metrics->num_oversubscription_episodes);
      counted_episode = true;
    }

    EvictionItem item{};
    {
      std::lock_guard<std::mutex> lk(_mutex);
      if (_lru.empty()) {
#ifndef NDEBUG
        std::cerr << "[BM][DEBUG] ensure_free_pages failed: LRU list empty"
                  << " required_bytes=" << bytes_required
                  << " used_bytes=" << _buffer_pool.used_bytes.load(std::memory_order_relaxed)
                  << " max_bytes=" << _buffer_pool.max_bytes << " freed_bytes=" << freed_bytes
                  << " node_id=" << _buffer_pool.node_id << "\n";
#endif
        increment_counter(_buffer_pool.metrics->num_eviction_failures);
        _buffer_pool.free_bytes(bytes_required);
        return false;
      }

      item = _lru.front();
    }

    increment_counter(_buffer_pool.metrics->num_eviction_candidate_inspections);

    const auto region = _buffer_pool.volatile_regions[static_cast<uint64_t>(item.page_id.size_type())];
    auto frame = region->get_frame(item.page_id);
    const auto current_state_and_version = frame->state_and_version();

    // If migrated or stale, drop from LRU.
    if (frame->node_id() != _buffer_pool.node_id || Frame::version(current_state_and_version) != item.timestamp) {
      std::lock_guard<std::mutex> lk(_mutex);
      const auto it = _index.find(item.page_id);
      if (it != _index.end()) {
        // Only erase if the stored node matches the version we just inspected.
        // If it differs, it has been updated/reinserted; keep the newer one.
        if (it->second->timestamp == item.timestamp) {
          _erase_locked(it->second);
        }
      }
      increment_counter(_buffer_pool.metrics->num_eviction_queue_items_purged);
      continue;
    }

    // Keep pinned frames; move to MRU to avoid immediate re-check.
    if (Frame::state(current_state_and_version) != Frame::UNLOCKED) {
      increment_counter(_buffer_pool.metrics->num_eviction_requeues_pinned);
      std::lock_guard<std::mutex> lk(_mutex);
      const auto it = _index.find(item.page_id);
      if (it != _index.end() && it->second->timestamp == item.timestamp) {
        _touch_locked(it->second);
      }
      continue;
    }

    // Try locking the frame exclusively
    if (!frame->try_lock_exclusive(current_state_and_version)) {
      increment_counter(_buffer_pool.metrics->num_eviction_requeues_lock_failed);
      std::lock_guard<std::mutex> lk(_mutex);
      const auto it = _index.find(item.page_id);
      if (it != _index.end() && it->second->timestamp == item.timestamp) {
        _touch_locked(it->second);
      }
      continue;
    }

    Assert(frame->node_id() == _buffer_pool.node_id,
           "Memory node mismatch: " + std::to_string(frame->node_id()) + " != " + std::to_string(_buffer_pool.node_id));

    // Remove from LRU under lock, then evict without holding LRU lock.
    {
      std::lock_guard<std::mutex> lk(_mutex);
      const auto it = _index.find(item.page_id);
      if (it == _index.end() || it->second->timestamp != item.timestamp) {
        frame->unlock_exclusive();
        // Stale/updated since inspection; try again.
        increment_counter(_buffer_pool.metrics->num_eviction_queue_items_purged);
        continue;
      }
      _erase_locked(it->second);
    }

    _buffer_pool.evict(item, frame);
    increment_counter(_buffer_pool.metrics->num_evictions);

    const auto evicted_bytes = bytes_for_size_type(item.page_id.size_type());
    freed_bytes += evicted_bytes;
    _buffer_pool.free_bytes(evicted_bytes);
  }

  return true;
}

void LruEviction::purge_eviction_candidates() {
  std::lock_guard<std::mutex> lk(_mutex);

  auto purged = size_t{0};
  auto it = _lru.begin();
  while (it != _lru.end() && purged < MAX_EVICTION_QUEUE_PURGES) {
    const auto item = *it;

    const auto region = _buffer_pool.volatile_regions[static_cast<uint64_t>(item.page_id.size_type())];
    auto frame = region->get_frame(item.page_id);
    const auto current_state_and_version = frame->state_and_version();

    // Purge stale or migrated entries. Keep pinned/ref entries.
    if (frame->node_id() != _buffer_pool.node_id || Frame::version(current_state_and_version) != item.timestamp) {
      auto erase_it = it++;
      // Only erase if the mapping still points to this exact node/version.
      const auto map_it = _index.find(item.page_id);
      if (map_it != _index.end() && map_it->second == erase_it && map_it->second->timestamp == item.timestamp) {
        _erase_locked(erase_it);
      } else {
        _lru.erase(erase_it);
      }
      increment_counter(_buffer_pool.metrics->num_eviction_queue_items_purged);
      ++purged;
      continue;
    }

    ++it;
  }
}

std::size_t LruEviction::memory_consumption() const {
  std::lock_guard<std::mutex> lk(_mutex);
  return sizeof(*this) + sizeof(_lru) + sizeof(List::value_type) * _lru.size() + sizeof(_index) +
         sizeof(decltype(_index)::value_type) * _index.size();
}

}  // namespace hyrise
