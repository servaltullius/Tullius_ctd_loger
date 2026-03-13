#include "CompatibilityPreflight.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "HelperCommon.h"
#include "HelperLog.h"
#include "SkyrimDiagHelper/PluginScanner.h"
#include "SkyrimDiagStringUtil.h"

namespace skydiag::helper::internal {
namespace {

using skydiag::WideLower;

struct PreflightCheck
{
  std::string id;
  std::string status;  // ok|warn|error
  std::string severity;
  std::string messageKo;
  std::string messageEn;
};

struct SymbolRuntimeHealth
{
  std::filesystem::path dbghelpPath;
  std::string dbghelpVersion;
  std::filesystem::path msdiaPath;
  bool msdiaAvailable = false;
  std::wstring symbolSearchPath;
  std::filesystem::path symbolCachePath;
  bool symbolCacheReady = false;
  bool symbolPathHealthy = false;
  bool searchPathUsesRemote = false;
  bool policyMismatch = false;
};

std::string QueryFileVersionString(const std::filesystem::path& filePath);

std::string AsciiLower(std::string_view s)
{
  std::string out;
  out.reserve(s.size());
  std::transform(s.begin(), s.end(), std::back_inserter(out), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
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

std::wstring ReadEnvVar(const wchar_t* name)
{
  if (!name || !*name) {
    return {};
  }
  const DWORD need = GetEnvironmentVariableW(name, nullptr, 0);
  if (need == 0) {
    return {};
  }
  std::wstring out(static_cast<std::size_t>(need - 1), L'\0');
  if (!out.empty()) {
    GetEnvironmentVariableW(name, out.data(), need);
  }
  return out;
}

bool WideContainsAsciiInsensitive(std::wstring_view haystack, std::string_view needleAscii)
{
  if (haystack.empty() || needleAscii.empty()) {
    return false;
  }
  std::wstring needle;
  needle.reserve(needleAscii.size());
  for (char ch : needleAscii) {
    needle.push_back(static_cast<wchar_t>(std::towlower(static_cast<wchar_t>(static_cast<unsigned char>(ch)))));
  }
  const std::wstring lowered = WideLower(std::wstring(haystack));
  return lowered.find(needle) != std::wstring::npos;
}

std::filesystem::path QueryLoadedModulePath(const wchar_t* moduleName)
{
  if (!moduleName || !*moduleName) {
    return {};
  }
  HMODULE module = GetModuleHandleW(moduleName);
  if (!module) {
    return {};
  }
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD copied = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (copied == 0) {
      return {};
    }
    if (copied < buffer.size() - 1) {
      buffer.resize(copied);
      return std::filesystem::path(buffer);
    }
    buffer.resize(buffer.size() * 2);
  }
}

std::filesystem::path SearchDllPath(const wchar_t* moduleName)
{
  if (!moduleName || !*moduleName) {
    return {};
  }
  const DWORD needed = SearchPathW(nullptr, moduleName, nullptr, 0, nullptr, nullptr);
  if (needed == 0) {
    return {};
  }
  std::wstring buffer(static_cast<std::size_t>(needed), L'\0');
  if (SearchPathW(nullptr, moduleName, nullptr, needed, buffer.data(), nullptr) == 0) {
    return {};
  }
  buffer.resize(std::wcslen(buffer.c_str()));
  return std::filesystem::path(buffer);
}

std::filesystem::path ResolveRuntimeDllPath(const wchar_t* moduleName)
{
  if (const auto loaded = QueryLoadedModulePath(moduleName); !loaded.empty()) {
    return loaded;
  }
  return SearchDllPath(moduleName);
}

std::wstring ResolveDefaultSymbolCacheDir(bool* outReady)
{
  if (outReady) {
    *outReady = false;
  }

  std::filesystem::path cacheDir;
  if (const std::wstring fromEnv = ReadEnvVar(L"SKYRIMDIAG_SYMBOL_CACHE_DIR"); !fromEnv.empty()) {
    cacheDir = fromEnv;
  } else if (const std::wstring localAppData = ReadEnvVar(L"LOCALAPPDATA"); !localAppData.empty()) {
    cacheDir = std::filesystem::path(localAppData) / L"SkyrimDiag" / L"SymbolCache";
  } else {
    wchar_t tmp[MAX_PATH]{};
    const DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n > 0 && n < MAX_PATH) {
      cacheDir = std::filesystem::path(tmp) / L"SkyrimDiagSymbolCache";
    }
  }

  if (cacheDir.empty()) {
    return {};
  }

  std::error_code ec;
  std::filesystem::create_directories(cacheDir, ec);
  const bool ready = !ec && std::filesystem::exists(cacheDir);
  if (outReady) {
    *outReady = ready;
  }
  return cacheDir.wstring();
}

std::wstring ResolveSymbolSearchPath(bool allowOnlineSymbols, std::wstring* outCachePath, bool* outCacheReady)
{
  if (outCachePath) {
    outCachePath->clear();
  }
  if (outCacheReady) {
    *outCacheReady = false;
  }

  if (const std::wstring explicitPath = ReadEnvVar(L"SKYRIMDIAG_SYMBOL_PATH"); !explicitPath.empty()) {
    return explicitPath;
  }

  if (const std::wstring ntSymbolPath = ReadEnvVar(L"_NT_SYMBOL_PATH"); !ntSymbolPath.empty()) {
    return ntSymbolPath;
  }

  const std::wstring cachePath = ResolveDefaultSymbolCacheDir(outCacheReady);
  if (outCachePath) {
    *outCachePath = cachePath;
  }
  if (cachePath.empty()) {
    return {};
  }
  if (!allowOnlineSymbols) {
    return cachePath;
  }
  return L"srv*" + cachePath + L"*https://msdl.microsoft.com/download/symbols";
}

SymbolRuntimeHealth CollectSymbolRuntimeHealth(const skydiag::helper::HelperConfig& cfg)
{
  SymbolRuntimeHealth health;
  health.dbghelpPath = ResolveRuntimeDllPath(L"dbghelp.dll");
  if (!health.dbghelpPath.empty()) {
    health.dbghelpVersion = QueryFileVersionString(health.dbghelpPath);
  }

  health.msdiaPath = ResolveRuntimeDllPath(L"msdia140.dll");
  health.msdiaAvailable = !health.msdiaPath.empty();

  std::wstring cachePath;
  health.symbolSearchPath = ResolveSymbolSearchPath(cfg.allowOnlineSymbols, &cachePath, &health.symbolCacheReady);
  if (!cachePath.empty()) {
    health.symbolCachePath = cachePath;
  }
  health.symbolPathHealthy = !health.symbolSearchPath.empty();
  health.searchPathUsesRemote = WideContainsAsciiInsensitive(health.symbolSearchPath, "https://") ||
                                WideContainsAsciiInsensitive(health.symbolSearchPath, "http://");
  health.policyMismatch = !cfg.allowOnlineSymbols && health.searchPathUsesRemote;
  return health;
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
  const SymbolRuntimeHealth symbolHealth = CollectSymbolRuntimeHealth(cfg);

  std::vector<PreflightCheck> checks;
  checks.reserve(10);

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

  checks.push_back(PreflightCheck{
    "DBGHELP_RUNTIME",
    (!symbolHealth.dbghelpPath.empty() && !symbolHealth.dbghelpVersion.empty()) ? "ok" : "warn",
    (!symbolHealth.dbghelpPath.empty() && !symbolHealth.dbghelpVersion.empty()) ? "low" : "high",
    (!symbolHealth.dbghelpPath.empty() && !symbolHealth.dbghelpVersion.empty())
      ? ("dbghelp.dll 런타임 확인: " + WideToUtf8(symbolHealth.dbghelpPath.wstring()))
      : "dbghelp.dll 런타임을 확인하지 못했습니다. 스택워크 품질이 저하될 수 있습니다.",
    (!symbolHealth.dbghelpPath.empty() && !symbolHealth.dbghelpVersion.empty())
      ? ("dbghelp.dll runtime detected: " + WideToUtf8(symbolHealth.dbghelpPath.wstring()))
      : "Failed to resolve dbghelp.dll runtime; stackwalk quality may be degraded.",
  });

  checks.push_back(PreflightCheck{
    "MSDIA_RUNTIME",
    symbolHealth.msdiaAvailable ? "ok" : "warn",
    symbolHealth.msdiaAvailable ? "low" : "medium",
    symbolHealth.msdiaAvailable
      ? ("msdia140.dll 사용 가능: " + WideToUtf8(symbolHealth.msdiaPath.wstring()))
      : "msdia140.dll을 찾지 못했습니다. 소스 라인 해석이 제한될 수 있습니다.",
    symbolHealth.msdiaAvailable
      ? ("msdia140.dll available: " + WideToUtf8(symbolHealth.msdiaPath.wstring()))
      : "msdia140.dll not found; source line resolution may be limited.",
  });

  checks.push_back(PreflightCheck{
    "SYMBOL_PATH_HEALTH",
    (symbolHealth.symbolPathHealthy && !symbolHealth.policyMismatch) ? "ok" : "warn",
    (symbolHealth.symbolPathHealthy && !symbolHealth.policyMismatch) ? "low" : "medium",
    (symbolHealth.symbolPathHealthy && !symbolHealth.policyMismatch)
      ? "심볼 검색 경로가 설정되어 있습니다."
      : (symbolHealth.policyMismatch
          ? "온라인 심볼 비허용 정책인데 검색 경로에 원격 심볼 서버가 포함되어 있습니다."
          : "심볼 검색 경로가 비어 있어 분석 품질이 저하될 수 있습니다."),
    (symbolHealth.symbolPathHealthy && !symbolHealth.policyMismatch)
      ? "Symbol search path is configured."
      : (symbolHealth.policyMismatch
          ? "Symbol path includes a remote source while online symbol policy is disabled."
          : "Symbol search path is empty; analysis quality may be limited."),
  });

  checks.push_back(PreflightCheck{
    "SYMBOL_CACHE_HEALTH",
    (symbolHealth.symbolCachePath.empty() || symbolHealth.symbolCacheReady) ? "ok" : "warn",
    (symbolHealth.symbolCachePath.empty() || symbolHealth.symbolCacheReady) ? "low" : "medium",
    (symbolHealth.symbolCachePath.empty() || symbolHealth.symbolCacheReady)
      ? "심볼 캐시 경로가 준비되어 있습니다."
      : "심볼 캐시 경로를 만들지 못했습니다. 로컬 캐시 기반 심볼화가 제한될 수 있습니다.",
    (symbolHealth.symbolCachePath.empty() || symbolHealth.symbolCacheReady)
      ? "Symbol cache path is ready."
      : "Failed to prepare symbol cache directory; local-cache symbolization may be limited.",
  });

  // Non-ESL (full) plugin slot limit check.
  {
    std::size_t fullPluginCount = 0;
    for (const auto& p : pluginScan.plugins) {
      if (p.is_active && !p.is_esl) {
        ++fullPluginCount;
      }
    }
    const bool nearLimit = (fullPluginCount >= 240);
    checks.push_back(PreflightCheck{
      "FULL_PLUGIN_SLOT_LIMIT",
      nearLimit ? "warn" : "ok",
      nearLimit ? "high" : "low",
      nearLimit
        ? ("비-ESL 플러그인 " + std::to_string(fullPluginCount) + "개 — 254 슬롯 한계에 근접합니다.")
        : ("비-ESL 플러그인 " + std::to_string(fullPluginCount) + "개 — 슬롯 여유 있음."),
      nearLimit
        ? ("Non-ESL plugin count " + std::to_string(fullPluginCount) + " — approaching 254 slot limit.")
        : ("Non-ESL plugin count " + std::to_string(fullPluginCount) + " — within safe range."),
    });
  }

  // Known incompatible mod combinations.
  {
    const bool hasMultiplePhysics =
      HasModuleAny(moduleNames, { L"hdtssephysics.dll" }) &&
      HasModuleAny(moduleNames, { L"hdtsmp64.dll" });
    const bool hasIncompatCombo = hasMultiplePhysics;
    std::string detailKo = "알려진 비호환 모드 조합이 감지되지 않았습니다.";
    std::string detailEn = "No known incompatible mod combinations detected.";
    if (hasMultiplePhysics) {
      detailKo = "HDT-SMP와 HDT-SMP (Faster) 물리 모드가 동시 로드 — 충돌 가능성이 높습니다.";
      detailEn = "HDT-SMP and HDT-SMP (Faster) physics mods loaded simultaneously — likely conflict.";
    }
    checks.push_back(PreflightCheck{
      "KNOWN_INCOMPATIBLE_COMBO",
      hasIncompatCombo ? "warn" : "ok",
      hasIncompatCombo ? "high" : "low",
      detailKo,
      detailEn,
    });
  }

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
  out["symbol_runtime"] = {
    { "dbghelp_path", WideToUtf8(symbolHealth.dbghelpPath.wstring()) },
    { "dbghelp_version", symbolHealth.dbghelpVersion },
    { "msdia_path", WideToUtf8(symbolHealth.msdiaPath.wstring()) },
    { "msdia_available", symbolHealth.msdiaAvailable },
    { "search_path", WideToUtf8(symbolHealth.symbolSearchPath) },
    { "cache_path", WideToUtf8(symbolHealth.symbolCachePath.wstring()) },
    { "cache_ready", symbolHealth.symbolCacheReady },
    { "online_symbol_source_allowed", cfg.allowOnlineSymbols },
    { "search_path_uses_remote", symbolHealth.searchPathUsesRemote },
    { "policy_mismatch", symbolHealth.policyMismatch },
  };
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
