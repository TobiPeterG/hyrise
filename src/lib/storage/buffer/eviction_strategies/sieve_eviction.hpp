#pragma once

#include <atomic>
#include <cstddef>
#include <shared_mutex>
#include <vector>

#include <storage/buffer/eviction_strategy.hpp>
#include <storage/buffer/helper.hpp>

namespace hyrise {

struct PageID;
class Frame;
enum class PageSizeType;

class SieveEviction : public EvictionStrategy {
 public:
  explicit SieveEviction(BufferPool& buffer_pool);

  bool perform_evictions(PageSizeType required_size) override;
  void add_eviction_candidate(const PageID& page_id, Frame* frame) override;
  void on_access(const PageID& page_id, Frame* frame) override;  // NEW
  void purge_eviction_candidates() override;
  std::size_t memory_consumption() const override;

 private:
  // Removes one item at index `idx` (must be valid under lock) and moves hand one step "toward tail".
  void _erase_at_and_advance_locked(const std::size_t idx);

  // Advances the hand one step "toward tail" (toward 0, wrapping) under lock.
  void _advance_hand_locked();

  // Purge helper: remove the entry at `this_hand` if it is still the current hand position.
  void purge_item(std::shared_lock<std::shared_mutex>& shared_lock, std::size_t this_hand);

 private:
  std::vector<EvictionItem> _eviction_vector;

  // Always maintained as an index in [0, _eviction_vector.size()) whenever vector non-empty.
  std::atomic<std::size_t> _hand;

  mutable std::shared_mutex _mutex;
};

}  // namespace hyrise
