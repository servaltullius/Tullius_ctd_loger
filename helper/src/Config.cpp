#include "SkyrimDiagHelper/Config.h"

#include <Windows.h>

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <vector>

namespace skydiag::helper {
namespace {

std::wstring ExeDir()
{
  std::vector<wchar_t> buf(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
  if (n == 0 || n >= buf.size()) {
    return {};
  }
  std::filesystem::path p(buf.data(), buf.data() + n);
  return p.parent_path().wstring();
}

std::wstring ReadIniString(const std::wstring& path, const wchar_t* section, const wchar_t* key, const wchar_t* def)
{
  std::size_t capacity = 256;
  if (def) {
    const std::size_t defLen = std::wcslen(def) + 1;
    if (defLen > capacity) {
      capacity = defLen;
    }
  }

  while (capacity <= 32768) {
    std::vector<wchar_t> buf(capacity, L'\0');
    const DWORD n = GetPrivateProfileStringW(
      section,
      key,
      def,
      buf.data(),
      static_cast<DWORD>(buf.size()),
      path.c_str());
    if (n < (buf.size() - 1)) {
      return std::wstring(buf.data(), n);
    }
    capacity *= 2;
  }

  return def ? std::wstring(def) : std::wstring{};
}

std::wstring IniPath()
{
  auto dir = ExeDir();
  if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') {
    dir.push_back(L'\\');
  }
  dir += L"SkyrimDiagHelper.ini";
  return dir;
}

std::uint32_t ReadIniUint32Clamped(
  const std::wstring& path,
  const wchar_t* section,
  const wchar_t* key,
  int def,
  std::uint32_t minValue,
  std::uint32_t maxValue)
{
  const int raw = GetPrivateProfileIntW(section, key, def, path.c_str());
  const auto clamped = std::clamp<long long>(
    static_cast<long long>(raw),
    static_cast<long long>(minValue),
    static_cast<long long>(maxValue));
  return static_cast<std::uint32_t>(clamped);
}

}  // namespace

HelperConfig LoadConfig(std::wstring* err)
{
  HelperConfig cfg{};

  const std::wstring path = IniPath();

  cfg.hangThresholdInGameSec = ReadIniUint32Clamped(
    path, L"SkyrimDiagHelper", L"HangThresholdInGameSec", 10, 1, 3600);
  cfg.hangThresholdInMenuSec = ReadIniUint32Clamped(
    path, L"SkyrimDiagHelper", L"HangThresholdInMenuSec", 30, 1, 7200);
  cfg.hangThresholdLoadingSec = ReadIniUint32Clamped(
    path, L"SkyrimDiagHelper", L"HangThresholdLoadingSec", 600, 1, 7200);

  const auto dumpMode = ReadIniUint32Clamped(
    path, L"SkyrimDiagHelper", L"DumpMode", 1, 0, 2);
  if (dumpMode == 0) {
    cfg.dumpMode = DumpMode::kMini;
  } else if (dumpMode == 2) {
    cfg.dumpMode = DumpMode::kFull;
  } else {
    cfg.dumpMode = DumpMode::kDefault;
  }

  cfg.outputDir = ReadIniString(path, L"SkyrimDiagHelper", L"OutputDir", L"");

  cfg.enableManualCaptureHotkey =
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"EnableManualCaptureHotkey", 1, path.c_str()) != 0;
  cfg.enableCompatibilityPreflight =
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"EnableCompatibilityPreflight", 1, path.c_str()) != 0;

  cfg.autoAnalyzeDump = GetPrivateProfileIntW(L"SkyrimDiagHelper", L"AutoAnalyzeDump", 1, path.c_str()) != 0;
  cfg.allowOnlineSymbols = GetPrivateProfileIntW(L"SkyrimDiagHelper", L"AllowOnlineSymbols", 0, path.c_str()) != 0;
  cfg.enableWerDumpFallbackHint =
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"EnableWerDumpFallbackHint", 1, path.c_str()) != 0;
  cfg.preserveFilteredCrashDumps =
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"PreserveFilteredCrashDumps", 0, path.c_str()) != 0;
  cfg.dumpToolExe = ReadIniString(
    path,
    L"SkyrimDiagHelper",
    L"DumpToolExe",
    L"SkyrimDiagWinUI\\SkyrimDiagDumpToolWinUI.exe");
  cfg.autoOpenViewerOnCrash = GetPrivateProfileIntW(L"SkyrimDiagHelper", L"AutoOpenViewerOnCrash", 1, path.c_str()) != 0;
  cfg.autoOpenCrashOnlyIfProcessExited =
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"AutoOpenCrashOnlyIfProcessExited", 1, path.c_str()) != 0;
  cfg.autoOpenCrashWaitForExitMs = ReadIniUint32Clamped(
    path, L"SkyrimDiagHelper", L"AutoOpenCrashWaitForExitMs", 2000, 0, 60000);
  cfg.enableAutoRecaptureOnUnknownCrash =
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"EnableAutoRecaptureOnUnknownCrash", 0, path.c_str()) != 0;
  cfg.autoRecaptureUnknownBucketThreshold = ReadIniUint32Clamped(
    path, L"SkyrimDiagHelper", L"AutoRecaptureUnknownBucketThreshold", 2, 1, 10);
  cfg.autoRecaptureAnalysisTimeoutSec = ReadIniUint32Clamped(
    path, L"SkyrimDiagHelper", L"AutoRecaptureAnalysisTimeoutSec", 20, 1, 300);
  cfg.autoOpenViewerOnHang = GetPrivateProfileIntW(L"SkyrimDiagHelper", L"AutoOpenViewerOnHang", 1, path.c_str()) != 0;
  cfg.autoOpenViewerOnManualCapture =
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"AutoOpenViewerOnManualCapture", 0, path.c_str()) != 0;
  cfg.autoOpenHangAfterProcessExit =
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"AutoOpenHangAfterProcessExit", 1, path.c_str()) != 0;
  cfg.autoOpenHangDelayMs = ReadIniUint32Clamped(
    path, L"SkyrimDiagHelper", L"AutoOpenHangDelayMs", 2000, 0, 60000);
  cfg.autoOpenViewerBeginnerMode =
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"AutoOpenViewerBeginnerMode", 1, path.c_str()) != 0;

  cfg.enableAdaptiveLoadingThreshold =
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"EnableAdaptiveLoadingThreshold", 1, path.c_str()) != 0;
  cfg.adaptiveLoadingMinSec = ReadIniUint32Clamped(
    path, L"SkyrimDiagHelper", L"AdaptiveLoadingMinSec", 120, 30, 3600);
  cfg.adaptiveLoadingMinExtraSec = ReadIniUint32Clamped(
    path, L"SkyrimDiagHelper", L"AdaptiveLoadingMinExtraSec", 120, 0, 1800);
  cfg.adaptiveLoadingMaxSec = ReadIniUint32Clamped(
    path, L"SkyrimDiagHelper", L"AdaptiveLoadingMaxSec", 1800, 60, 7200);
  if (cfg.adaptiveLoadingMaxSec < cfg.adaptiveLoadingMinSec) {
    cfg.adaptiveLoadingMaxSec = cfg.adaptiveLoadingMinSec;
  }

  cfg.suppressHangWhenNotForeground =
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"SuppressHangWhenNotForeground", 1, path.c_str()) != 0;

  cfg.foregroundGraceSec = ReadIniUint32Clamped(
    path, L"SkyrimDiagHelper", L"ForegroundGraceSec", 5, 0, 60);

  cfg.enableEtwCaptureOnHang =
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"EnableEtwCaptureOnHang", 0, path.c_str()) != 0;
  cfg.etwWprExe = ReadIniString(path, L"SkyrimDiagHelper", L"EtwWprExe", L"wpr.exe");

  cfg.etwHangProfile = ReadIniString(path, L"SkyrimDiagHelper", L"EtwHangProfile", L"");
  if (cfg.etwHangProfile.empty()) {
    // Backward compatibility for older ini files.
    cfg.etwHangProfile = ReadIniString(path, L"SkyrimDiagHelper", L"EtwProfile", L"GeneralProfile");
  }
  cfg.etwHangFallbackProfile = ReadIniString(path, L"SkyrimDiagHelper", L"EtwHangFallbackProfile", L"");

  cfg.etwMaxDurationSec = ReadIniUint32Clamped(
    path, L"SkyrimDiagHelper", L"EtwMaxDurationSec", 20, 1, 300);

  cfg.enableEtwCaptureOnCrash =
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"EnableEtwCaptureOnCrash", 0, path.c_str()) != 0;

  cfg.etwCrashProfile = ReadIniString(path, L"SkyrimDiagHelper", L"EtwCrashProfile", L"GeneralProfile");
  if (cfg.etwCrashProfile.empty()) {
    // Treat empty as default for safer behavior.
    cfg.etwCrashProfile = L"GeneralProfile";
  }

  const std::uint32_t etwCrashCaptureSeconds = ReadIniUint32Clamped(
    path, L"SkyrimDiagHelper", L"EtwCrashCaptureSeconds", 8, 1, 30);
  cfg.etwCrashCaptureSeconds = etwCrashCaptureSeconds;

  cfg.enableIncidentManifest =
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"EnableIncidentManifest", 1, path.c_str()) != 0;
  cfg.incidentManifestIncludeConfigSnapshot =
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"IncidentManifestIncludeConfigSnapshot", 1, path.c_str()) != 0;
  cfg.suppressDuringGrassCaching =
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"SuppressDuringGrassCaching", 1, path.c_str()) != 0;

  cfg.maxCrashDumps = ReadIniUint32Clamped(
    path, L"SkyrimDiagHelper", L"MaxCrashDumps", 20, 0, 500);
  cfg.maxHangDumps = ReadIniUint32Clamped(
    path, L"SkyrimDiagHelper", L"MaxHangDumps", 20, 0, 500);
  cfg.maxManualDumps = ReadIniUint32Clamped(
    path, L"SkyrimDiagHelper", L"MaxManualDumps", 20, 0, 500);
  cfg.maxEtwTraces = ReadIniUint32Clamped(
    path, L"SkyrimDiagHelper", L"MaxEtwTraces", 5, 0, 100);

  cfg.maxHelperLogBytes = ReadIniUint32Clamped(
    path, L"SkyrimDiagHelper", L"MaxHelperLogBytes", 8 * 1024 * 1024, 0, 100 * 1024 * 1024);
  cfg.maxHelperLogFiles = ReadIniUint32Clamped(
    path, L"SkyrimDiagHelper", L"MaxHelperLogFiles", 3, 0, 20);

  if (cfg.outputDir.empty()) {
    cfg.outputDir = ExeDir();
  }

  if (err) {
    err->clear();
  }
  return cfg;
}

}  // namespace skydiag::helper
