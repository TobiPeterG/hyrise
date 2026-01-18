#pragma once

namespace hyrise {
struct BufferPool;

class EvictionStrategy {
public:
  explicit EvictionStrategy(BufferPool& buffer_pool);
  virtual ~EvictionStrategy() = default;
  virtual bool perform_evictions(PageSizeType required_size) = 0;
  virtual void add_eviction_candidate(const PageID& page_id, Frame* frame) = 0;
  virtual void purge_eviction_candidates() = 0;
  virtual std::size_t memory_consumption() const = 0;

protected:
  BufferPool& _buffer_pool;
};

}  // namespace hyrise
