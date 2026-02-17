#include "s3_fifo_eviction.hpp"

#include <storage/buffer/helper.hpp>

#include "storage/buffer/eviction_strategy_registry.hpp"

namespace hyrise {

namespace {
EvictionStrategyRegistrar s_s3_fifo_registrar{
  "s3_fifo",
  [](BufferPool& buffer_pool) { return std::make_unique<S3_FifoEviction>(buffer_pool, 0.9, 1); },
  {"S3_FIFO"}};

EvictionStrategyRegistrar s_s3_fifo_registrar_multi_bit{
  "s3_fifo_multi_bit",
  [](BufferPool& buffer_pool) { return std::make_unique<S3_FifoEviction>(buffer_pool, 0.9, 2); },
  {"S3_FIFO_MULTI_BIT", "S3_FIFO_MULTIBIT"}};
}

S3_FifoEviction::S3_FifoEviction(BufferPool& buffer_pool, const float main_queue_ratio, const uint8_t num_frequency_bits)
: EvictionStrategy(buffer_pool), _small_queue_ratio(1 - main_queue_ratio), _main_queue_ratio(main_queue_ratio),
_full_capacity(_buffer_pool.max_bytes / bytes_for_size_type(PageSizeType::KiB4)),
_ghost_queue_capacity(std::max(static_cast<uint64_t>(static_cast<float>(_full_capacity) * _main_queue_ratio), uint64_t{1})),
_max_frequency((0b1U << num_frequency_bits) - 1),
_ghost_queue(PageIDComparator{_ghost_queue_capacity}) {}

void S3_FifoEviction::add_eviction_candidate(const PageID& page_id, Frame* const frame) {
  decltype(_ghost_queue)::const_accessor accessor;
  const auto found = _ghost_queue.find(accessor, page_id);

  DebugAssert(frame->node_id() == _buffer_pool.node_id, "Memory node mismatch");
  increment_counter(_buffer_pool.metrics->num_eviction_queue_adds);

  const auto current_state_and_version = frame->state_and_version();
  // Entry is in ghost queue and recent enough
  if (found && _ghost_insertion_count - accessor->second < _ghost_queue_capacity) {
    _main_queue.push({.page_id = page_id, .timestamp = Frame::version(current_state_and_version)});
  } else { // Entry is not in ghost queue or stale
    _small_queue.push({.page_id = page_id, .timestamp = Frame::version(current_state_and_version)});
  }
}

bool S3_FifoEviction::perform_evictions(const PageSizeType required_size) {
  // TODO: Free at least 64 * PageSite bytes to reduce TLB shootdowns
  const auto bytes_required = bytes_for_size_type(required_size);

  _buffer_pool.reserve_bytes(bytes_required);
  const auto combined_size = _small_queue.unsafe_size() + _main_queue.unsafe_size();
  const auto combined_size_d = static_cast<double>(combined_size);
  const auto small_queue_expected_size = static_cast<std::size_t>(combined_size_d * _small_queue_ratio);

  while (_buffer_pool.used_bytes.load(std::memory_order_relaxed) > _buffer_pool.max_bytes) {
    bool successful = false;
    if (_small_queue.unsafe_size() >= small_queue_expected_size || _main_queue.unsafe_size() == 0) {
      successful |= evict_from_small_queue();
    } else {
      successful |= evict_from_main_queue();
    }

    if (!successful) {
      increment_counter(_buffer_pool.metrics->num_eviction_failures);
      _buffer_pool.free_bytes(bytes_required);
      return false;
    }
  }

  return true;
}

void S3_FifoEviction::on_access(const PageID& /*unused*/, Frame* frame) {
  frame->inc_reference_level_saturating(_max_frequency);
}

bool S3_FifoEviction::evict_from_small_queue() {
  while (true) {
    EvictionItem eviction_item;
    if (!_small_queue.try_pop(eviction_item)) {
      return false;
    }

    increment_counter(_buffer_pool.metrics->num_eviction_candidate_inspections);

    const auto region = _buffer_pool.volatile_regions[static_cast<uint64_t>(eviction_item.page_id.size_type())];
    auto* frame = region->get_frame(eviction_item.page_id);
    const auto current_state_and_version = frame->state_and_version();

    if (frame->node_id() != _buffer_pool.node_id) {
      increment_counter(_buffer_pool.metrics->num_eviction_queue_items_purged);
      continue;
    }

    // Stale entry (frame generation changed)
    if (Frame::version(current_state_and_version) != eviction_item.timestamp) {
      increment_counter(_buffer_pool.metrics->num_eviction_queue_items_purged);
      continue;
    }

    if (eviction_item.can_evict(current_state_and_version)) {
      if (!frame->try_lock_exclusive(current_state_and_version)) {
        increment_counter(_buffer_pool.metrics->num_eviction_requeues_lock_failed);
        _small_queue.push(eviction_item);
        continue;
      }

      Assert(frame->node_id() == _buffer_pool.node_id,
           "Memory node mismatch: " + std::to_string(frame->node_id()) + " != " + std::to_string(_buffer_pool.node_id));

      decltype(_ghost_queue)::accessor accessor;
      _ghost_queue.erase(eviction_item.page_id);
      _ghost_queue.emplace(accessor, eviction_item.page_id, _ghost_insertion_count.fetch_add(1, std::memory_order::relaxed));

      _buffer_pool.evict(eviction_item, frame);
      increment_counter(_buffer_pool.metrics->num_evictions);

      _buffer_pool.free_bytes(bytes_for_size_type(eviction_item.page_id.size_type()));

      return true;
    } else {
      _main_queue.push({.page_id = eviction_item.page_id, .timestamp = Frame::version(current_state_and_version)});

      return true;
    }
  }
}

bool S3_FifoEviction::evict_from_main_queue() {
  while (true) {
    EvictionItem eviction_item;
    if (!_main_queue.try_pop(eviction_item)) {
      return false;
    }

    increment_counter(_buffer_pool.metrics->num_eviction_candidate_inspections);

    const auto region = _buffer_pool.volatile_regions[static_cast<uint64_t>(eviction_item.page_id.size_type())];
    auto* frame = region->get_frame(eviction_item.page_id);
    const auto current_state_and_version = frame->state_and_version();

    if (frame->node_id() != _buffer_pool.node_id) {
      increment_counter(_buffer_pool.metrics->num_eviction_queue_items_purged);
      continue;
    }

    // Stale entry (frame generation changed)
    if (Frame::version(current_state_and_version) != eviction_item.timestamp) {
      increment_counter(_buffer_pool.metrics->num_eviction_queue_items_purged);
      continue;
    }

    if (eviction_item.can_evict(current_state_and_version)) {
      if (!frame->try_lock_exclusive(current_state_and_version)) {
        increment_counter(_buffer_pool.metrics->num_eviction_requeues_lock_failed);
        _main_queue.push(eviction_item);
        continue;
      }

      Assert(frame->node_id() == _buffer_pool.node_id,
           "Memory node mismatch: " + std::to_string(frame->node_id()) + " != " + std::to_string(_buffer_pool.node_id));

      _buffer_pool.evict(eviction_item, frame);
      increment_counter(_buffer_pool.metrics->num_evictions);

      _buffer_pool.free_bytes(bytes_for_size_type(eviction_item.page_id.size_type()));

      return true;
    } else {
      frame->dec_reference_level_if_positive();
      _main_queue.push({.page_id = eviction_item.page_id, .timestamp = Frame::version(current_state_and_version)});
    }
  }
}

// Not sure how this function translates to S3-FIFO, but perhaps purging from the both queues according to their
// relative size makes sense to me.
void S3_FifoEviction::purge_eviction_candidates() {
  auto item = EvictionItem{};
  const auto main_queue_purges = static_cast<std::size_t>(static_cast<float>(MAX_EVICTION_QUEUE_PURGES) * _main_queue_ratio);
  const auto small_queue_purges = static_cast<std::size_t>(static_cast<float>(MAX_EVICTION_QUEUE_PURGES) * _small_queue_ratio);

  for (auto i = std::size_t{0}; i < main_queue_purges; ++i) {
    if (!_main_queue.try_pop(item)) {
      break;
    }

    const auto region = _buffer_pool.volatile_regions[static_cast<uint64_t>(item.page_id.size_type())];
    auto* frame = region->get_frame(item.page_id);
    const auto current_state_and_version = frame->state_and_version();

    // Purge stale or migrated entries. Keep pinned/ref entries.
    if (frame->node_id() != _buffer_pool.node_id || Frame::version(current_state_and_version) != item.timestamp) {
      increment_counter(_buffer_pool.metrics->num_eviction_queue_items_purged);
      continue;
    }

    _main_queue.push(item);
  }

  for (auto i = std::size_t{0}; i < small_queue_purges; ++i) {
    if (!_small_queue.try_pop(item)) {
      break;
    }

    const auto region = _buffer_pool.volatile_regions[static_cast<uint64_t>(item.page_id.size_type())];
    auto* frame = region->get_frame(item.page_id);
    const auto current_state_and_version = frame->state_and_version();

    // Purge stale or migrated entries. Keep pinned/ref entries.
    if (frame->node_id() != _buffer_pool.node_id || Frame::version(current_state_and_version) != item.timestamp) {
      decltype(_ghost_queue)::accessor accessor;
      _ghost_queue.erase(item.page_id);
      _ghost_queue.emplace(accessor, item.page_id, _ghost_insertion_count.fetch_add(1, std::memory_order::relaxed));

      increment_counter(_buffer_pool.metrics->num_eviction_queue_items_purged);
      continue;
    }

    _small_queue.push(item);
  }
}

std::size_t S3_FifoEviction::memory_consumption() const {
  return sizeof(*this)
  + sizeof(_main_queue) + (sizeof(decltype(_main_queue)::value_type) * _main_queue.unsafe_size())
  + sizeof(_small_queue) + (sizeof(decltype(_small_queue)::value_type) * _small_queue.unsafe_size())
  + sizeof(_ghost_queue) + (sizeof(decltype(_ghost_queue)::value_type) * _ghost_queue.size());
}

S3_FifoEviction::PageIDComparator::PageIDComparator(const uint64_t capacity) : capacity(capacity) {}

S3_FifoEviction::PageIDComparator::PageIDComparator(const PageIDComparator& other) = default;

S3_FifoEviction::PageIDComparator::~PageIDComparator() = default;

std::size_t S3_FifoEviction::PageIDComparator::hash(const PageID& page_id) const {
  // The idea is to artificially limit the number of entries in the ghost queue / map this way. It is possible that that
  // is a horrible idea, as some hash functions are no longer equally distributed when taken modulo.
  return std::hash<PageID>{}(page_id) % capacity;
}

bool S3_FifoEviction::PageIDComparator::equal(const PageID& page_id1, const PageID& page_id2) const {
  return hash(page_id1) == hash(page_id2);
}

}  // namespace hyrise