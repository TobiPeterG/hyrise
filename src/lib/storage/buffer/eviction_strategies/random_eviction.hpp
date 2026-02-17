#pragma once

#include <oneapi/tbb/concurrent_priority_queue.h>

#include <random>
#include <storage/buffer/eviction_strategy.hpp>

namespace hyrise {
struct EvictionItem;

class RandomEviction : public EvictionStrategy {
public:
  explicit RandomEviction(BufferPool& buffer_pool);
  void add_eviction_candidate(const PageID& page_id, Frame* frame) override;
  bool perform_evictions(PageSizeType required_size) override;
  void purge_eviction_candidates() override;
  // void on_access(const PageID& page_id, Frame* frame) override;
  std::size_t memory_consumption() const override;

private:
  using QueueEntry = std::pair<uint64_t, EvictionItem>;

  class PriorityFunction {
  public:
    bool operator()(const QueueEntry& left, const QueueEntry& right) const;
  };

  tbb::concurrent_priority_queue<QueueEntry, PriorityFunction> _queue;
};

}  // namespace hyrise
