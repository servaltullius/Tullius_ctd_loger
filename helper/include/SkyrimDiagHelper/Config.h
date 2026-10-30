#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "SkyrimDiagHelper/ManualCaptureHotkey.h"
#include "SkyrimDiagHelper/Retention.h"

namespace skydiag::helper {

enum class DumpMode : std::uint32_t {
  kMini = 0,
  kDefault = 1,
  kFull = 2,
};

struct HelperConfig {
  std::uint32_t hangThresholdInGameSec = 10;
  std::uint32_t hangThresholdInMenuSec = 30;
  std::uint32_t hangThresholdLoadingSec = 600;
  DumpMode dumpMode = DumpMode::kDefault;
  // ADR-0023: DumpMode=1 dumps write only the game executable's code whole,
  // and of other modules the code the stacks point at. 1 writes every
  // module's code (and module headers in crash dumps), as through v0.2.71.
  bool dumpAllModuleCode = false;
  std::wstring outputDir;  // empty => next to exe
  // Tried in order when outputDir cannot be created or written (see
  // MakeOutputBase): without them a bad OutputDir lost every capture and log.
  std::vector<std::wstring> outputDirFallbacks;
  bool enableManualCaptureHotkey = true;
  ManualCaptureHotkey manualCaptureHotkey = kDefaultManualCaptureHotkey;
  bool enableCompatibilityPreflight = true;
  bool autoAnalyzeDump = true;
  std::wstring dumpToolExe = L"SkyrimDiagWinUI\\SkyrimDiagDumpToolWinUI.exe";
  bool allowOnlineSymbols = false;
  bool enableWerDumpFallbackHint = true;
  bool preserveFilteredCrashDumps = false;  // If true, never delete crash dumps even if false-positive filter triggers
  // A clean (exit code 0) shutdown normally means a handled exception, so the dump is
  // discarded. When a strong fault was already published and no heartbeat recovery was
  // observed, keep a small metadata record so a genuinely missed CTD stays investigable.
  bool enableCleanExitEvidenceQuarantine = true;
  bool autoOpenViewerOnCrash = true;
  bool autoOpenCrashOnlyIfProcessExited = true;
  std::uint32_t autoOpenCrashWaitForExitMs = 2000;
  bool enableAutoRecaptureOnUnknownCrash = false;
  std::uint32_t autoRecaptureUnknownBucketThreshold = 2;
  std::uint32_t autoRecaptureAnalysisTimeoutSec = 20;
  bool autoOpenViewerOnHang = true;
  bool autoOpenViewerOnManualCapture = false;
  // The WinUI viewer cannot start under Wine/Proton; auto-open stays off there
  // unless this is set.
  bool autoOpenViewerUnderWine = false;
  bool autoOpenHangAfterProcessExit = true;
  std::uint32_t autoOpenHangDelayMs = 2000;
  bool autoOpenViewerBeginnerMode = true;
  bool enablePssSnapshotForFreeze = false;
  bool enableAdaptiveLoadingThreshold = true;
  std::uint32_t adaptiveLoadingMinSec = 120;
  std::uint32_t adaptiveLoadingMinExtraSec = 120;
  std::uint32_t adaptiveLoadingMaxSec = 1800;
  bool suppressHangWhenNotForeground = true;
  std::uint32_t foregroundGraceSec = 5;
  bool enableEtwCaptureOnHang = false;
  std::wstring etwWprExe = L"wpr.exe";
  std::wstring etwHangProfile = L"GeneralProfile";
  std::wstring etwHangFallbackProfile;
  std::uint32_t etwMaxDurationSec = 20;
  bool enableEtwCaptureOnCrash = false;
  std::wstring etwCrashProfile = L"GeneralProfile";
  std::uint32_t etwCrashCaptureSeconds = 8;
  bool enableIncidentManifest = true;
  bool incidentManifestIncludeConfigSnapshot = true;
  bool suppressDuringGrassCaching = true;

  // Retention / disk cleanup (0 = unlimited)
  std::uint32_t maxCrashDumps = 20;
  std::uint32_t maxHangDumps = 20;
  std::uint32_t maxManualDumps = 20;
  std::uint32_t maxEtwTraces = 5;
  // Crash, freeze and manual dumps together, in GiB.
  std::uint32_t maxTotalDumpGB = 5;

  // Helper log rotation (0 = unlimited/disabled)
  std::uint32_t maxHelperLogBytes = 8u * 1024u * 1024u;
  std::uint32_t maxHelperLogFiles = 3;
  // Earlier game sessions whose helper log is kept (previous.log, previous.2.log, ...).
  std::uint32_t maxHelperSessionLogs = 5;
};

// OutputDir as SkyrimDiagHelper.ini documents it: blank means the default
// subfolder of the helper exe folder, and a relative value is relative to
// that folder. Resolved to an absolute path once here: the dump tool is
// started with the output folder as its working directory, so a relative
// path would be looked up inside itself (MyLogs\MyLogs\x.dmp).
inline std::filesystem::path ResolveOutputDirAgainstExeDir(
  const std::wstring& raw,
  const std::filesystem::path& exeDir,
  std::wstring_view defaultSubdir)
{
  std::filesystem::path out = raw.empty() ? exeDir / defaultSubdir : std::filesystem::path(raw);
  if (out.is_relative()) {
    out = exeDir / out;
  }
  out = out.lexically_normal();
  // "D:\\Logs\\" would reach the dump tool as --out-dir "D:\\Logs\\", whose
  // trailing backslash escapes the closing quote on the command line.
  if (!out.has_filename() && out.has_relative_path()) {
    out = out.parent_path();
  }
  return out;
}

HelperConfig LoadConfig(std::wstring* err);

// The retention settings of a config, as every retention sweep applies them.
inline RetentionLimits RetentionLimitsFromConfig(const HelperConfig& cfg)
{
  RetentionLimits limits{};
  limits.maxCrashDumps = cfg.maxCrashDumps;
  limits.maxHangDumps = cfg.maxHangDumps;
  limits.maxManualDumps = cfg.maxManualDumps;
  limits.maxEtwTraces = cfg.maxEtwTraces;
  limits.maxTotalDumpGB = cfg.maxTotalDumpGB;
  return limits;
}

}  // namespace skydiag::helper
