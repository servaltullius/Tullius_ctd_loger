#include "PluginScanner.h"

#include <Windows.h>

#include <ShlObj.h>
#include <TlHelp32.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace skydiag::helper {
namespace {

std::wstring WideLower(std::wstring_view s)
{
  std::wstring out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return out;
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

  std::istringstream stream(content);
  std::string line;
  while (std::getline(stream, line)) {
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

PluginScanResult ScanPlugins(
  const std::filesystem::path& gameExeDir,
  const std::vector<std::wstring>& moduleFilenames)
{
  PluginScanResult result{};
  result.mo2_detected = HasModule(moduleFilenames, L"usvfs_x64.dll") || HasModule(moduleFilenames, L"uvsfs64.dll");

  std::filesystem::path pluginsTxtPath;

  if (result.mo2_detected) {
    auto moIni = gameExeDir / "ModOrganizer.ini";
    if (!std::filesystem::exists(moIni)) {
      moIni = gameExeDir.parent_path() / "ModOrganizer.ini";
    }
    if (std::filesystem::exists(moIni)) {
      std::ifstream ini(moIni);
      std::string line;
      std::string selectedProfile;
      while (std::getline(ini, line)) {
        if (line.rfind("selected_profile=", 0) == 0) {
          selectedProfile = line.substr(17);
          TrimAsciiInPlace(selectedProfile);
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
  const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto activePlugins = ParsePluginsTxt(content);

  const auto dataDir = gameExeDir / "Data";
  for (const auto& pluginName : activePlugins) {
    PluginMeta meta{};
    meta.filename = pluginName;
    meta.is_active = true;

    const auto pluginPath = dataDir / std::filesystem::u8path(pluginName);
    if (std::filesystem::exists(pluginPath)) {
      std::ifstream pf(pluginPath, std::ios::binary);
      if (pf.is_open()) {
        std::vector<std::uint8_t> buf(4096);
        pf.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        const std::size_t bytesRead = static_cast<std::size_t>(pf.gcount());
        (void)ParseTes4Header(buf.data(), bytesRead, meta);
      }
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
  j["error"] = result.error;
  j["plugins"] = nlohmann::json::array();

  for (const auto& plugin : result.plugins) {
    nlohmann::json p = nlohmann::json::object();
    p["filename"] = plugin.filename;
    p["header_version"] = plugin.header_version;
    p["is_esl"] = plugin.is_esl;
    p["is_active"] = plugin.is_active;
    p["masters"] = plugin.masters;
    j["plugins"].push_back(std::move(p));
  }

  return j.dump();
}

}  // namespace skydiag::helper
