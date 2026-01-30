#pragma once

#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hyrise {

class BufferPool;
class EvictionStrategy;

/**
 * Registry / factory for eviction strategies.
 *
 * Strategies self-register via EvictionStrategyRegistrar.
 * BufferPool creates an instance based on a name from config/JSON.
 */
class EvictionStrategyRegistry {
 public:
  using Factory = std::function<std::unique_ptr<EvictionStrategy>(BufferPool&)>;

  static EvictionStrategyRegistry& instance();

  // Register a strategy under a canonical name and optional aliases.
  // Returns false if any name collides with an existing registration.
  bool register_strategy(const std::string& canonical_name, Factory factory, std::vector<std::string> aliases = {});

  // Create by name (normalized, see normalize()).
  std::unique_ptr<EvictionStrategy> create(const std::string& name, BufferPool& pool) const;

  // For error messages / debugging.
  std::vector<std::string> available_names() const;

 private:
  EvictionStrategyRegistry() = default;

  static std::string normalize(std::string s);

  mutable std::shared_mutex _mutex;

  // normalized name -> factory
  std::unordered_map<std::string, Factory> _factories;

  // normalized name -> canonical name
  std::unordered_map<std::string, std::string> _canonical;
};

struct EvictionStrategyRegistrar {
  EvictionStrategyRegistrar(const std::string& canonical_name, EvictionStrategyRegistry::Factory factory,
                            std::vector<std::string> aliases = {});
};

}  // namespace hyrise
