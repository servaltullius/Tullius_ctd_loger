#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/LoadStats.h"
#include "SkyrimDiagHelper/ProcessAttach.h"

#include "CrashEtwCapture.h"
#include "CrashCapture.h"
#include "HangCapture.h"
#include "PendingCrashAnalysis.h"

namespace skydiag::helper::internal {

inline constexpr int kHotkeyId = 0x5344;  // 'SD' (arbitrary)
inline constexpr ULONGLONG kManualCaptureDebounceMs = 250;

struct HelperLoopState
{
  CrashCaptureState crashCaptured{};
  HangCaptureState hangState{};
  std::wstring pendingHangViewerDumpPath;
  std::wstring pendingCrashViewerDumpPath;
  std::wstring capturedCrashDumpPath;
  PendingCrashAnalysis pendingCrashAnalysis{};
  PendingCrashEtwCapture pendingCrashEtw{};
  std::uint64_t nextCrashEventRetryTick64 = 0;
  std::uint64_t nextCrashEventWarnTick64 = 0;
  std::uint32_t postExitEvidenceSeq = 0;
  // A crash viewer was opened for the dump when the game exited; it runs its
  // own analysis of that dump.
  bool crashViewerLaunchedAtExit = false;
  // The full recapture of the captured crash, cleaned up with it.
  CrashRecaptureArtifacts crashRecapture{};
  // Where Crash Logger writes its logs (ResolveCrashLoggerLogDirs), for the
  // exit path's look for its crash log.
  std::vector<std::filesystem::path> crashLoggerLogDirs;
};

struct CrashArtifactRemovalResult
{
  std::uint32_t removedCount = 0;
  bool dumpExistedBefore = false;
  bool dumpExistsAfter = false;
  std::error_code dumpError;
};

HANDLE AcquireHelperSingletonMutex(std::uint32_t pid, std::wstring* err);
std::wstring BuildCrashEventUnavailableMessage(const AttachedProcess& proc, std::wstring_view prefix);
CrashArtifactRemovalResult RemoveCrashArtifactsForDump(
  const std::filesystem::path& outBase,
  std::wstring_view dumpPath,
  const std::filesystem::path& extraArtifactPath = {},
  bool preserveDumpFile = false);
void InitializeLoopState(const AttachedProcess& proc, HelperLoopState* state);
// CrashLogger.ini's Crashlog Directory value (its raw bytes) as a path: UTF-8
// as Crash Logger reads it, else the ANSI code page. Empty when it decodes in
// neither. Never throws on bad bytes, unlike a path built from a u8string.
std::filesystem::path DecodeCrashLoggerIniPath(const std::string& bytes);
// The folders Crash Logger writes its crash-*.log to: the Crashlog Directory
// set in CrashLogger.ini, and the SKSE log folder under Documents\My Games.
// Empty unless Crash Logger is installed with its thread dump hotkey on.
std::vector<std::filesystem::path> ResolveCrashLoggerLogDirs(const HelperConfig& cfg, const AttachedProcess& proc);
// Also starts watching Crash Logger's thread dump hotkey when Crash Logger is
// installed with it on, whether or not the manual capture hotkey is enabled;
// the unregister call stops both.
void RegisterManualCaptureHotkeyIfEnabled(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase);
void UnregisterManualCaptureHotkeyIfEnabled(const HelperConfig& cfg);
void PumpManualCaptureInputs(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase,
  LoadStats* loadStats,
  std::uint32_t adaptiveLoadingThresholdSec);
// After the process exited: true when a committed crash that the helper did
// not release as handled is on record (captured, or recorded here as
// metadata). A crash released earlier is not reported again.
bool DrainCrashEventBeforeExit(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase,
  DWORD exitCode,
  HelperLoopState* state);
void CleanupCrashArtifactsAfterZeroExit(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase,
  HelperLoopState* state);
void AppendExitClassificationLog(const std::filesystem::path& outBase, bool hadCapturedCrash);
void LaunchDeferredViewersAfterExit(
  const HelperConfig& cfg,
  const std::filesystem::path& outBase,
  DWORD exitCode,
  HelperLoopState* state);
void HandleProcessWaitFailed(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase,
  DWORD waitError,
  HelperLoopState* state);
// At helper shutdown: leave a running crash analysis to finish its report,
// unless the viewer opened at exit analyzes the same dump.
void ReleasePendingCrashAnalysisAtShutdown(const std::filesystem::path& outBase, HelperLoopState* state);

// A crash kept while the game then keeps running is released as handled
// (see AdvanceLateRecovery). Returns true when it released one this tick.
bool HandleLateRecoveryTick(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase,
  HelperLoopState* state);

bool HandleProcessExitTick(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase,
  HelperLoopState* state);
void RunHelperLoop(
  const HelperConfig& cfg,
  AttachedProcess& proc,
  const std::filesystem::path& outBase,
  LoadStats* loadStats,
  const std::filesystem::path& loadStatsPath,
  std::uint32_t* adaptiveLoadingThresholdSec,
  std::uint64_t attachNowQpc,
  HelperLoopState* state);
void ShutdownLoopState(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase,
  HelperLoopState* state);
bool DetectGrassCacheMode(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase);
void RunGrassCacheLoop(const AttachedProcess& proc, const std::filesystem::path& outBase);

}  // namespace skydiag::helper::internal
