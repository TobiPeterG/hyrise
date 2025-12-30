#include "hyrise.hpp"
#include "storage/buffer/jemalloc_resource.hpp"

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

Hyrise& Hyrise::operator=(Hyrise&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  // Stop work first so nothing touches state while we dismantle it.
  if (_scheduler) {
    _scheduler->finish();
  }

  // Destroy subsystems that might access tables during destruction.
  // (Assigning from moved-from objects can keep them in valid states, but we want to
  // deterministically destroy current state in the right order.)
  plugin_manager = PluginManager{};

  // Destroy tables before the BufferManager mapping can change.
  storage_manager = StorageManager{};

  // Clear thread-local allocation caches that might still point into the old BufferManager mapping.
  // TODO: Is this really required?
  LinearBufferResource::get().reset();

#ifdef HYRISE_WITH_JEMALLOC
  // Now that tables/segments are gone, it is safe to reset jemalloc's arena state.
  // Do this before the BufferManager mapping is replaced, so cached extents cannot
  // keep referencing the old mapping.
  JemallocMemoryResource::get().reset();
#endif

  //  Now it should be safe to replace the BufferManager.
  buffer_manager = BufferManager{};

  // Replace the remaining state
  buffer_manager = std::move(other.buffer_manager);
  storage_manager = std::move(other.storage_manager);
  plugin_manager = std::move(other.plugin_manager);

  transaction_manager = std::move(other.transaction_manager);
  meta_table_manager = std::move(other.meta_table_manager);
  settings_manager = std::move(other.settings_manager);
  log_manager = std::move(other.log_manager);
  topology = std::move(other.topology);

  default_pqp_cache = std::move(other.default_pqp_cache);
  default_lqp_cache = std::move(other.default_lqp_cache);
  benchmark_runner = std::move(other.benchmark_runner);

  _scheduler = std::move(other._scheduler);
  return *this;
}

void Hyrise::reset() {
#ifndef NDEBUG
  std::cerr << "[RESET] before: BM base=" << static_cast<void*>(Hyrise::get().buffer_manager.debug_mapped_region_base()) << "\n";
#endif
  Hyrise::get().scheduler()->finish();

  get() = Hyrise{};
#ifndef NDEBUG
  std::cerr << "[RESET] after : BM base=" << static_cast<void*>(Hyrise::get().buffer_manager.debug_mapped_region_base()) << "\n";
#endif
}

const std::shared_ptr<AbstractScheduler>& Hyrise::scheduler() const {
  return _scheduler;
}

bool Hyrise::is_multi_threaded() const {
  return std::dynamic_pointer_cast<ImmediateExecutionScheduler>(_scheduler) == nullptr;
}

void Hyrise::set_scheduler(const std::shared_ptr<AbstractScheduler>& new_scheduler) {
  _scheduler->finish();
  _scheduler = new_scheduler;
  _scheduler->begin();
}

}  // namespace hyrise
