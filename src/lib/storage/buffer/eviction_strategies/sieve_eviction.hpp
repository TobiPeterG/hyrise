#pragma once

#include <cstddef>

#include <oneapi/tbb/concurrent_vector.h>

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
  std::size_t memory_consumption() const override;
  void purge_eviction_candidates() override;

private:
  void purge_item(std::shared_lock<std::shared_mutex>& shared_lock, std::size_t this_hand);

  std::vector<EvictionItem> _eviction_vector;
  std::atomic<std::size_t> _hand;

  std::shared_mutex _mutex;
};

}  // namespace hyrise
