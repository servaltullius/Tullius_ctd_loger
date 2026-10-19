#include "PluginScanner.h"

#include <Windows.h>

#include <ShlObj.h>
#include <TlHelp32.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "SkyrimDiagQSettings.h"
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

// std::filesystem::exists(path) throws on errors such as ERROR_NOT_READY (an
// empty card reader behind a junction) or an access error from usvfs; the
// scan is optional context and must never throw for that (review M13).
bool ExistsNoThrow(const std::filesystem::path& path)
{
  std::error_code ec;
  return std::filesystem::exists(path, ec);
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

// ModOrganizer.ini's selected_profile, as QSettings wrote it (DecodeQSettingsIniValue).
std::string ParseSelectedProfileValue(std::string raw)
{
  return skydiag::DecodeQSettingsIniValue(std::move(raw));
}

std::filesystem::path ResolveGameExePathFromDir(const std::filesystem::path& gameExeDir)
{
  for (const wchar_t* name : { L"SkyrimSE.exe", L"SkyrimVR.exe", L"Skyrim.exe" }) {
    const auto candidate = gameExeDir / name;
    if (ExistsNoThrow(candidate)) {
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
    if (ExistsNoThrow(candidate)) {
      return candidate;
    }
  }
  return {};
}

// Portable ModOrganizer.ini files that may belong to the running MO2: next to
// or above the game folder, then next to the loaded usvfs DLL. Existing files
// only, in that order, without duplicates.
std::vector<Mo2InstanceCandidate> CollectPortableMo2Inis(
  const std::filesystem::path& gameExeDir,
  const std::vector<std::wstring>* modulePaths)
{
  std::vector<Mo2InstanceCandidate> out;
  std::unordered_set<std::wstring> seenInis;
  const auto add = [&](const std::filesystem::path& ini) {
    if (ini.empty() || !ExistsNoThrow(ini)) {
      return;
    }
    const auto normalized = ini.lexically_normal();
    if (!seenInis.insert(WideLower(normalized.wstring())).second) {
      return;
    }
    // MO2 always runs the portable instance of an install holding portable.txt.
    out.push_back(Mo2InstanceCandidate{ normalized, ExistsNoThrow(normalized.parent_path() / L"portable.txt") });
  };
  add(gameExeDir / "ModOrganizer.ini");
  add(gameExeDir.parent_path() / "ModOrganizer.ini");
  if (modulePaths) {
    std::unordered_set<std::wstring> seenModules;
    for (const auto& rawPath : *modulePaths) {
      if (rawPath.empty() || !seenModules.insert(WideLower(rawPath)).second) {
        continue;
      }
      add(ResolveMo2IniFromModulePath(std::filesystem::path(rawPath)));
    }
  }
  return out;
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

// path::u8string() throws on an unpaired UTF-16 surrogate (an NTFS name can
// hold one) and u8path() throws on bytes that are not UTF-8. These never
// throw: such a character becomes U+FFFD instead.
std::string PathToUtf8(const std::filesystem::path& path)
{
  const std::wstring& wide = path.native();
  if (wide.empty() || wide.size() > static_cast<std::size_t>(INT_MAX)) {
    return {};
  }
  const int len = static_cast<int>(wide.size());
  const int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), len, nullptr, 0, nullptr, nullptr);
  if (utf8Len <= 0) {
    return {};
  }
  std::string utf8(static_cast<std::size_t>(utf8Len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), len, utf8.data(), utf8Len, nullptr, nullptr);
  return utf8;
}

std::filesystem::path Utf8ToPath(const std::string& utf8)
{
  if (utf8.empty() || utf8.size() > static_cast<std::size_t>(INT_MAX)) {
    return {};
  }
  const int len = static_cast<int>(utf8.size());
  const int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), len, nullptr, 0);
  if (wideLen <= 0) {
    return {};
  }
  std::wstring wide(static_cast<std::size_t>(wideLen), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), len, wide.data(), wideLen);
  return std::filesystem::path(std::move(wide));
}

// The TES4 record (24-byte header plus its data) at the start of a plugin.
// The master list of a plugin with ~90 or more masters runs past 4 KB, which
// is all that used to be read (review M12); the record's own size says how
// much to read, capped against a corrupt size. Masters precede ONAM, the only
// large TES4 field, so the cap does not cut them in practice.
std::vector<std::uint8_t> ReadTes4Record(const std::filesystem::path& path)
{
  std::vector<std::uint8_t> buf;
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return buf;
  }
  constexpr std::size_t kRecordHeaderBytes = 24;
  constexpr std::size_t kMaxTes4RecordBytes = std::size_t{256} * 1024;
  buf.resize(kRecordHeaderBytes);
  in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
  buf.resize(static_cast<std::size_t>(in.gcount()));
  if (buf.size() < kRecordHeaderBytes || std::memcmp(buf.data(), "TES4", 4) != 0) {
    return buf;
  }
  std::uint32_t dataSize = 0;
  std::memcpy(&dataSize, buf.data() + 4, 4);
  const std::size_t want =
    std::min<std::size_t>(kRecordHeaderBytes + static_cast<std::size_t>(dataSize), kMaxTes4RecordBytes);
  buf.resize(want);
  in.read(reinterpret_cast<char*>(buf.data() + kRecordHeaderBytes),
    static_cast<std::streamsize>(want - kRecordHeaderBytes));
  buf.resize(kRecordHeaderBytes + static_cast<std::size_t>(in.gcount()));
  return buf;
}

// CreateToolhelp32Snapshot fails with ERROR_BAD_LENGTH while the target is
// loading or unloading modules (review L6), e.g. when the helper attaches
// while SKSE plugins load; retry briefly instead of reporting no modules.
HANDLE SnapModulesWithRetry(std::uint32_t pid)
{
  for (int attempt = 0;; ++attempt) {
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH || attempt >= 4) {
      return snap;
    }
    Sleep(20);
  }
}

bool ReadWholeFile(const std::filesystem::path& path, std::string* out)
{
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return false;
  }
  *out = std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return true;
}

// A global (non-portable) MO2 instance keeps its ModOrganizer.ini under
// %LOCALAPPDATA%\ModOrganizer\<instance>, not next to ModOrganizer.exe; MO2
// records the instance it runs as CurrentInstance (empty for portable).
std::filesystem::path FindGlobalMo2InstanceIni()
{
  wchar_t name[512]{};
  DWORD bytes = sizeof(name);
  if (RegGetValueW(
        HKEY_CURRENT_USER, L"Software\\Mod Organizer Team\\Mod Organizer", L"CurrentInstance", RRF_RT_REG_SZ, nullptr,
        name, &bytes) != ERROR_SUCCESS ||
      name[0] == L'\0') {
    return {};
  }
  wchar_t* localAppData = nullptr;
  std::filesystem::path ini;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) && localAppData) {
    ini = std::filesystem::path(localAppData) / L"ModOrganizer" / name / L"ModOrganizer.ini";
  }
  CoTaskMemFree(localAppData);
  std::error_code ec;
  return (!ini.empty() && std::filesystem::is_regular_file(ini, ec)) ? ini : std::filesystem::path{};
}

}  // namespace

Mo2InstanceChoice ChooseMo2InstanceIni(
  const std::vector<Mo2InstanceCandidate>& portableInis,
  const std::filesystem::path& globalInstanceIni)
{
  Mo2InstanceChoice choice{};
  for (const auto& candidate : portableInis) {
    if (candidate.portableLocked) {
      choice.ini = candidate.ini;
      choice.source = "portable_locked";
      return choice;
    }
  }
  if (!globalInstanceIni.empty()) {
    choice.ini = globalInstanceIni;
    choice.source = "global";
    // A portable ModOrganizer.ini is around too. MO2 would pick the global
    // instance, but one started with an explicit instance argument may not.
    choice.ambiguous = !portableInis.empty();
    return choice;
  }
  if (!portableInis.empty()) {
    choice.ini = portableInis.front().ini;
    choice.source = "portable";
  }
  return choice;
}

Mo2IniSettings ParseMo2Ini(const std::string& content)
{
  Mo2IniSettings settings{};
  std::istringstream in(content);
  std::string line;
  std::string section;
  bool firstLine = true;
  while (std::getline(in, line)) {
    if (firstLine) {
      StripUtf8BomInPlace(line);
      firstLine = false;
    }
    TrimAsciiInPlace(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') {
      continue;
    }
    if (line.front() == '[' && line.back() == ']') {
      section = AsciiLower(line.substr(1, line.size() - 2));
      continue;
    }
    const auto eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    std::string key = line.substr(0, eq);
    TrimAsciiInPlace(key);
    key = AsciiLower(key);
    const std::string value = ParseSelectedProfileValue(line.substr(eq + 1));
    if (section == "general" && key == "selected_profile") {
      settings.selected_profile = value;
    } else if (section == "settings") {
      if (key == "base_directory") {
        settings.base_directory = value;
      } else if (key == "mod_directory") {
        settings.mod_directory = value;
      } else if (key == "profiles_directory") {
        settings.profiles_directory = value;
      } else if (key == "overwrite_directory") {
        settings.overwrite_directory = value;
      }
    }
  }
  return settings;
}

std::filesystem::path ResolveMo2Directory(
  const std::string& configured,
  const std::filesystem::path& baseDir,
  const wchar_t* defaultLeaf)
{
  if (configured.empty()) {
    return (baseDir / defaultLeaf).lexically_normal();
  }
  std::string value = configured;
  constexpr std::string_view kBaseDir = "%base_dir%";
  if (const auto pos = AsciiLower(value).find(kBaseDir); pos != std::string::npos) {
    value = value.substr(0, pos) + PathToUtf8(baseDir) + value.substr(pos + kBaseDir.size());
  }
  std::filesystem::path path = Utf8ToPath(value);
  if (path.is_relative()) {
    path = baseDir / path;
  }
  return path.lexically_normal();
}

std::vector<std::string> ParseMo2EnabledModsWinnerFirst(const std::string& modlistContent)
{
  // MO2 writes modlist.txt highest priority first (Profile::doWriteModlist):
  // "+Name" enabled, "-Name" disabled, "*Name" unmanaged (DLC, Creation Club).
  std::vector<std::string> mods;
  std::unordered_set<std::string> seen;
  std::istringstream in(modlistContent);
  std::string line;
  bool firstLine = true;
  while (std::getline(in, line)) {
    if (firstLine) {
      StripUtf8BomInPlace(line);
      firstLine = false;
    }
    TrimAsciiInPlace(line);
    if (line.size() < 2 || line[0] != '+') {
      continue;
    }
    std::string name = line.substr(1);
    TrimAsciiInPlace(name);
    if (!name.empty() && seen.insert(AsciiLower(name)).second) {
      mods.push_back(std::move(name));
    }
  }
  return mods;
}

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
  out.masters_truncated = false;

  const std::size_t headerEnd = 24;
  const std::size_t fullRecordEnd = headerEnd + static_cast<std::size_t>(dataSize);
  const std::size_t recordEnd = std::min<std::size_t>(size, fullRecordEnd);
  std::size_t pos = headerEnd;
  // Set once a field that follows the master list (ONAM, INTV, INCC, ...) is
  // reached: a record cut off after that point still has every master.
  bool pastMasterList = false;
  // An XXXX field carries the 32-bit size of the next field.
  std::optional<std::uint32_t> nextSizeOverride;
  while (pos + 6 <= recordEnd) {
    char subType[5]{};
    std::memcpy(subType, data + pos, 4);
    std::uint16_t subSize16 = 0;
    std::memcpy(&subSize16, data + pos + 4, 2);
    pos += 6;
    const std::size_t subSize = nextSizeOverride ? *nextSizeOverride : subSize16;
    nextSizeOverride.reset();

    if (subSize > recordEnd - pos) {
      break;
    }

    if (std::strcmp(subType, "XXXX") == 0 && subSize == 4) {
      std::uint32_t nextSize = 0;
      std::memcpy(&nextSize, data + pos, 4);
      nextSizeOverride = nextSize;
    } else if (std::strcmp(subType, "HEDR") != 0 && std::strcmp(subType, "CNAM") != 0 &&
               std::strcmp(subType, "SNAM") != 0 && std::strcmp(subType, "MAST") != 0 &&
               std::strcmp(subType, "DATA") != 0) {
      pastMasterList = true;
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

  // The bytes given end inside the record before the master list was known to
  // be complete: missing-master checks only see the masters listed here.
  out.masters_truncated = size < fullRecordEnd && !pastMasterList;
  return true;
}

std::vector<std::string> ParsePluginsTxt(const std::string& content)
{
  std::vector<std::string> active;
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
      std::string name = line.substr(1);
      TrimAsciiInPlace(name);
      if (!name.empty()) {
        active.push_back(std::move(name));
      }
    }
  }

  // Skyrim SE/AE (and MO2, Vortex) mark active plugins with '*'; a line
  // without it is an inactive plugin, so a list with no '*' at all has none
  // active. The unstarred "legacy" reading made every disabled plugin active
  // (missing-master and header-1.71 warnings for plugins that were off).
  return active;
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

  HANDLE snap = SnapModulesWithRetry(pid);
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

  HANDLE snap = SnapModulesWithRetry(pid);
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
  HANDLE snap = SnapModulesWithRetry(pid);
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

std::optional<std::filesystem::file_time_type> QueryProcessStartTimeBestEffort(HANDLE process)
{
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!process || !GetProcessTimes(process, &created, &exited, &kernel, &user)) {
    return std::nullopt;
  }
  // MSVC's file_clock counts 100 ns ticks from 1601-01-01, as FILETIME does.
  const auto ticks = (static_cast<std::int64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
  if (ticks == 0) {
    return std::nullopt;
  }
  return std::filesystem::file_time_type(std::filesystem::file_time_type::duration(ticks));
}

SkseLogScan CollectSkseLogBestEffort(
  std::uint64_t gameImageBase,
  std::optional<std::filesystem::file_time_type> processStart)
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
  return MatchSkseLogFiles(logPaths, gameImageBase, processStart);
}

SkseLogScan MatchSkseLogFiles(
  const std::vector<std::filesystem::path>& logPaths,
  std::uint64_t gameImageBase,
  std::optional<std::filesystem::file_time_type> processStart)
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
    // SKSE writes this log from inside the game process, so a log last written
    // before the process started belongs to an earlier launch whatever its
    // image base. The margin covers file time granularity.
    constexpr auto kStartMargin = std::chrono::seconds(2);
    if (processStart && writeTime + kStartMargin < *processStart) {
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
      scan.time_checked = processStart.has_value();
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
    // Review M14: a portable ModOrganizer.ini left next to the game used to win
    // over the global instance MO2 actually runs; MO2's own order is used now.
    const auto instance = ChooseMo2InstanceIni(
      CollectPortableMo2Inis(gameExeDir, modulePaths), FindGlobalMo2InstanceIni());
    const auto& moIni = instance.ini;
    std::string iniText;
    if (moIni.empty() || !ReadWholeFile(moIni, &iniText)) {
      result.mo2.status = "no_ini";
    } else {
      try {
        result.mo2.instance_source = instance.source;
        result.mo2.instance_ambiguous = instance.ambiguous;
        // MO2 cannot switch profiles while the game runs, so what the INI and the
        // profile say now is what the game was started with.
        auto settings = ParseMo2Ini(AnsiToUtf8IfNeeded(iniText));
        // An @ByteArray \xNN escape can leave bytes that are not UTF-8.
        for (auto* value : { &settings.selected_profile, &settings.base_directory, &settings.mod_directory,
                             &settings.profiles_directory, &settings.overwrite_directory }) {
          *value = AnsiToUtf8IfNeeded(*value);
        }
        const auto iniDir = moIni.parent_path().lexically_normal();
        const auto baseDir = settings.base_directory.empty()
          ? iniDir
          : ResolveMo2Directory(settings.base_directory, iniDir, L"");
        const auto profilesDir = ResolveMo2Directory(settings.profiles_directory, baseDir, L"profiles");
        result.mo2.ini_path = PathToUtf8(moIni.lexically_normal());
        result.mo2.base_dir = PathToUtf8(baseDir);
        result.mo2.mods_dir = PathToUtf8(ResolveMo2Directory(settings.mod_directory, baseDir, L"mods"));
        result.mo2.profiles_dir = PathToUtf8(profilesDir);
        result.mo2.overwrite_dir = PathToUtf8(ResolveMo2Directory(settings.overwrite_directory, baseDir, L"overwrite"));
        result.mo2.profile = settings.selected_profile;

        const auto profileDir = profilesDir / Utf8ToPath(settings.selected_profile);
        std::error_code ec;
        if (settings.selected_profile.empty() || !std::filesystem::is_directory(profileDir, ec)) {
          result.mo2.status = "no_profile";
        } else {
          const auto profilePluginsTxt = profileDir / "plugins.txt";
          if (std::filesystem::exists(profilePluginsTxt, ec)) {
            pluginsTxtPath = profilePluginsTxt;
            result.plugins_source = "mo2_profile";
          }
          std::string modlist;
          if (!ReadWholeFile(profileDir / "modlist.txt", &modlist)) {
            result.mo2.status = "no_modlist";
          } else {
            auto mods = ParseMo2EnabledModsWinnerFirst(AnsiToUtf8IfNeeded(modlist));
            // The scan is embedded in the dump; large lists run to a few thousand.
            constexpr std::size_t kMaxCapturedMods = 20000;
            if (mods.size() > kMaxCapturedMods) {
              mods.resize(kMaxCapturedMods);
              result.mo2.enabled_mods_truncated = true;
            }
            result.mo2.enabled_mods = std::move(mods);
            result.mo2.status = "captured";
          }
        }
      } catch (const std::exception&) {
        // A path that does not convert must not cost the plugin scan below.
        result.mo2 = {};
        result.mo2.status = "error";
      }
    }
  }

  if (pluginsTxtPath.empty()) {
    wchar_t* localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) && localAppData) {
      // GOG and Epic keep their list under their own folder name (MO2 tells
      // them apart by Galaxy64.dll / EOSSDK-Win64-Shipping.dll next to the
      // exe); reading the Steam list there would describe another install.
      const wchar_t* folder = ExistsNoThrow(gameExeDir / L"Galaxy64.dll")
        ? L"Skyrim Special Edition GOG"
        : ExistsNoThrow(gameExeDir / L"EOSSDK-Win64-Shipping.dll") ? L"Skyrim Special Edition EPIC"
                                                                  : L"Skyrim Special Edition";
      const auto standardPath = std::filesystem::path(localAppData) / folder / L"plugins.txt";
      CoTaskMemFree(localAppData);
      if (ExistsNoThrow(standardPath)) {
        pluginsTxtPath = standardPath;
        result.plugins_source = "standard";
      }
    }
  }

  if (pluginsTxtPath.empty()) {
    const auto fallbackPath = gameExeDir / "plugins.txt";
    if (ExistsNoThrow(fallbackPath)) {
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
    if (std::filesystem::exists(dataDir / Utf8ToPath(name), ec)) {
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
  // Started by the game, this helper inherits MO2's virtual file system and
  // sees the Data folder the game sees; started any other way under MO2 it
  // sees only the real folder, where mod-provided CC files are absent.
  const bool seesGameData = !result.mo2_detected || GetModuleHandleW(L"usvfs_x64.dll") != nullptr;
  result.implicit_masters_verified = seesGameData;

  for (const auto& pluginName : activePlugins) {
    PluginMeta meta{};
    meta.filename = pluginName;
    meta.is_active = true;

    const auto pluginPath = dataDir / Utf8ToPath(pluginName);
    std::error_code existsEc;
    if (std::filesystem::exists(pluginPath, existsEc)) {
      const auto record = ReadTes4Record(pluginPath);
      meta.slot_type_known = ParseTes4Header(record.data(), record.size(), meta);
      for (auto& master : meta.masters) {
        master = AnsiToUtf8IfNeeded(master);
      }
    }
    // The game loads every .esl file as light whatever its header flag says
    // (review L5): a renamed .esl without the flag used the full count.
    if (EndsWithAsciiInsensitive(pluginName, ".esl")) {
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
  j["implicit_masters_verified"] = result.implicit_masters_verified;
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
    if (plugin.masters_truncated) {
      p["masters_truncated"] = true;
    }
    j["plugins"].push_back(std::move(p));
  }

  if (!result.mo2.status.empty()) {
    const auto& mo2 = result.mo2;
    j["mo2"] = {
      { "status", mo2.status },
      { "ini_path", mo2.ini_path },
      { "base_dir", mo2.base_dir },
      { "mods_dir", mo2.mods_dir },
      { "profiles_dir", mo2.profiles_dir },
      { "overwrite_dir", mo2.overwrite_dir },
      { "profile", mo2.profile },
      { "enabled_mods", mo2.enabled_mods },
      { "enabled_mods_truncated", mo2.enabled_mods_truncated },
    };
    if (!mo2.instance_source.empty()) {
      j["mo2"]["instance_source"] = mo2.instance_source;
      j["mo2"]["instance_ambiguous"] = mo2.instance_ambiguous;
    }
  }

  if (!result.skse_log.status.empty()) {
    const auto& log = result.skse_log.summary;
    nlohmann::json s = nlohmann::json::object();
    s["status"] = result.skse_log.status;
    if (result.skse_log.status == "matched") {
      s["time_checked"] = result.skse_log.time_checked;
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
