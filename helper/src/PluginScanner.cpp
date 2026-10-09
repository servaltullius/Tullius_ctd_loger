#include "PluginScanner.h"

#include <Windows.h>

#include <ShlObj.h>
#include <TlHelp32.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "SkyrimDiagStringUtil.h"

namespace skydiag::helper {
namespace {

using skydiag::WideLower;

void StripUtf8BomInPlace(std::string& s)
{
  if (s.size() >= 3 &&
      static_cast<unsigned char>(s[0]) == 0xEF &&
      static_cast<unsigned char>(s[1]) == 0xBB &&
      static_cast<unsigned char>(s[2]) == 0xBF) {
    s.erase(0, 3);
  }
}

std::string AsciiLower(std::string_view s)
{
  std::string out;
  out.reserve(s.size());
  std::transform(s.begin(), s.end(), std::back_inserter(out), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  return out;
}

void TrimAsciiInPlace(std::string& s)
{
  while (!s.empty()) {
    const unsigned char c = static_cast<unsigned char>(s.back());
    if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
      s.pop_back();
      continue;
    }
    break;
  }
  std::size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == ' ' || c == '\t') {
      ++i;
      continue;
    }
    break;
  }
  if (i > 0) {
    s.erase(0, i);
  }
}

bool HasModule(const std::vector<std::wstring>& modules, const wchar_t* moduleName)
{
  const std::wstring target = WideLower(moduleName ? moduleName : L"");
  if (target.empty()) {
    return false;
  }
  for (const auto& mod : modules) {
    if (WideLower(mod) == target) {
      return true;
    }
  }
  return false;
}

bool EndsWithAsciiInsensitive(std::string_view value, std::string_view suffix)
{
  if (value.size() < suffix.size()) {
    return false;
  }
  return AsciiLower(value.substr(value.size() - suffix.size())) == AsciiLower(suffix);
}

std::string ParseSelectedProfileValue(std::string raw)
{
  StripUtf8BomInPlace(raw);
  TrimAsciiInPlace(raw);

  constexpr std::string_view kByteArrayPrefix = "@ByteArray(";
  if (raw.size() >= kByteArrayPrefix.size() + 1 &&
      raw.rfind(kByteArrayPrefix, 0) == 0 &&
      raw.back() == ')') {
    raw = raw.substr(kByteArrayPrefix.size(), raw.size() - kByteArrayPrefix.size() - 1);
    TrimAsciiInPlace(raw);
  }

  if (raw.size() >= 2) {
    const char first = raw.front();
    const char last = raw.back();
    if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
      raw = raw.substr(1, raw.size() - 2);
      TrimAsciiInPlace(raw);
    }
  }

  return raw;
}

std::filesystem::path ResolveGameExePathFromDir(const std::filesystem::path& gameExeDir)
{
  for (const wchar_t* name : { L"SkyrimSE.exe", L"SkyrimVR.exe", L"Skyrim.exe" }) {
    const auto candidate = gameExeDir / name;
    if (std::filesystem::exists(candidate)) {
      return candidate;
    }
  }
  return {};
}

std::string QueryFileVersionString(const std::filesystem::path& filePath)
{
  if (filePath.empty()) {
    return {};
  }
  DWORD handle = 0;
  const DWORD size = GetFileVersionInfoSizeW(filePath.c_str(), &handle);
  if (size == 0) {
    return {};
  }

  std::vector<std::uint8_t> buffer(size);
  if (!GetFileVersionInfoW(filePath.c_str(), handle, size, buffer.data())) {
    return {};
  }

  VS_FIXEDFILEINFO* ffi = nullptr;
  UINT ffiSize = 0;
  if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &ffiSize) ||
      !ffi ||
      ffiSize < sizeof(VS_FIXEDFILEINFO)) {
    return {};
  }

  const std::uint32_t ms = ffi->dwFileVersionMS;
  const std::uint32_t ls = ffi->dwFileVersionLS;
  return std::to_string(HIWORD(ms)) + "."
       + std::to_string(LOWORD(ms)) + "."
       + std::to_string(HIWORD(ls)) + "."
       + std::to_string(LOWORD(ls));
}

std::filesystem::path ResolveMo2IniFromModulePath(const std::filesystem::path& modulePath)
{
  if (modulePath.empty()) {
    return {};
  }

  const std::wstring moduleLower = WideLower(modulePath.filename().wstring());
  if (moduleLower != L"usvfs_x64.dll" && moduleLower != L"uvsfs64.dll") {
    return {};
  }

  std::vector<std::filesystem::path> candidates;
  if (modulePath.has_parent_path()) {
    candidates.push_back(modulePath.parent_path() / "ModOrganizer.ini");
    if (modulePath.parent_path().has_parent_path()) {
      candidates.push_back(modulePath.parent_path().parent_path() / "ModOrganizer.ini");
    }
  }

  for (const auto& candidate : candidates) {
    if (std::filesystem::exists(candidate)) {
      return candidate;
    }
  }
  return {};
}

std::filesystem::path FindMo2IniFromModulePaths(const std::vector<std::wstring>* modulePaths)
{
  if (!modulePaths) {
    return {};
  }

  std::unordered_set<std::wstring> seen;
  for (const auto& rawPath : *modulePaths) {
    if (rawPath.empty()) {
      continue;
    }
    const std::wstring lowered = WideLower(rawPath);
    if (!seen.insert(lowered).second) {
      continue;
    }
    if (const auto iniPath = ResolveMo2IniFromModulePath(std::filesystem::path(rawPath)); !iniPath.empty()) {
      return iniPath;
    }
  }
  return {};
}

// The game and SKSE read and write plugin and DLL names through the ANSI
// code page (plugins.txt, Skyrim.ccc, TES4 MAST records, skse64.log), so names
// with accented letters are not UTF-8. JSON and u8path need UTF-8: text that is
// not already valid UTF-8 is converted from CP_ACP.
std::string AnsiToUtf8IfNeeded(const std::string& text)
{
  if (text.empty() || text.size() > static_cast<std::size_t>(INT_MAX)) {
    return text;
  }
  const int len = static_cast<int>(text.size());
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), len, nullptr, 0) > 0) {
    return text;
  }
  const int wideLen = MultiByteToWideChar(CP_ACP, 0, text.data(), len, nullptr, 0);
  if (wideLen <= 0) {
    return {};
  }
  std::wstring wide(static_cast<std::size_t>(wideLen), L'\0');
  MultiByteToWideChar(CP_ACP, 0, text.data(), len, wide.data(), wideLen);
  const int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLen, nullptr, 0, nullptr, nullptr);
  if (utf8Len <= 0) {
    return {};
  }
  std::string utf8(static_cast<std::size_t>(utf8Len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLen, utf8.data(), utf8Len, nullptr, nullptr);
  return utf8;
}

}  // namespace

bool ParseTes4Header(const std::uint8_t* data, std::size_t size, PluginMeta& out)
{
  if (!data || size < 24) {
    return false;
  }
  if (std::memcmp(data, "TES4", 4) != 0) {
    return false;
  }

  std::uint32_t dataSize = 0;
  std::memcpy(&dataSize, data + 4, 4);

  std::uint32_t flags = 0;
  std::memcpy(&flags, data + 8, 4);
  out.is_esl = (flags & 0x0200u) != 0u;
  out.header_version = 0.0f;
  out.masters.clear();

  const std::size_t headerEnd = 24;
  const std::size_t recordEnd = std::min<std::size_t>(size, headerEnd + dataSize);
  std::size_t pos = headerEnd;
  while (pos + 6 <= recordEnd) {
    char subType[5]{};
    std::memcpy(subType, data + pos, 4);
    std::uint16_t subSize = 0;
    std::memcpy(&subSize, data + pos + 4, 2);
    pos += 6;

    if (pos + subSize > recordEnd) {
      break;
    }

    if (std::strcmp(subType, "HEDR") == 0 && subSize >= 4) {
      float headerVersion = 0.0f;
      std::memcpy(&headerVersion, data + pos, 4);
      out.header_version = headerVersion;
    } else if (std::strcmp(subType, "MAST") == 0 && subSize > 0) {
      std::string master(reinterpret_cast<const char*>(data + pos), subSize);
      if (!master.empty() && master.back() == '\0') {
        master.pop_back();
      }
      if (!master.empty()) {
        out.masters.push_back(std::move(master));
      }
    }

    pos += subSize;
  }

  return true;
}

std::vector<std::string> ParsePluginsTxt(const std::string& content)
{
  std::vector<std::string> active;
  std::vector<std::string> legacyActive;
  bool hasStarredLines = false;
  bool firstLine = true;

  std::istringstream stream(content);
  std::string line;
  while (std::getline(stream, line)) {
    if (firstLine) {
      StripUtf8BomInPlace(line);
      firstLine = false;
    }
    TrimAsciiInPlace(line);
    if (line.empty() || line[0] == '#') {
      continue;
    }

    if (line[0] == '*') {
      hasStarredLines = true;
      std::string name = line.substr(1);
      TrimAsciiInPlace(name);
      if (!name.empty()) {
        active.push_back(std::move(name));
      }
    } else {
      legacyActive.push_back(line);
    }
  }

  return hasStarredLines ? active : legacyActive;
}

std::vector<std::string> ParseCreationClubContentList(const std::string& content)
{
  std::vector<std::string> plugins;
  bool firstLine = true;
  std::istringstream stream(content);
  std::string line;
  while (std::getline(stream, line)) {
    if (firstLine) {
      StripUtf8BomInPlace(line);
      firstLine = false;
    }
    TrimAsciiInPlace(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') {
      continue;
    }
    plugins.push_back(std::move(line));
  }
  return plugins;
}

bool TryResolveGameExeDir(HANDLE processHandle, std::filesystem::path& outDir)
{
  if (!processHandle) {
    return false;
  }

  DWORD size = 32768;
  std::wstring buf(size, L'\0');
  if (!QueryFullProcessImageNameW(processHandle, 0, buf.data(), &size) || size == 0) {
    return false;
  }
  buf.resize(size);

  const std::filesystem::path exePath(buf);
  if (!exePath.has_parent_path()) {
    return false;
  }

  outDir = exePath.parent_path();
  return true;
}

std::vector<std::wstring> CollectModuleFilenamesBestEffort(std::uint32_t pid)
{
  std::vector<std::wstring> modules;
  if (pid == 0) {
    return modules;
  }

  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (snap == INVALID_HANDLE_VALUE) {
    return modules;
  }

  MODULEENTRY32W me{};
  me.dwSize = sizeof(me);
  std::unordered_set<std::wstring> seen;
  for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
    std::wstring name = me.szModule;
    if (name.empty()) {
      continue;
    }
    const std::wstring lower = WideLower(name);
    if (seen.insert(lower).second) {
      modules.push_back(std::move(name));
    }
  }

  CloseHandle(snap);
  return modules;
}

std::vector<std::wstring> CollectModulePathsBestEffort(std::uint32_t pid)
{
  std::vector<std::wstring> paths;
  if (pid == 0) {
    return paths;
  }

  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (snap == INVALID_HANDLE_VALUE) {
    return paths;
  }

  MODULEENTRY32W me{};
  me.dwSize = sizeof(me);
  std::unordered_set<std::wstring> seen;
  for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
    std::wstring path = me.szExePath;
    if (path.empty()) {
      continue;
    }
    const std::wstring lower = WideLower(path);
    if (seen.insert(lower).second) {
      paths.push_back(std::move(path));
    }
  }

  CloseHandle(snap);
  return paths;
}


std::uint64_t QueryMainModuleBaseBestEffort(std::uint32_t pid)
{
  if (pid == 0) {
    return 0;
  }
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (snap == INVALID_HANDLE_VALUE) {
    return 0;
  }
  // The first module of a snapshot is the process executable.
  MODULEENTRY32W me{};
  me.dwSize = sizeof(me);
  std::uint64_t base = 0;
  if (Module32FirstW(snap, &me)) {
    base = reinterpret_cast<std::uint64_t>(me.modBaseAddr);
  }
  CloseHandle(snap);
  return base;
}

SkseLogScan CollectSkseLogBestEffort(std::uint64_t gameImageBase)
{
  // SKSE writes to Documents\My Games\<game folder>\SKSE\skse64.log. The game
  // folder name differs by store ("Skyrim Special Edition", "... GOG", ...).
  std::vector<std::filesystem::path> logPaths;
  std::filesystem::path myGames;
  if (gameImageBase != 0) {
    wchar_t* documents = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)) && documents) {
      myGames = std::filesystem::path(documents) / L"My Games";
    }
    CoTaskMemFree(documents);  // required even when the call fails
  }
  if (!myGames.empty()) {
    // increment(ec), not a range-for: its operator++ throws on an iteration
    // error, which would throw away the finished plugin scan with it.
    std::error_code iterEc;
    for (std::filesystem::directory_iterator it(myGames, iterEc), end; !iterEc && it != end; it.increment(iterEc)) {
      const auto& entry = *it;
      std::error_code ec;
      if (!entry.is_directory(ec)) {
        continue;
      }
      if (WideLower(entry.path().filename().wstring()).rfind(L"skyrim special edition", 0) != 0) {
        continue;
      }
      auto logPath = entry.path() / L"SKSE" / L"skse64.log";
      if (std::filesystem::is_regular_file(logPath, ec)) {
        logPaths.push_back(std::move(logPath));
      }
    }
  }
  return MatchSkseLogFiles(logPaths, gameImageBase);
}

SkseLogScan MatchSkseLogFiles(const std::vector<std::filesystem::path>& logPaths, std::uint64_t gameImageBase)
{
  SkseLogScan scan{};
  if (gameImageBase == 0) {
    scan.status = "no_image_base";
    return scan;
  }
  if (logPaths.empty()) {
    scan.status = "not_found";
    return scan;
  }

  constexpr std::uintmax_t kMaxLogBytes = 32ull * 1024ull * 1024ull;
  // An executable can map at the same address again within one boot, so when
  // logs of several store folders match, the most recently written one wins.
  std::filesystem::file_time_type newestMatch{};
  for (const auto& logPath : logPaths) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(logPath, ec);
    if (ec || size == 0 || size > kMaxLogBytes) {
      continue;
    }
    const auto writeTime = std::filesystem::last_write_time(logPath, ec);
    if (ec || (scan.status == "matched" && writeTime <= newestMatch)) {
      continue;
    }
    std::ifstream in(logPath, std::ios::binary);
    if (!in.is_open()) {
      continue;
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(in.gcount()));
    auto summary = ParseSkseLog(AnsiToUtf8IfNeeded(text));
    if (summary.recognized && summary.image_base == gameImageBase) {
      scan.status = "matched";
      scan.summary = std::move(summary);
      newestMatch = writeTime;
    }
  }
  if (scan.status.empty()) {
    scan.status = "no_matching_log";
  }
  return scan;
}

PluginScanResult ScanPlugins(
  const std::filesystem::path& gameExeDir,
  const std::vector<std::wstring>& moduleFilenames,
  const std::vector<std::wstring>* modulePaths)
{
  PluginScanResult result{};
  result.game_exe_version = QueryFileVersionString(ResolveGameExePathFromDir(gameExeDir));
  result.mo2_detected = HasModule(moduleFilenames, L"usvfs_x64.dll") || HasModule(moduleFilenames, L"uvsfs64.dll");

  std::filesystem::path pluginsTxtPath;

  if (result.mo2_detected) {
    auto moIni = gameExeDir / "ModOrganizer.ini";
    if (!std::filesystem::exists(moIni)) {
      moIni = gameExeDir.parent_path() / "ModOrganizer.ini";
    }
    if (!std::filesystem::exists(moIni)) {
      moIni = FindMo2IniFromModulePaths(modulePaths);
    }
    if (std::filesystem::exists(moIni)) {
      std::ifstream ini(moIni);
      std::string line;
      std::string selectedProfile;
      bool firstLine = true;
      constexpr std::string_view kSelectedProfileKey = "selected_profile=";
      while (std::getline(ini, line)) {
        if (firstLine) {
          StripUtf8BomInPlace(line);
          firstLine = false;
        }
        TrimAsciiInPlace(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') {
          continue;
        }
        if (line.rfind(kSelectedProfileKey, 0) == 0) {
          selectedProfile = ParseSelectedProfileValue(line.substr(kSelectedProfileKey.size()));
          break;
        }
      }
      if (!selectedProfile.empty()) {
        const auto profilePath = moIni.parent_path() / "profiles" / selectedProfile / "plugins.txt";
        if (std::filesystem::exists(profilePath)) {
          pluginsTxtPath = profilePath;
          result.plugins_source = "mo2_profile";
        }
      }
    }
  }

  if (pluginsTxtPath.empty()) {
    wchar_t* localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) && localAppData) {
      const auto standardPath = std::filesystem::path(localAppData) / L"Skyrim Special Edition" / L"plugins.txt";
      CoTaskMemFree(localAppData);
      if (std::filesystem::exists(standardPath)) {
        pluginsTxtPath = standardPath;
        result.plugins_source = "standard";
      }
    }
  }

  if (pluginsTxtPath.empty()) {
    const auto fallbackPath = gameExeDir / "plugins.txt";
    if (std::filesystem::exists(fallbackPath)) {
      pluginsTxtPath = fallbackPath;
      result.plugins_source = "fallback";
    }
  }

  if (pluginsTxtPath.empty()) {
    result.plugins_source = "error";
    result.error = "Could not find plugins.txt";
    return result;
  }

  std::ifstream in(pluginsTxtPath);
  if (!in.is_open()) {
    result.plugins_source = "error";
    result.error = "Could not open plugins.txt";
    return result;
  }
  const std::string content = AnsiToUtf8IfNeeded(
    std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
  const auto listedPlugins = ParsePluginsTxt(content);

  // The game loads the base masters and the Creation Club files named in
  // Skyrim.ccc without a plugins.txt entry, whenever they are in Data. Mod
  // managers usually leave them out of plugins.txt, so without them every
  // plugin that depends on an Anniversary Edition CC file looked like it had
  // a missing master.
  const auto dataDir = gameExeDir / "Data";
  std::vector<std::string> activePlugins;
  std::unordered_set<std::string> seen;
  auto addImplicitIfPresent = [&](const std::string& name) {
    if (seen.count(AsciiLower(name)) != 0u) {
      return;
    }
    std::error_code ec;
    if (std::filesystem::exists(dataDir / std::filesystem::u8path(name), ec)) {
      seen.insert(AsciiLower(name));
      activePlugins.push_back(name);
    }
  };
  for (const char* baseMaster : { "Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm" }) {
    addImplicitIfPresent(baseMaster);
  }
  if (std::ifstream ccc(gameExeDir / "Skyrim.ccc"); ccc.is_open()) {
    const std::string cccContent = AnsiToUtf8IfNeeded(
      std::string((std::istreambuf_iterator<char>(ccc)), std::istreambuf_iterator<char>()));
    for (const auto& name : ParseCreationClubContentList(cccContent)) {
      addImplicitIfPresent(name);
    }
  }
  for (const auto& name : listedPlugins) {
    if (seen.insert(AsciiLower(name)).second) {
      activePlugins.push_back(name);
    }
  }
  result.implicit_plugins_included = true;

  for (const auto& pluginName : activePlugins) {
    PluginMeta meta{};
    meta.filename = pluginName;
    meta.is_active = true;

    const auto pluginPath = dataDir / std::filesystem::u8path(pluginName);
    std::error_code existsEc;
    if (std::filesystem::exists(pluginPath, existsEc)) {
      std::ifstream pf(pluginPath, std::ios::binary);
      if (pf.is_open()) {
        std::vector<std::uint8_t> buf(4096);
        pf.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        const std::size_t bytesRead = static_cast<std::size_t>(pf.gcount());
        meta.slot_type_known = ParseTes4Header(buf.data(), bytesRead, meta);
        for (auto& master : meta.masters) {
          master = AnsiToUtf8IfNeeded(master);
        }
      }
    }
    if (!meta.slot_type_known && EndsWithAsciiInsensitive(pluginName, ".esl")) {
      meta.is_esl = true;
      meta.slot_type_known = true;
    }
    result.plugins.push_back(std::move(meta));
  }

  return result;
}

std::string SerializePluginScanResult(const PluginScanResult& result)
{
  nlohmann::json j = nlohmann::json::object();
  j["game_exe_version"] = result.game_exe_version;
  j["plugins_source"] = result.plugins_source;
  j["mo2_detected"] = result.mo2_detected;
  j["implicit_plugins_included"] = result.implicit_plugins_included;
  j["error"] = result.error;
  j["plugins"] = nlohmann::json::array();

  for (const auto& plugin : result.plugins) {
    nlohmann::json p = nlohmann::json::object();
    p["filename"] = plugin.filename;
    p["header_version"] = plugin.header_version;
    p["is_esl"] = plugin.is_esl;
    p["is_active"] = plugin.is_active;
    p["slot_type_known"] = plugin.slot_type_known;
    p["masters"] = plugin.masters;
    j["plugins"].push_back(std::move(p));
  }

  if (!result.skse_log.status.empty()) {
    const auto& log = result.skse_log.summary;
    nlohmann::json s = nlohmann::json::object();
    s["status"] = result.skse_log.status;
    if (result.skse_log.status == "matched") {
      s["skse_version"] = log.skse_version;
      s["checked_count"] = log.checked_count;
      s["loaded_count"] = log.loaded_count;
      s["issue_count"] = log.issues.size();
      // The scan is embedded in the dump; a broken mod folder must not bloat it.
      constexpr std::size_t kMaxIssues = 64;
      s["issues"] = nlohmann::json::array();
      for (std::size_t i = 0; i < log.issues.size() && i < kMaxIssues; ++i) {
        const auto& issue = log.issues[i];
        s["issues"].push_back({
          { "dll", issue.dll_name },
          { "name", issue.plugin_name },
          { "status", issue.status },
          { "code", issue.error_code },
        });
      }
    }
    j["skse_log"] = std::move(s);
  }

  // The names above are converted to UTF-8; replace anything that still is
  // not, rather than throwing and losing the capture this scan belongs to.
  return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace skydiag::helper
