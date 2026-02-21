#include "CompatibilityPreflight.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "HelperCommon.h"
#include "HelperLog.h"
#include "SkyrimDiagHelper/PluginScanner.h"

namespace skydiag::helper::internal {
namespace {

struct PreflightCheck
{
  std::string id;
  std::string status;  // ok|warn|error
  std::string severity;
  std::string messageKo;
  std::string messageEn;
};

std::string AsciiLower(std::string_view s)
{
  std::string out;
  out.reserve(s.size());
  std::transform(s.begin(), s.end(), std::back_inserter(out), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  return out;
}

std::wstring WideLower(std::wstring_view s)
{
  std::wstring out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](wchar_t c) {
    return static_cast<wchar_t>(std::towlower(c));
  });
  return out;
}

std::wstring Utf8ToWide(std::string_view s)
{
  if (s.empty()) {
    return {};
  }
  const int needed = MultiByteToWideChar(
    CP_UTF8,
    0,
    s.data(),
    static_cast<int>(s.size()),
    nullptr,
    0);
  if (needed <= 0) {
    return {};
  }
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(
    CP_UTF8,
    0,
    s.data(),
    static_cast<int>(s.size()),
    out.data(),
    needed);
  return out;
}

bool VersionLessThan(std::string_view lhs, std::string_view rhs)
{
  auto parse = [](std::string_view s) {
    std::vector<int> out;
    std::size_t i = 0;
    while (i < s.size()) {
      std::size_t start = i;
      while (i < s.size() && s[i] != '.') {
        ++i;
      }
      int value = 0;
      bool hasDigit = false;
      for (std::size_t p = start; p < i; ++p) {
        if (!std::isdigit(static_cast<unsigned char>(s[p]))) {
          break;
        }
        hasDigit = true;
        value = (value * 10) + (s[p] - '0');
      }
      out.push_back(hasDigit ? value : 0);
      if (i < s.size() && s[i] == '.') {
        ++i;
      }
    }
    return out;
  };

  const auto lv = parse(lhs);
  const auto rv = parse(rhs);
  const std::size_t n = std::max(lv.size(), rv.size());
  for (std::size_t i = 0; i < n; ++i) {
    const int a = (i < lv.size()) ? lv[i] : 0;
    const int b = (i < rv.size()) ? rv[i] : 0;
    if (a < b) {
      return true;
    }
    if (a > b) {
      return false;
    }
  }
  return false;
}

std::string QueryFileVersionString(const std::filesystem::path& filePath)
{
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
  if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &ffiSize) || !ffi || ffiSize < sizeof(VS_FIXEDFILEINFO)) {
    return {};
  }

  const std::uint32_t ms = ffi->dwFileVersionMS;
  const std::uint32_t ls = ffi->dwFileVersionLS;
  return std::to_string(HIWORD(ms)) + "."
       + std::to_string(LOWORD(ms)) + "."
       + std::to_string(HIWORD(ls)) + "."
       + std::to_string(LOWORD(ls));
}

bool FileExistsAny(const std::filesystem::path& dir, std::initializer_list<const wchar_t*> names)
{
  for (const wchar_t* n : names) {
    if (!n || !*n) {
      continue;
    }
    if (std::filesystem::exists(dir / n)) {
      return true;
    }
  }
  return false;
}

bool HasModuleAny(const std::vector<std::wstring>& modules, std::initializer_list<const wchar_t*> names)
{
  std::vector<std::wstring> lowered;
  lowered.reserve(modules.size());
  for (const auto& m : modules) {
    lowered.push_back(WideLower(m));
  }
  for (const wchar_t* n : names) {
    if (!n || !*n) {
      continue;
    }
    const std::wstring needle = WideLower(n);
    if (std::find(lowered.begin(), lowered.end(), needle) != lowered.end()) {
      return true;
    }
  }
  return false;
}

bool AnyPluginHeaderVersionGte(const skydiag::helper::PluginScanResult& scan, double threshold)
{
  for (const auto& p : scan.plugins) {
    if (static_cast<double>(p.header_version) + 1e-6 >= threshold) {
      return true;
    }
  }
  return false;
}

nlohmann::json ToJson(const PreflightCheck& c)
{
  return nlohmann::json{
    { "id", c.id },
    { "status", c.status },
    { "severity", c.severity },
    { "message_ko", c.messageKo },
    { "message_en", c.messageEn },
  };
}

}  // namespace

void RunCompatibilityPreflight(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase)
{
  if (!cfg.enableCompatibilityPreflight) {
    return;
  }

  std::filesystem::path gameExeDir;
  if (!skydiag::helper::TryResolveGameExeDir(proc.process, gameExeDir)) {
    AppendLogLine(outBase, L"Compatibility preflight skipped: failed to resolve game exe directory.");
    return;
  }

  const auto moduleNames = skydiag::helper::CollectModuleFilenamesBestEffort(proc.pid);
  auto pluginScan = skydiag::helper::ScanPlugins(gameExeDir, moduleNames);

  std::filesystem::path gameExePath;
  for (const wchar_t* name : { L"SkyrimSE.exe", L"SkyrimVR.exe", L"Skyrim.exe" }) {
    const auto candidate = gameExeDir / name;
    if (std::filesystem::exists(candidate)) {
      gameExePath = candidate;
      break;
    }
  }

  if (pluginScan.game_exe_version.empty() && !gameExePath.empty()) {
    pluginScan.game_exe_version = QueryFileVersionString(gameExePath);
  }

  const std::filesystem::path pluginsDir = gameExeDir / L"Data" / L"SKSE" / L"Plugins";
  const bool hasCrashLoggerBinary = FileExistsAny(pluginsDir, { L"CrashLoggerSSE.dll", L"CrashLogger.dll" });
  const bool hasTrainwreckBinary = FileExistsAny(pluginsDir, { L"trainwreck.dll", L"Trainwreck.dll" });

  const bool beesModuleLoaded = HasModuleAny(moduleNames, { L"bees.dll", L"BackportedESLSupport.dll" });
  const bool beesBinaryPresent = FileExistsAny(pluginsDir, { L"bees.dll", L"BackportedESLSupport.dll" });
  const bool needsBees = AnyPluginHeaderVersionGte(pluginScan, 1.71);
  const bool runtimeNeedsBees =
    !pluginScan.game_exe_version.empty() && VersionLessThan(pluginScan.game_exe_version, "1.6.1130");

  std::vector<PreflightCheck> checks;
  checks.reserve(4);

  checks.push_back(PreflightCheck{
    "PLUGIN_SCAN_SOURCE",
    pluginScan.plugins_source == "error" ? "warn" : "ok",
    pluginScan.plugins_source == "error" ? "medium" : "low",
    pluginScan.plugins_source == "error"
      ? "plugins.txt를 찾지 못해 플러그인 사전 점검이 제한되었습니다."
      : "플러그인 목록 사전 점검이 정상적으로 수행되었습니다.",
    pluginScan.plugins_source == "error"
      ? "plugins.txt not found; plugin preflight checks are limited."
      : "Plugin list preflight check completed successfully.",
  });

  checks.push_back(PreflightCheck{
    "CRASH_LOGGER_CONFLICT",
    (hasCrashLoggerBinary && hasTrainwreckBinary) ? "warn" : "ok",
    (hasCrashLoggerBinary && hasTrainwreckBinary) ? "high" : "low",
    (hasCrashLoggerBinary && hasTrainwreckBinary)
      ? "Crash Logger 계열 DLL이 중복 감지되었습니다. 하나만 활성화하는 것을 권장합니다."
      : "Crash Logger 계열 중복 DLL은 감지되지 않았습니다.",
    (hasCrashLoggerBinary && hasTrainwreckBinary)
      ? "Multiple crash logger binaries detected. Keep only one active."
      : "No duplicate crash logger binaries detected.",
  });

  checks.push_back(PreflightCheck{
    "BEES_RUNTIME_COMPAT",
    (needsBees && runtimeNeedsBees && !beesModuleLoaded && !beesBinaryPresent) ? "warn" : "ok",
    (needsBees && runtimeNeedsBees && !beesModuleLoaded && !beesBinaryPresent) ? "high" : "low",
    (needsBees && runtimeNeedsBees && !beesModuleLoaded && !beesBinaryPresent)
      ? "헤더 1.71 플러그인이 감지됐지만 BEES가 없어 구버전 런타임에서 CTD 위험이 있습니다."
      : "BEES 관련 런타임 호환성 위험은 감지되지 않았습니다.",
    (needsBees && runtimeNeedsBees && !beesModuleLoaded && !beesBinaryPresent)
      ? "Header 1.71 plugin(s) detected without BEES on older runtime; CTD risk is high."
      : "No BEES-related runtime compatibility risk detected.",
  });

  checks.push_back(PreflightCheck{
    "SYMBOL_POLICY",
    cfg.allowOnlineSymbols ? "warn" : "ok",
    cfg.allowOnlineSymbols ? "medium" : "low",
    cfg.allowOnlineSymbols
      ? "온라인 심볼 소스가 허용되어 있습니다. 개인정보/재현성 정책을 점검하세요."
      : "오프라인 심볼 정책(권장)이 적용되어 있습니다.",
    cfg.allowOnlineSymbols
      ? "Online symbol source is enabled; review privacy/reproducibility policy."
      : "Offline symbol policy (recommended) is active.",
  });

  int warnCount = 0;
  int errorCount = 0;
  for (const auto& c : checks) {
    if (c.status == "warn") {
      ++warnCount;
      AppendLogLine(outBase, L"[Preflight] WARN " + Utf8ToWide(c.id) + L": " + Utf8ToWide(c.messageEn));
    } else if (c.status == "error") {
      ++errorCount;
      AppendLogLine(outBase, L"[Preflight] ERROR " + Utf8ToWide(c.id) + L": " + Utf8ToWide(c.messageEn));
    }
  }

  nlohmann::json out = nlohmann::json::object();
  out["schema"] = { { "name", "SkyrimDiagPreflight" }, { "version", 1 } };
  out["pid"] = proc.pid;
  out["game_exe_dir"] = WideToUtf8(gameExeDir.wstring());
  out["game_exe_version"] = pluginScan.game_exe_version;
  out["plugins_source"] = pluginScan.plugins_source;
  out["mo2_detected"] = pluginScan.mo2_detected;
  out["loaded_module_count"] = moduleNames.size();
  out["checks"] = nlohmann::json::array();
  for (const auto& c : checks) {
    out["checks"].push_back(ToJson(c));
  }
  out["summary"] = {
    { "warn_count", warnCount },
    { "error_count", errorCount },
  };

  const auto preflightPath = outBase / L"SkyrimDiag_Preflight.json";
  WriteTextFileUtf8(preflightPath, out.dump(2));
  AppendLogLine(
    outBase,
    L"Compatibility preflight written: " + preflightPath.wstring() +
      L" (warn=" + std::to_wstring(warnCount) + L", error=" + std::to_wstring(errorCount) + L")");
}

}  // namespace skydiag::helper::internal
