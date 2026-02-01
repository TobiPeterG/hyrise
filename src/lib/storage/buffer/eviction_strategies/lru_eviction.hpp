#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <unordered_map>

#include <storage/buffer/eviction_strategy.hpp>
#include <storage/buffer/helper.hpp>

namespace hyrise {

class LruEviction : public EvictionStrategy {
 public:
  explicit LruEviction(BufferPool& buffer_pool);

  bool perform_evictions(PageSizeType required_size) override;
  void add_eviction_candidate(const PageID& page_id, Frame* frame) override;
  void on_access(const PageID& page_id, Frame* frame) override;
  void purge_eviction_candidates() override;
  std::size_t memory_consumption() const override;

 private:
  using List = std::list<EvictionItem>;
  using Iterator = List::iterator;

  struct PageIDHasher {
    std::size_t operator()(const PageID& pid) const noexcept {
      // Pack bitfields into a stable 64-bit key
      const uint64_t valid = static_cast<uint64_t>(pid._valid) & 0x1ull;
      const uint64_t st = static_cast<uint64_t>(pid._size_type) & ((1ull << PAGE_SIZE_TYPE_BITS) - 1ull);
      const uint64_t idx = static_cast<uint64_t>(pid.index);

      const uint64_t key = (idx << (PAGE_SIZE_TYPE_BITS + 1)) | (st << 1) | valid;
      return std::hash<uint64_t>{}(key);
    }
  };

  // Move iterator to MRU (back). Caller holds _mutex.
  void _touch_locked(Iterator it);

  // Remove and erase mapping. Caller holds _mutex.
  void _erase_locked(Iterator it);

  // LRU list: front = LRU, back = MRU.
  List _lru;

  // Maps a page_id to its current node in _lru (hash-based).
  std::unordered_map<PageID, Iterator, PageIDHasher> _index;

  mutable std::mutex _mutex;
};

}  // namespace hyrise
