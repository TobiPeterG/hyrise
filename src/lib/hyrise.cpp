#include "hyrise.hpp"
#include "storage/buffer/jemalloc_resource.hpp"

#ifndef NDEBUG
#include <iostream>
#endif

namespace hyrise {

Hyrise::Hyrise() {
  // The default_memory_resource must be initialized before Hyrise's members so that
  // it is destructed after them and remains accessible during their deconstruction.
  // For example, when the StorageManager is destructed, it causes its stored tables
  // to be deconstructed, too. As these might call deallocate on the
  // default_memory_resource, it is important that the resource has not been
  // destructed before. As objects are destructed in the reverse order of their
  // construction, explicitly initializing the resource first means that it is
  // destructed last.
  boost::container::pmr::get_default_resource();

  buffer_manager = BufferManager{};
  storage_manager = {};
  plugin_manager = PluginManager{};
  transaction_manager = TransactionManager{};
  meta_table_manager = MetaTableManager{};
  settings_manager = SettingsManager{};
  log_manager = LogManager{};
  topology = Topology{};

  _scheduler = std::make_shared<ImmediateExecutionScheduler>();
}

// TODO: Is this really required?
void Hyrise::reset() {
  auto& h = Hyrise::get();

#ifndef NDEBUG
  std::cerr << "[RESET] before: BM base=" << static_cast<void*>(h.buffer_manager.debug_mapped_region_base()) << "\n";
#endif

  if (h._scheduler) {
    h._scheduler->finish();
  }
  h._scheduler.reset();

  h.default_pqp_cache.reset();
  h.default_lqp_cache.reset();
  h.benchmark_runner.reset();

  h.plugin_manager = PluginManager{};
  h.storage_manager = StorageManager{};

  h.transaction_manager = TransactionManager{};
  h.meta_table_manager = MetaTableManager{};
  h.settings_manager = SettingsManager{};
  h.log_manager = LogManager{};
  h.topology = Topology{};

  LinearBufferResource::get().reset();

#ifdef HYRISE_WITH_JEMALLOC
  JemallocMemoryResource::get().reset();
#endif

  h.buffer_manager = BufferManager{};

  h._scheduler = std::make_shared<ImmediateExecutionScheduler>();
  h._scheduler->begin();

#ifndef NDEBUG
  std::cerr << "[RESET] after : BM base=" << static_cast<void*>(h.buffer_manager.debug_mapped_region_base()) << "\n";
#endif
}

const std::shared_ptr<AbstractScheduler>& Hyrise::scheduler() const {
  return _scheduler;
}

bool Hyrise::is_multi_threaded() const {
  return std::dynamic_pointer_cast<ImmediateExecutionScheduler>(_scheduler) == nullptr;
}

void Hyrise::set_scheduler(const std::shared_ptr<AbstractScheduler>& new_scheduler) {
  if (_scheduler) {
    _scheduler->finish();
  }
  _scheduler = new_scheduler;
  if (_scheduler) {
    _scheduler->begin();
  }
}

}  // namespace hyrise
