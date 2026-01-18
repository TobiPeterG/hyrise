#include "eviction_strategy.hpp"

namespace hyrise {

EvictionStrategy::EvictionStrategy(BufferPool& buffer_pool) : _buffer_pool(buffer_pool) {}

}  // namespace hyrise