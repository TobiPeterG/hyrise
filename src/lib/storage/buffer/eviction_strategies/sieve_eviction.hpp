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
  struct Slot {
    EvictionItem item;
    bool valid{false};
  };

  // Advances the hand one step "toward tail" (toward 0, wrapping) under lock.
  void _advance_hand_locked();
  
  bool _advance_hand_to_prev_valid_from_locked(std::size_t start_idx);

  // Advances the hand to the previous valid entry. Returns false if no valid entry exists.
  bool _advance_hand_to_prev_valid_locked();

  // Marks the slot at idx invalid (tombstone) and advances hand to previous valid.
  void _invalidate_at_and_advance_locked(const std::size_t idx);

  // Purge helper: invalidate the entry at `this_hand` if it is still the current hand position.
  void purge_item(std::shared_lock<std::shared_mutex>& shared_lock, std::size_t this_hand);

  // Compacts away tombstones. Must be called with unique lock held.
  void _compact_if_needed_locked();

 private:
  std::vector<Slot> _eviction_vector;

  // Always maintained as an index in [0, _eviction_vector.size()) whenever there is at least one valid entry.
  std::atomic<std::size_t> _hand;

  // Number of valid entries (not tombstoned). Protected by _mutex.
  std::size_t _live_entries{0};

  // Number of tombstones. Protected by _mutex.
  std::size_t _tombstones{0};

  mutable std::shared_mutex _mutex;
};

}  // namespace hyrise
