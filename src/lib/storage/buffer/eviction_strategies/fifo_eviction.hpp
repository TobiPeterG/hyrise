#pragma once

#include <cstddef>

#include <storage/buffer/eviction_strategy.hpp>
#include <storage/buffer/helper.hpp>

namespace hyrise {

class FifoEviction : public EvictionStrategy {
 public:
  explicit FifoEviction(BufferPool& buffer_pool);

  bool perform_evictions(PageSizeType required_size) override;
  void add_eviction_candidate(const PageID& page_id, Frame* frame) override;
  void on_access(const PageID& page_id, Frame* frame) override;  // intentionally no-op for FIFO
  void purge_eviction_candidates() override;
  std::size_t memory_consumption() const override;

 private:
  EvictionQueue _eviction_queue;
};

}  // namespace hyrise
