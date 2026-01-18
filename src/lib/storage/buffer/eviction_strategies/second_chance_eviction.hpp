#pragma once

#include <storage/buffer/eviction_strategy.hpp>

namespace hyrise {

class SecondChanceEviction : public EvictionStrategy {
public:
  explicit SecondChanceEviction(BufferPool& buffer_pool);
  bool perform_evictions(PageSizeType required_size) override;
  void add_eviction_candidate(const PageID& page_id, Frame* frame) override;
  std::size_t memory_consumption() const override;
  void purge_eviction_candidates() override;

private:
  EvictionQueue _eviction_queue;
};

}  // namespace hyrise
