#pragma once

#include <atomic>
#include <cstddef>

#include <storage/buffer/eviction_strategy.hpp>
#include <storage/buffer/helper.hpp>

namespace hyrise {

class DelayedFifoReinsertionEviction : public EvictionStrategy {
 public:
  explicit DelayedFifoReinsertionEviction(BufferPool& buffer_pool);

  bool perform_evictions(PageSizeType required_size) override;
  void add_eviction_candidate(const PageID& page_id, Frame* frame) override;
  void on_access(const PageID& page_id, Frame* frame) override;
  void purge_eviction_candidates() override;
  std::size_t memory_consumption() const override;

 private:
  // Logical time (increments on each access).
  std::atomic<uint32_t> _time{0};

  // FIFO queue of candidates (same structure as second chance).
  EvictionQueue _eviction_queue;

  // D-FR parameters
  static constexpr double DELAY_RATIO = 0.05;   // 5% (paper default used in Fig. 11)
  static constexpr uint8_t FREQ_BITS = 1;       // paper default (used in Fig. 11)

  uint32_t _delay_time_cached() const;
  uint8_t _max_freq() const;
};

}  // namespace hyrise
