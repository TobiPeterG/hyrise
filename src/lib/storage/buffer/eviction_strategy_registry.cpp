#include "storage/buffer/eviction_strategy_registry.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#include "utils/assert.hpp"

namespace hyrise {

EvictionStrategyRegistry& EvictionStrategyRegistry::instance() {
  static EvictionStrategyRegistry registry;
  return registry;
}

std::string EvictionStrategyRegistry::normalize(std::string s) {
  // Lowercase and make separators consistent:
  // "Second Chance", "second-chance", "second_chance" => "second_chance"
  std::string out;
  out.reserve(s.size());

  auto last_was_sep = false;
  for (const auto ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    if (std::isalnum(c)) {
      out.push_back(static_cast<char>(std::tolower(c)));
      last_was_sep = false;
    } else {
      if (!out.empty() && !last_was_sep) {
        out.push_back('_');
        last_was_sep = true;
      }
    }
  }

  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }

  return out;
}

bool EvictionStrategyRegistry::register_strategy(const std::string& canonical_name, Factory factory,
                                                std::vector<std::string> aliases) {
  const auto canonical_norm = normalize(canonical_name);

  std::unique_lock lock{_mutex};

  auto try_insert = [&](const std::string& name) -> bool {
    const auto norm = normalize(name);

    const auto it = _factories.find(norm);
    if (it != _factories.end()) {
      // Already registered. If it's the same canonical strategy, treat as idempotent.
      const auto canon_it = _canonical.find(norm);
      if (canon_it != _canonical.end() && canon_it->second == canonical_name) {
        return true;
      }
      return false;
    }

    _factories.emplace(norm, factory);
    _canonical.emplace(norm, canonical_name);
    return true;
  };

  if (!try_insert(canonical_name)) {
    return false;
  }

  // If canonical name contains spaces/dashes etc., also register its normalized form explicitly.
  if (!try_insert(canonical_norm)) {
    return false;
  }

  for (const auto& alias : aliases) {
    if (!try_insert(alias)) {
      return false;
    }
  }

  return true;
}

std::unique_ptr<EvictionStrategy> EvictionStrategyRegistry::create(const std::string& name, BufferPool& pool) const {
  const auto norm = normalize(name);

  std::shared_lock lock{_mutex};

  const auto it = _factories.find(norm);
  if (it == _factories.end()) {
    const auto names = available_names();
    std::string msg = "Unknown eviction strategy '" + name + "'. Available: ";
    for (auto i = size_t{0}; i < names.size(); ++i) {
      msg += names[i];
      if (i + 1 < names.size()) {
        msg += ", ";
      }
    }
    Fail(msg);
  }

  return (it->second)(pool);
}

std::vector<std::string> EvictionStrategyRegistry::available_names() const {
  std::shared_lock lock{_mutex};

  std::vector<std::string> out;
  out.reserve(_canonical.size());
  for (const auto& [_, canon] : _canonical) {
    out.push_back(canon);
  }

  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

EvictionStrategyRegistrar::EvictionStrategyRegistrar(const std::string& canonical_name, EvictionStrategyRegistry::Factory factory,
                                                    std::vector<std::string> aliases) {
  const auto ok =
      EvictionStrategyRegistry::instance().register_strategy(canonical_name, std::move(factory), std::move(aliases));
  Assert(ok, "Duplicate eviction strategy registration for '" + canonical_name + "'");
}

}  // namespace hyrise
