#include "AddressResolver.h"

#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>

namespace skydiag::dump_tool {

bool AddressResolver::LoadFromJson(const std::filesystem::path& jsonPath, const std::string& gameVersion)
{
  try {
    std::ifstream f(jsonPath);
    if (!f.is_open()) {
      return false;
    }
    const auto j = nlohmann::json::parse(f, nullptr, true);
    if (!j.is_object() || !j.contains("game_versions") || !j["game_versions"].is_object()) {
      return false;
    }
    const auto& versions = j["game_versions"];
    if (!versions.contains(gameVersion) || !versions[gameVersion].is_object()) {
      return false;
    }

    const auto& raw = versions[gameVersion];
    const auto* entries = &raw;
    if (raw.contains("functions") && raw["functions"].is_object()) {
      entries = &raw["functions"];
    }

    std::unordered_map<std::uint64_t, std::string> loaded;
    for (const auto& [offsetStr, name] : entries->items()) {
      if (!name.is_string()) {
        continue;
      }
      const std::uint64_t offset = std::stoull(offsetStr, nullptr, 16);
      loaded[offset] = name.get<std::string>();
    }
    if (loaded.empty()) {
      return false;
    }
    m_functions = std::move(loaded);
    return true;
  } catch (...) {
    return false;
  }
}

std::optional<std::string> AddressResolver::Resolve(std::uint64_t offset) const
{
  const auto it = m_functions.find(offset);
  if (it != m_functions.end()) {
    return it->second;
  }

  constexpr std::uint64_t kTolerance = 0x100;
  std::optional<std::pair<std::uint64_t, std::string>> nearest;
  for (const auto& [fnOffset, fnName] : m_functions) {
    if (offset < fnOffset) {
      continue;
    }
    const std::uint64_t diff = offset - fnOffset;
    if (diff >= kTolerance) {
      continue;
    }
    if (!nearest || diff < nearest->first) {
      nearest = std::make_pair(diff, fnName);
    }
  }
  if (nearest) {
    return nearest->second;
  }
  return std::nullopt;
}

}  // namespace skydiag::dump_tool
