#pragma once

#include <cstdint>
#include <string>

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
  std::wstring outputDir;  // empty => next to exe
  bool enableManualCaptureHotkey = true;
  bool enableCompatibilityPreflight = true;
  bool autoAnalyzeDump = true;
  std::wstring dumpToolExe = L"SkyrimDiagWinUI\\SkyrimDiagDumpToolWinUI.exe";
  bool allowOnlineSymbols = false;
  bool enableWerDumpFallbackHint = true;
  bool preserveFilteredCrashDumps = false;  // If true, never delete crash dumps even if false-positive filter triggers
  bool autoOpenViewerOnCrash = true;
  bool autoOpenCrashOnlyIfProcessExited = true;
  std::uint32_t autoOpenCrashWaitForExitMs = 2000;
  bool enableAutoRecaptureOnUnknownCrash = false;
  std::uint32_t autoRecaptureUnknownBucketThreshold = 2;
  std::uint32_t autoRecaptureAnalysisTimeoutSec = 20;
  bool autoOpenViewerOnHang = true;
  bool autoOpenViewerOnManualCapture = false;
  bool autoOpenHangAfterProcessExit = true;
  std::uint32_t autoOpenHangDelayMs = 2000;
  bool autoOpenViewerBeginnerMode = true;
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

  // Retention / disk cleanup (0 = unlimited)
  std::uint32_t maxCrashDumps = 20;
  std::uint32_t maxHangDumps = 20;
  std::uint32_t maxManualDumps = 20;
  std::uint32_t maxEtwTraces = 5;

  // Helper log rotation (0 = unlimited/disabled)
  std::uint32_t maxHelperLogBytes = 8u * 1024u * 1024u;
  std::uint32_t maxHelperLogFiles = 3;
};

HelperConfig LoadConfig(std::wstring* err);

}  // namespace skydiag::helper
