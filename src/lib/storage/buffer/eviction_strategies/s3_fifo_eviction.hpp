#pragma once

#include <oneapi/tbb/concurrent_queue.h>
#include <oneapi/tbb/concurrent_hash_map.h>

#include <storage/buffer/eviction_strategy.hpp>

namespace hyrise {
struct EvictionItem;

class S3_FifoEviction : public EvictionStrategy {
public:
  explicit S3_FifoEviction(BufferPool& buffer_pool, float main_queue_ratio, uint8_t num_frequency_bits);
  void add_eviction_candidate(const PageID& page_id, Frame* frame) override;
  bool perform_evictions(PageSizeType required_size) override;
  void purge_eviction_candidates() override;
  void on_access(const PageID& page_id, Frame* frame) override;
  std::size_t memory_consumption() const override;

private:
  class PageIDComparator {
  public:
    explicit PageIDComparator(uint64_t capacity);
    PageIDComparator(const PageIDComparator& other);
    ~PageIDComparator();
    std::size_t hash(const PageID& page_id) const;
    bool equal(const PageID& page_id1, const PageID& page_id2) const;

  private:
    const uint64_t capacity;
  };

  std::atomic<uint64_t> _ghost_insertion_count;
  const float _small_queue_ratio;
  const float _main_queue_ratio;
  const uint64_t _full_capacity;
  const uint64_t _ghost_queue_capacity;
  const uint8_t _max_frequency;

  tbb::concurrent_queue<EvictionItem> _main_queue;
  tbb::concurrent_queue<EvictionItem> _small_queue;
  tbb::concurrent_hash_map<PageID, uint64_t, PageIDComparator> _ghost_queue;

  [[nodiscard]] bool evict_from_small_queue();
  [[nodiscard]] bool evict_from_main_queue();
};

}  // namespace hyrise
