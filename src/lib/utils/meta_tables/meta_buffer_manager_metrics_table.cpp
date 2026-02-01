#include "meta_buffer_manager_metrics_table.hpp"
#include "hyrise.hpp"

namespace hyrise {

// TODO: Extract more metrics
MetaBufferManagerMetricsTable::MetaBufferManagerMetricsTable()
    : AbstractMetaTable(TableColumnDefinitions{{"pool_size_dram", DataType::Long, false},
                                               {"pool_size_numa", DataType::Long, false},
                                               {"free_bytes_dram_node", DataType::Long, false},
                                               {"free_bytes_numa_node", DataType::Long, false},
                                               {"total_bytes_dram_node", DataType::Long, false},
                                               {"total_bytes_numa_node", DataType::Long, false},
                                               {"reserved_bytes_dram_buffer_pool", DataType::Long, false},
                                               {"reserved_bytes_numa_buffer_pool", DataType::Long, false},
                                               {"total_allocated_bytes", DataType::Long, false},
                                               {"total_unused_bytes", DataType::Long, false},
                                               {"internal_fragmentation_rate", DataType::Double, false},
                                               {"num_allocs", DataType::Long, false},
                                               {"num_deallocs", DataType::Long, false},
                                               {"total_bytes_copied_from_ssd_to_dram", DataType::Long, false},
                                               {"total_bytes_copied_from_ssd_to_numa", DataType::Long, false},
                                               {"total_bytes_copied_from_numa_to_dram", DataType::Long, false},
                                               {"total_bytes_copied_from_dram_to_numa", DataType::Long, false},
                                               {"total_bytes_copied_from_dram_to_ssd", DataType::Long, false},
                                               {"total_bytes_copied_from_numa_to_ssd", DataType::Long, false},
                                               {"total_bytes_copied_to_ssd", DataType::Long, false},
                                               {"total_bytes_copied_from_ssd", DataType::Long, false},
                                               {"total_hits", DataType::Long, false},
                                               {"total_misses", DataType::Long, false},
                                               {"total_pins", DataType::Long, false},
                                               {"current_pins", DataType::Long, false},
                                               {"num_dram_eviction_queue_items_purged", DataType::Long, false},
                                               {"num_dram_eviction_queue_adds", DataType::Long, false},
                                               {"num_numa_eviction_queue_items_purged", DataType::Long, false},
                                               {"num_numa_eviction_queue_adds", DataType::Long, false},
                                               {"num_dram_evictions", DataType::Long, false},
                                               {"num_numa_evictions", DataType::Long, false},
                                               {"num_madvice_free_calls", DataType::Long, false},
                                               {"num_numa_tonode_memory_calls", DataType::Long, false},

                                               // eviction diagnostics
                                               {"num_dram_eviction_candidate_inspections", DataType::Long, false},
                                               {"num_numa_eviction_candidate_inspections", DataType::Long, false},
                                               {"num_dram_eviction_requeues_pinned", DataType::Long, false},
                                               {"num_numa_eviction_requeues_pinned", DataType::Long, false},
                                               {"num_dram_eviction_requeues_referenced", DataType::Long, false},
                                               {"num_numa_eviction_requeues_referenced", DataType::Long, false},
                                               {"num_dram_eviction_requeues_lock_failed", DataType::Long, false},
                                               {"num_numa_eviction_requeues_lock_failed", DataType::Long, false},
                                               {"num_dram_eviction_failures", DataType::Long, false},
                                               {"num_numa_eviction_failures", DataType::Long, false},
                                               {"num_dram_oversubscription_episodes", DataType::Long, false},
                                               {"num_numa_oversubscription_episodes", DataType::Long, false},
                                               {"num_eviction_candidate_inspections_total", DataType::Long, false},
                                               {"num_evictions_total", DataType::Long, false},

                                               {"total_bytes_state", DataType::Long, false}

      }) {}

const std::string& MetaBufferManagerMetricsTable::name() const {
  static const auto name = std::string{"buffer_manager_metrics"};
  return name;
}

std::shared_ptr<Table> MetaBufferManagerMetricsTable::_on_generate() const {
  const auto metrics = Hyrise::get().buffer_manager.metrics();
  auto output_table = std::make_shared<Table>(_column_definitions, TableType::Data, std::nullopt, UseMvcc::Yes);

  const auto dram = metrics->dram_buffer_pool_metrics;
  const auto numa = metrics->numa_buffer_pool_metrics;

  const auto dram_inspections = dram->num_eviction_candidate_inspections.load(std::memory_order_relaxed);
  const auto numa_inspections = numa->num_eviction_candidate_inspections.load(std::memory_order_relaxed);
  const auto inspections_total = dram_inspections + numa_inspections;

  const auto dram_evictions = dram->num_evictions.load(std::memory_order_relaxed);
  const auto numa_evictions = numa->num_evictions.load(std::memory_order_relaxed);
  const auto evictions_total = dram_evictions + numa_evictions;

  output_table->append(
      {static_cast<int64_t>(Hyrise::get().buffer_manager.config().dram_buffer_pool_size),
       static_cast<int64_t>(Hyrise::get().buffer_manager.config().numa_buffer_pool_size),
       static_cast<int64_t>(Hyrise::get().buffer_manager.free_bytes_dram_node()),
       static_cast<int64_t>(Hyrise::get().buffer_manager.free_bytes_numa_node()),
       static_cast<int64_t>(Hyrise::get().buffer_manager.total_bytes_dram_node()),
       static_cast<int64_t>(Hyrise::get().buffer_manager.total_bytes_numa_node()),
       static_cast<int64_t>(Hyrise::get().buffer_manager.reserved_bytes_dram_buffer_pool()),
       static_cast<int64_t>(Hyrise::get().buffer_manager.reserved_bytes_numa_buffer_pool()),
       static_cast<int64_t>(metrics->total_allocated_bytes.load(std::memory_order_relaxed)),
       static_cast<int64_t>(metrics->total_unused_bytes.load(std::memory_order_relaxed)),
       metrics->internal_fragmentation_rate(),
       static_cast<int64_t>(metrics->num_allocs.load(std::memory_order_relaxed)),
       static_cast<int64_t>(metrics->num_deallocs.load(std::memory_order_relaxed)),
       static_cast<int64_t>(metrics->total_bytes_copied_from_ssd_to_dram.load(std::memory_order_relaxed)),
       static_cast<int64_t>(metrics->total_bytes_copied_from_ssd_to_numa.load(std::memory_order_relaxed)),
       static_cast<int64_t>(metrics->total_bytes_copied_from_numa_to_dram.load(std::memory_order_relaxed)),
       static_cast<int64_t>(metrics->total_bytes_copied_from_dram_to_numa.load(std::memory_order_relaxed)),
       static_cast<int64_t>(dram->total_bytes_copied_to_ssd.load(std::memory_order_relaxed)),
       static_cast<int64_t>(numa->total_bytes_copied_to_ssd.load(std::memory_order_relaxed)),
       static_cast<int64_t>(metrics->total_bytes_copied_to_ssd.load(std::memory_order_relaxed)),
       static_cast<int64_t>(metrics->total_bytes_copied_from_ssd.load(std::memory_order_relaxed)),
       static_cast<int64_t>(metrics->total_hits.load(std::memory_order_relaxed)),
       static_cast<int64_t>(metrics->total_misses.load(std::memory_order_relaxed)),
       static_cast<int64_t>(metrics->total_pins.load(std::memory_order_relaxed)),
       static_cast<int64_t>(metrics->current_pins.load(std::memory_order_relaxed)),

       static_cast<int64_t>(dram->num_eviction_queue_items_purged.load(std::memory_order_relaxed)),
       static_cast<int64_t>(dram->num_eviction_queue_adds.load(std::memory_order_relaxed)),

       static_cast<int64_t>(numa->num_eviction_queue_items_purged.load(std::memory_order_relaxed)),
       static_cast<int64_t>(numa->num_eviction_queue_adds.load(std::memory_order_relaxed)),

       static_cast<int64_t>(dram_evictions), static_cast<int64_t>(numa_evictions),

       static_cast<int64_t>(metrics->num_madvice_free_calls.load(std::memory_order_relaxed)),
       static_cast<int64_t>(metrics->num_numa_tonode_memory_calls.load(std::memory_order_relaxed)),

       // eviction diagnostics
       static_cast<int64_t>(dram_inspections), static_cast<int64_t>(numa_inspections),
       static_cast<int64_t>(dram->num_eviction_requeues_pinned.load(std::memory_order_relaxed)),
       static_cast<int64_t>(numa->num_eviction_requeues_pinned.load(std::memory_order_relaxed)),
       static_cast<int64_t>(dram->num_eviction_requeues_referenced.load(std::memory_order_relaxed)),
       static_cast<int64_t>(numa->num_eviction_requeues_referenced.load(std::memory_order_relaxed)),
       static_cast<int64_t>(dram->num_eviction_requeues_lock_failed.load(std::memory_order_relaxed)),
       static_cast<int64_t>(numa->num_eviction_requeues_lock_failed.load(std::memory_order_relaxed)),
       static_cast<int64_t>(dram->num_eviction_failures.load(std::memory_order_relaxed)),
       static_cast<int64_t>(numa->num_eviction_failures.load(std::memory_order_relaxed)),
       static_cast<int64_t>(dram->num_oversubscription_episodes.load(std::memory_order_relaxed)),
       static_cast<int64_t>(numa->num_oversubscription_episodes.load(std::memory_order_relaxed)),
       static_cast<int64_t>(inspections_total), static_cast<int64_t>(evictions_total),

       static_cast<int64_t>(Hyrise::get().buffer_manager.memory_consumption())});

  return output_table;
}
}  // namespace hyrise
