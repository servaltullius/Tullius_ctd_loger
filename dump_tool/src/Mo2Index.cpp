#include "Mo2Index.h"
#include "MinidumpUtil.h"
#include "Utf.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace skydiag::dump_tool {
namespace {

using minidump::WideLower;

std::string_view Trim(std::string_view s)
{
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
    s.remove_suffix(1);
  }
  return s;
}

bool StartsWith(std::string_view s, std::string_view prefix)
{
  return (s.size() >= prefix.size()) && (s.substr(0, prefix.size()) == prefix);
}

std::optional<std::vector<std::string>> TryReadLines(const std::filesystem::path& path)
{
  std::ifstream f(path, std::ios::in | std::ios::binary);
  if (!f) {
    return std::nullopt;
  }

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(std::move(line));
  }

  // Strip UTF-8 BOM from first line if present.
  if (!lines.empty()) {
    auto& first = lines[0];
    if (first.size() >= 3 && static_cast<unsigned char>(first[0]) == 0xEF && static_cast<unsigned char>(first[1]) == 0xBB &&
        static_cast<unsigned char>(first[2]) == 0xBF) {
      first.erase(0, 3);
    }
  }
  return lines;
}

std::optional<std::wstring> TryReadSelectedProfileName(const std::filesystem::path& mo2Base)
{
  const auto iniPath = mo2Base / L"ModOrganizer.ini";

  std::error_code ec;
  if (!std::filesystem::exists(iniPath, ec)) {
    return std::nullopt;
  }

  const auto linesOpt = TryReadLines(iniPath);
  if (!linesOpt) {
    return std::nullopt;
  }

  constexpr std::string_view kKey = "selected_profile=";
  for (const auto& raw : *linesOpt) {
    const std::string_view line = Trim(raw);
    if (!StartsWith(line, kKey)) {
      continue;
    }

    std::string_view v = line.substr(kKey.size());
    v = Trim(v);
    // Common MO2 format: selected_profile=@ByteArray(Default)
    constexpr std::string_view kBA = "@ByteArray(";
    if (StartsWith(v, kBA) && v.size() >= (kBA.size() + 1) && v.back() == ')') {
      v = v.substr(kBA.size(), v.size() - kBA.size() - 1);
      v = Trim(v);
    }
    if (v.empty()) {
      return std::nullopt;
    }
    return Utf8ToWide(v);
  }
  return std::nullopt;
}

std::optional<std::filesystem::path> TryPickMo2ProfileDir(const std::filesystem::path& mo2Base, std::wstring* outProfileName)
{
  const auto profilesDir = mo2Base / L"profiles";
  std::error_code ec;
  if (!std::filesystem::is_directory(profilesDir, ec)) {
    return std::nullopt;
  }

  if (auto selected = TryReadSelectedProfileName(mo2Base)) {
    const auto p = profilesDir / *selected;
    if (std::filesystem::is_directory(p, ec)) {
      if (outProfileName) {
        *outProfileName = *selected;
      }
      return p;
    }
  }

  // Fallback: choose the profile whose modlist.txt was modified most recently.
  bool haveBest = false;
  std::filesystem::file_time_type bestTime{};
  std::filesystem::path bestPath;
  std::wstring bestName;

  for (const auto& ent : std::filesystem::directory_iterator(profilesDir, ec)) {
    if (ec) {
      break;
    }
    if (!ent.is_directory(ec)) {
      continue;
    }
    const auto p = ent.path();
    const auto modlist = p / L"modlist.txt";
    if (!std::filesystem::exists(modlist, ec)) {
      continue;
    }
    const auto t = std::filesystem::last_write_time(modlist, ec);
    if (ec) {
      continue;
    }
    if (!haveBest || t > bestTime) {
      haveBest = true;
      bestTime = t;
      bestPath = p;
      bestName = p.filename().wstring();
    }
  }

  if (!haveBest) {
    return std::nullopt;
  }
  if (outProfileName) {
    *outProfileName = bestName;
  }
  return bestPath;
}

std::vector<std::wstring> ReadMo2EnabledModsWinnerFirst(const std::filesystem::path& modlistPath)
{
  std::vector<std::wstring> out;
  const auto linesOpt = TryReadLines(modlistPath);
  if (!linesOpt) {
    return out;
  }

  std::unordered_set<std::wstring> seenLower;

  // MO2 rule: lower (bottom) has higher priority. Walk bottom->top to get winner-first ordering.
  for (auto it = linesOpt->rbegin(); it != linesOpt->rend(); ++it) {
    std::string_view line = Trim(*it);
    if (line.empty()) {
      continue;
    }
    const char prefix = line.front();
    if (prefix != '+' && prefix != '-') {
      continue;
    }
    line.remove_prefix(1);
    line = Trim(line);
    if (line.empty()) {
      continue;
    }
    if (prefix != '+') {
      continue;
    }

    std::wstring name = Utf8ToWide(line);
    if (name.empty()) {
      continue;
    }
    std::wstring lower = WideLower(name);
    if (seenLower.find(lower) != seenLower.end()) {
      continue;
    }
    seenLower.insert(lower);
    out.push_back(std::move(name));
  }

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
  idx.profilesDir = idx.base / L"profiles";

  std::error_code ec;
  if (!std::filesystem::is_directory(idx.modsDir, ec)) {
    return std::nullopt;
  }

  // Discover installed mod directories first (case-insensitive map).
  std::unordered_map<std::wstring, std::filesystem::path> dirByLower;
  std::unordered_map<std::wstring, std::wstring> nameByLower;
  dirByLower.reserve(512);
  nameByLower.reserve(512);

  for (const auto& ent : std::filesystem::directory_iterator(idx.modsDir, ec)) {
    if (ec) {
      break;
    }
    if (!ent.is_directory(ec)) {
      continue;
    }
    const auto p = ent.path();
    const auto name = p.filename().wstring();
    const auto lower = WideLower(name);
    dirByLower[lower] = p;
    nameByLower[lower] = name;
  }

  // Prefer the active profile's modlist.txt to respect MO2 left-pane priority order.
  if (auto profileDir = TryPickMo2ProfileDir(idx.base, &idx.profileName)) {
    idx.profileDir = *profileDir;
    idx.modlistPath = idx.profileDir / L"modlist.txt";
  }

  std::unordered_set<std::wstring> usedLower;
  usedLower.reserve(dirByLower.size());

  if (!idx.modlistPath.empty() && std::filesystem::exists(idx.modlistPath, ec)) {
    const auto enabledWinnerFirst = ReadMo2EnabledModsWinnerFirst(idx.modlistPath);
    idx.modDirs.reserve(enabledWinnerFirst.size());
    idx.modNames.reserve(enabledWinnerFirst.size());

    for (const auto& nameFromList : enabledWinnerFirst) {
      const auto lower = WideLower(nameFromList);
      auto it = dirByLower.find(lower);
      if (it == dirByLower.end()) {
        continue;
      }
      idx.modDirs.push_back(it->second);
      auto nit = nameByLower.find(lower);
      idx.modNames.push_back((nit != nameByLower.end()) ? nit->second : nameFromList);
      usedLower.insert(lower);
    }
  }

  // Append any remaining mod directories not present in the active profile list (deterministic by name).
  if (idx.modDirs.size() < dirByLower.size()) {
    std::vector<std::wstring> remaining;
    remaining.reserve(dirByLower.size() - idx.modDirs.size());
    for (const auto& [lower, _] : dirByLower) {
      if (usedLower.find(lower) == usedLower.end()) {
        remaining.push_back(lower);
      }
    }
    std::sort(remaining.begin(), remaining.end());
    for (const auto& lower : remaining) {
      auto it = dirByLower.find(lower);
      if (it == dirByLower.end()) {
        continue;
      }
      idx.modDirs.push_back(it->second);
      auto nit = nameByLower.find(lower);
      idx.modNames.push_back((nit != nameByLower.end()) ? nit->second : it->second.filename().wstring());
    }
  }

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
