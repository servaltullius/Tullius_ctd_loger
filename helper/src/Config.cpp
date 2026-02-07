#include "SkyrimDiagHelper/Config.h"

#include <Windows.h>

#include <filesystem>

namespace skydiag::helper {
namespace {

std::wstring ExeDir()
{
  wchar_t buf[MAX_PATH]{};
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::filesystem::path p(buf, buf + n);
  return p.parent_path().wstring();
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

}  // namespace

HelperConfig LoadConfig(std::wstring* err)
{
  HelperConfig cfg{};

  const std::wstring path = IniPath();

  cfg.hangThresholdInGameSec =
    static_cast<std::uint32_t>(GetPrivateProfileIntW(L"SkyrimDiagHelper", L"HangThresholdInGameSec", 10, path.c_str()));
  cfg.hangThresholdInMenuSec =
    static_cast<std::uint32_t>(GetPrivateProfileIntW(L"SkyrimDiagHelper", L"HangThresholdInMenuSec", 30, path.c_str()));
  cfg.hangThresholdLoadingSec =
    static_cast<std::uint32_t>(GetPrivateProfileIntW(L"SkyrimDiagHelper", L"HangThresholdLoadingSec", 600, path.c_str()));

  const auto dumpMode =
    static_cast<std::uint32_t>(GetPrivateProfileIntW(L"SkyrimDiagHelper", L"DumpMode", 1, path.c_str()));
  if (dumpMode == 0) {
    cfg.dumpMode = DumpMode::kMini;
  } else if (dumpMode == 2) {
    cfg.dumpMode = DumpMode::kFull;
  } else {
    cfg.dumpMode = DumpMode::kDefault;
  }

  wchar_t outDir[MAX_PATH]{};
  GetPrivateProfileStringW(L"SkyrimDiagHelper", L"OutputDir", L"", outDir, MAX_PATH, path.c_str());
  cfg.outputDir = outDir;

  cfg.enableManualCaptureHotkey =
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"EnableManualCaptureHotkey", 1, path.c_str()) != 0;

  cfg.autoAnalyzeDump = GetPrivateProfileIntW(L"SkyrimDiagHelper", L"AutoAnalyzeDump", 1, path.c_str()) != 0;
  wchar_t dumpToolExe[MAX_PATH]{};
  GetPrivateProfileStringW(
    L"SkyrimDiagHelper",
    L"DumpToolExe",
    L"SkyrimDiagWinUI\\SkyrimDiagDumpToolWinUI.exe",
    dumpToolExe,
    MAX_PATH,
    path.c_str());
  cfg.dumpToolExe = dumpToolExe;
  cfg.autoOpenViewerOnCrash = GetPrivateProfileIntW(L"SkyrimDiagHelper", L"AutoOpenViewerOnCrash", 1, path.c_str()) != 0;
  cfg.autoOpenCrashOnlyIfProcessExited =
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"AutoOpenCrashOnlyIfProcessExited", 1, path.c_str()) != 0;
  cfg.autoOpenCrashWaitForExitMs = static_cast<std::uint32_t>(
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"AutoOpenCrashWaitForExitMs", 2000, path.c_str()));
  cfg.autoOpenViewerOnHang = GetPrivateProfileIntW(L"SkyrimDiagHelper", L"AutoOpenViewerOnHang", 1, path.c_str()) != 0;
  cfg.autoOpenViewerOnManualCapture =
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"AutoOpenViewerOnManualCapture", 0, path.c_str()) != 0;
  cfg.autoOpenHangAfterProcessExit =
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"AutoOpenHangAfterProcessExit", 1, path.c_str()) != 0;
  cfg.autoOpenHangDelayMs = static_cast<std::uint32_t>(
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"AutoOpenHangDelayMs", 2000, path.c_str()));
  cfg.autoOpenViewerBeginnerMode =
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"AutoOpenViewerBeginnerMode", 1, path.c_str()) != 0;

  cfg.enableAdaptiveLoadingThreshold =
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"EnableAdaptiveLoadingThreshold", 1, path.c_str()) != 0;
  cfg.adaptiveLoadingMinSec = static_cast<std::uint32_t>(
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"AdaptiveLoadingMinSec", 120, path.c_str()));
  cfg.adaptiveLoadingMinExtraSec = static_cast<std::uint32_t>(
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"AdaptiveLoadingMinExtraSec", 120, path.c_str()));
  cfg.adaptiveLoadingMaxSec = static_cast<std::uint32_t>(
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"AdaptiveLoadingMaxSec", 1800, path.c_str()));

  cfg.suppressHangWhenNotForeground =
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"SuppressHangWhenNotForeground", 1, path.c_str()) != 0;

  cfg.foregroundGraceSec = static_cast<std::uint32_t>(
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"ForegroundGraceSec", 5, path.c_str()));

  cfg.enableEtwCaptureOnHang =
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"EnableEtwCaptureOnHang", 0, path.c_str()) != 0;
  wchar_t etwWprExe[MAX_PATH]{};
  GetPrivateProfileStringW(L"SkyrimDiagHelper", L"EtwWprExe", L"wpr.exe", etwWprExe, MAX_PATH, path.c_str());
  cfg.etwWprExe = etwWprExe;

  wchar_t etwProfile[128]{};
  GetPrivateProfileStringW(L"SkyrimDiagHelper", L"EtwProfile", L"GeneralProfile", etwProfile, static_cast<DWORD>(std::size(etwProfile)), path.c_str());
  cfg.etwProfile = etwProfile;

  cfg.etwMaxDurationSec = static_cast<std::uint32_t>(
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"EtwMaxDurationSec", 20, path.c_str()));

  cfg.maxCrashDumps = static_cast<std::uint32_t>(
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"MaxCrashDumps", 20, path.c_str()));
  cfg.maxHangDumps = static_cast<std::uint32_t>(
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"MaxHangDumps", 20, path.c_str()));
  cfg.maxManualDumps = static_cast<std::uint32_t>(
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"MaxManualDumps", 20, path.c_str()));
  cfg.maxEtwTraces = static_cast<std::uint32_t>(
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"MaxEtwTraces", 5, path.c_str()));

  cfg.maxHelperLogBytes = static_cast<std::uint32_t>(
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"MaxHelperLogBytes", 8 * 1024 * 1024, path.c_str()));
  cfg.maxHelperLogFiles = static_cast<std::uint32_t>(
    GetPrivateProfileIntW(L"SkyrimDiagHelper", L"MaxHelperLogFiles", 3, path.c_str()));

  if (cfg.outputDir.empty()) {
    cfg.outputDir = ExeDir();
  }

  if (err) {
    err->clear();
  }
  return cfg;
}

}  // namespace skydiag::helper
