#include "Mo2Index.h"
#include "SkyrimDiagQSettings.h"
#include "SkyrimDiagStringUtil.h"
#include "Utf.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace skydiag::dump_tool {
namespace {

using skydiag::WideLower;

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

    // selected_profile=@ByteArray(Default); a name with Korean letters is
    // written as \xNN escapes inside it (DecodeQSettingsIniValue).
    const std::string v = skydiag::DecodeQSettingsIniValue(std::string(line.substr(kKey.size())));
    if (v.empty()) {
      return std::nullopt;
    }
    return Utf8OrAnsiToWide(v);
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
    const auto& p = ent.path();
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

  // MO2 writes modlist.txt highest priority first (the bottom of its left
  // pane is the top of the file; Profile::doWriteModlist), so the file order
  // is already winner-first. Reading it bottom-up, as before v0.2.65, took
  // the losing mod's file (CrashLogger.ini, a mesh provider) for the winner.
  for (auto it = linesOpt->begin(); it != linesOpt->end(); ++it) {
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

  return CleanMo2ModFolderName(s.substr(start, end - start));
}

std::wstring CleanMo2ModFolderName(std::wstring_view folderName)
{
  std::vector<std::wstring_view> tokens;
  std::vector<std::size_t> starts;
  for (std::size_t pos = 0; pos < folderName.size();) {
    const std::size_t space = folderName.find(L' ', pos);
    const std::size_t end = space == std::wstring_view::npos ? folderName.size() : space;
    if (end > pos) {
      tokens.push_back(folderName.substr(pos, end - pos));
      starts.push_back(pos);
    }
    pos = end + 1;
  }
  const auto isDigit = [](wchar_t c) { return c >= L'0' && c <= L'9'; };
  const auto isAlnum = [&](wchar_t c) { return isDigit(c) || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); };
  const auto allOf = [](std::wstring_view token, auto predicate) {
    return !token.empty() && std::all_of(token.begin(), token.end(), predicate);
  };
  // "2026" or "2026-08-21T09-21Z"
  const auto isUploadDate = [&](std::wstring_view token, bool* withTime) {
    *withTime = false;
    if (token.size() == 4) {
      return allOf(token, isDigit) && token[0] == L'2' && token[1] == L'0';
    }
    if (token.size() != 17 || token[4] != L'-' || token[7] != L'-' || token[10] != L'T' || token[13] != L'-' ||
        token[16] != L'Z' || token[0] != L'2' || token[1] != L'0') {
      return false;
    }
    for (const std::size_t i : { 0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u, 11u, 12u, 14u, 15u }) {
      if (!isDigit(token[i])) {
        return false;
      }
    }
    *withTime = true;
    return true;
  };
  const auto isVersion = [&](std::wstring_view token) {
    return allOf(token, [&](wchar_t c) { return isAlnum(c) || c == L'.' || c == L'-' || c == L'_'; }) &&
      std::any_of(token.begin(), token.end(), isDigit);
  };

  std::size_t last = tokens.size();
  bool withTime = false;
  // A download hash follows a full timestamp ("yIjvuh041").
  if (last >= 5 && tokens[last - 1].size() >= 6 && tokens[last - 1].size() <= 12 && allOf(tokens[last - 1], isAlnum) &&
      isUploadDate(tokens[last - 2], &withTime) && withTime) {
    --last;
  }
  // Name, Nexus mod ID, version, upload date.
  if (last < 4 || !isUploadDate(tokens[last - 1], &withTime) || !isVersion(tokens[last - 2]) ||
      tokens[last - 3].size() < 2 || tokens[last - 3].size() > 7 || !allOf(tokens[last - 3], isDigit)) {
    return std::wstring(folderName);
  }
  std::wstring_view name = folderName.substr(0, starts[last - 3]);
  while (!name.empty() && (name.back() == L' ' || name.back() == L'-')) {
    name.remove_suffix(1);
  }
  return name.empty() ? std::wstring(folderName) : std::wstring(name);
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
    const auto& p = ent.path();
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

  bool activeProfileModlistLoaded = false;
  if (!idx.modlistPath.empty() && std::filesystem::exists(idx.modlistPath, ec)) {
    activeProfileModlistLoaded = true;
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

  // Without an active profile, fall back to installed dirs. When a profile was read,
  // keep provider hints scoped to enabled mods so disabled profile entries do not look active.
  if (!activeProfileModlistLoaded && idx.modDirs.size() < dirByLower.size()) {
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

std::optional<CapturedMo2State> TryParseCapturedMo2State(std::string_view pluginScanJsonUtf8)
{
  if (pluginScanJsonUtf8.empty()) {
    return std::nullopt;
  }
  const auto j = nlohmann::json::parse(pluginScanJsonUtf8, nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) {
    return std::nullopt;
  }
  const auto it = j.find("mo2");
  if (it == j.end() || !it->is_object() || it->value("status", std::string{}) != "captured") {
    return std::nullopt;
  }
  const auto pathOf = [&](const char* key) {
    const auto v = it->find(key);
    return (v != it->end() && v->is_string()) ? std::filesystem::path(Utf8ToWide(v->get<std::string>()))
                                              : std::filesystem::path{};
  };

  CapturedMo2State state{};
  state.base = pathOf("base_dir");
  state.modsDir = pathOf("mods_dir");
  state.overwriteDir = pathOf("overwrite_dir");
  state.profilesDir = pathOf("profiles_dir");
  if (const auto p = it->find("profile"); p != it->end() && p->is_string()) {
    state.profileName = Utf8ToWide(p->get<std::string>());
  }
  if (const auto mods = it->find("enabled_mods"); mods != it->end() && mods->is_array()) {
    state.enabledModsWinnerFirst.reserve(mods->size());
    for (const auto& m : *mods) {
      if (m.is_string() && !m.get_ref<const std::string&>().empty()) {
        state.enabledModsWinnerFirst.push_back(Utf8ToWide(m.get<std::string>()));
      }
    }
  }
  if (state.modsDir.empty()) {
    return std::nullopt;
  }
  return state;
}

Mo2Index BuildMo2IndexFromCapture(const CapturedMo2State& state)
{
  Mo2Index idx{};
  idx.source = Mo2Index::Source::kCrashTime;
  idx.base = state.base;
  idx.modsDir = state.modsDir;
  idx.overwriteDir = state.overwriteDir;
  idx.profilesDir = state.profilesDir;
  idx.profileName = state.profileName;
  // The names come from the dump. Each is one folder under profiles or mods:
  // a separator, a drive or ".." would make it name another folder (an
  // absolute "\\server\share" replaces the parent path outright).
  const auto isPlainFolderName = [](const std::wstring& name) {
    return !name.empty() && name != L"." && name != L".." && name.find_first_of(L"\\/:") == std::wstring::npos;
  };
  if (!state.profilesDir.empty() && isPlainFolderName(state.profileName)) {
    idx.profileDir = state.profilesDir / state.profileName;
    idx.modlistPath = idx.profileDir / L"modlist.txt";
  }
  idx.modDirs.reserve(state.enabledModsWinnerFirst.size());
  idx.modNames.reserve(state.enabledModsWinnerFirst.size());
  for (const auto& name : state.enabledModsWinnerFirst) {
    if (!isPlainFolderName(name)) {
      continue;
    }
    idx.modDirs.push_back(state.modsDir / name);
    idx.modNames.push_back(name);
  }
  return idx;
}

std::vector<std::wstring> FindMo2ProvidersForDataPath(
  const Mo2Index& idx,
  std::wstring_view relPath,
  std::size_t maxProviders,
  Mo2FolderCache* folderCache)
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

  // The folders above the file, shallowest first, with their cache entries.
  std::vector<std::pair<std::filesystem::path, std::vector<std::uint8_t>*>> folders;
  if (folderCache) {
    std::filesystem::path folder;
    std::wstring key;
    const auto parent = rel.parent_path();
    for (const auto& part : parent) {
      folder /= part;
      if (!key.empty()) {
        key += L'\\';
      }
      key += WideLower(part.wstring());
      auto& presence = folderCache->presence[key];
      presence.resize(idx.modDirs.size(), 0u);
      folders.emplace_back(folder, &presence);
    }
  }

  for (std::size_t i = 0; i < idx.modDirs.size(); i++) {
    bool possible = true;
    for (auto& [folder, presence] : folders) {
      auto& state = (*presence)[i];
      if (state == 0u) {
        state = std::filesystem::is_directory(idx.modDirs[i] / folder, ec) ? 1u : 2u;
      }
      if (state == 2u) {
        possible = false;
        break;
      }
    }
    if (possible && std::filesystem::exists(idx.modDirs[i] / rel, ec)) {
      out.push_back(CleanMo2ModFolderName(idx.modNames[i]));
      if (out.size() >= maxProviders) {
        break;
      }
    }
  }

  return out;
}

}  // namespace skydiag::dump_tool
