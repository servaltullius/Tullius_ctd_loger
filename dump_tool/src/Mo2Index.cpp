#include "Mo2Index.h"

#include <algorithm>

namespace skydiag::dump_tool {
namespace {

std::wstring WideLower(std::wstring_view s)
{
  std::wstring out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
  return out;
}

}  // namespace

std::wstring InferMo2ModNameFromPath(std::wstring_view fullPath)
{
  // Best-effort: detect "...\\mods\\<ModName>\\..." (MO2)
  std::wstring s(fullPath);
  std::wstring lower = WideLower(s);

  const std::wstring needle = L"\\mods\\";
  const auto pos = lower.find(needle);
  if (pos == std::wstring::npos) {
    return {};
  }

  const auto start = pos + needle.size();
  if (start >= s.size()) {
    return {};
  }

  const auto end = s.find(L'\\', start);
  if (end == std::wstring::npos || end <= start) {
    return {};
  }

  return s.substr(start, end - start);
}

std::optional<std::filesystem::path> TryInferMo2BaseDirFromModulePaths(const std::vector<std::wstring>& modulePaths)
{
  for (const auto& path : modulePaths) {
    if (path.empty()) {
      continue;
    }
    const std::wstring lower = WideLower(path);
    const std::wstring needle = L"\\mods\\";
    const auto pos = lower.find(needle);
    if (pos == std::wstring::npos || pos == 0) {
      continue;
    }
    return std::filesystem::path(path.substr(0, pos));
  }
  return std::nullopt;
}

std::optional<Mo2Index> TryBuildMo2IndexFromModulePaths(const std::vector<std::wstring>& modulePaths)
{
  auto baseOpt = TryInferMo2BaseDirFromModulePaths(modulePaths);
  if (!baseOpt) {
    return std::nullopt;
  }

  Mo2Index idx{};
  idx.base = *baseOpt;
  idx.modsDir = idx.base / L"mods";
  idx.overwriteDir = idx.base / L"overwrite";

  std::error_code ec;
  if (!std::filesystem::is_directory(idx.modsDir, ec)) {
    return std::nullopt;
  }

  for (const auto& ent : std::filesystem::directory_iterator(idx.modsDir, ec)) {
    if (ec) {
      break;
    }
    if (!ent.is_directory(ec)) {
      continue;
    }
    const auto p = ent.path();
    idx.modDirs.push_back(p);
    idx.modNames.push_back(p.filename().wstring());
  }

  // Stable ordering for deterministic output.
  std::vector<std::size_t> order(idx.modDirs.size());
  for (std::size_t i = 0; i < order.size(); i++) {
    order[i] = i;
  }
  std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    return WideLower(idx.modNames[a]) < WideLower(idx.modNames[b]);
  });

  std::vector<std::filesystem::path> modDirsSorted;
  std::vector<std::wstring> modNamesSorted;
  modDirsSorted.reserve(idx.modDirs.size());
  modNamesSorted.reserve(idx.modNames.size());
  for (const auto i : order) {
    modDirsSorted.push_back(std::move(idx.modDirs[i]));
    modNamesSorted.push_back(std::move(idx.modNames[i]));
  }
  idx.modDirs = std::move(modDirsSorted);
  idx.modNames = std::move(modNamesSorted);

  return idx;
}

std::vector<std::wstring> FindMo2ProvidersForDataPath(const Mo2Index& idx, std::wstring_view relPath, std::size_t maxProviders)
{
  std::vector<std::wstring> out;
  if (relPath.empty() || maxProviders == 0) {
    return out;
  }

  std::wstring p(relPath);
  while (!p.empty() && (p.front() == L'\\' || p.front() == L'/')) {
    p.erase(p.begin());
  }

  std::filesystem::path rel(p);
  if (rel.empty() || rel.is_absolute()) {
    return out;
  }

  std::error_code ec;

  // overwrite wins in MO2 and is highly relevant for conflicts
  if (std::filesystem::is_directory(idx.overwriteDir, ec)) {
    if (std::filesystem::exists(idx.overwriteDir / rel, ec)) {
      out.push_back(L"overwrite");
      if (out.size() >= maxProviders) {
        return out;
      }
    }
  }

  for (std::size_t i = 0; i < idx.modDirs.size(); i++) {
    if (std::filesystem::exists(idx.modDirs[i] / rel, ec)) {
      out.push_back(idx.modNames[i]);
      if (out.size() >= maxProviders) {
        break;
      }
    }
  }

  return out;
}

}  // namespace skydiag::dump_tool
