#include "LoadedSaves.h"

#include <fstream>

#include <nlohmann/json.hpp>

#include "JsonValue.h"
#include "SkyrimDiagLongPath.h"

namespace skydiag::dump_tool {

namespace {

constexpr std::uint64_t kOtherSaveWindowSeconds = std::uint64_t{ 24 } * 60u * 60u;

}  // namespace

std::vector<LoadedSave> ReadLoadedSaves(const std::filesystem::path& path)
{
  std::vector<LoadedSave> loads;
  try {
    std::ifstream f(skydiag::PathForIo(path));
    if (!f.is_open()) {
      return loads;
    }
    const auto j = nlohmann::json::parse(f, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || JsonValueOr(j, "version", 0u) != 1u || !j.contains("loads") || !j["loads"].is_array()) {
      return loads;
    }
    for (const auto& row : j["loads"]) {
      LoadedSave load{};
      load.key = JsonValueOr(row, "save_key", std::string{});
      load.name_utf8 = JsonValueOr(row, "save_name", std::string{});
      load.loaded_unix = JsonValueOr(row, "loaded_unix", std::uint64_t{ 0 });
      load.attempt_key = JsonValueOr(row, "attempt_key", std::string{});
      if (!load.key.empty() && load.loaded_unix != 0u) {
        loads.push_back(std::move(load));
      }
    }
  } catch (...) {
    loads.clear();  // a damaged file tells nothing
  }
  return loads;
}

std::optional<LoadedSave> NewestOtherLoadedSave(
  const std::vector<LoadedSave>& loads, std::string_view saveKey, std::uint64_t captureUnix)
{
  if (saveKey.empty() || captureUnix == 0u) {
    return std::nullopt;
  }
  const std::uint64_t windowStart = captureUnix > kOtherSaveWindowSeconds ? captureUnix - kOtherSaveWindowSeconds : 0u;
  const LoadedSave* newest = nullptr;
  for (const auto& load : loads) {
    if (load.key == saveKey || load.loaded_unix > captureUnix || load.loaded_unix < windowStart) {
      continue;
    }
    if (!newest || load.loaded_unix >= newest->loaded_unix) {
      newest = &load;
    }
  }
  return newest ? std::optional<LoadedSave>(*newest) : std::nullopt;
}

}  // namespace skydiag::dump_tool
