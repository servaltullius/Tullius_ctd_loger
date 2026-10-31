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

// A press within kManualCaptureDebounceMs of the end of the last manual
// capture starts no new one (a last tick of 0 means none yet).
constexpr bool ShouldDebounceManualCapture(ULONGLONG nowTick64, ULONGLONG lastCaptureTick64) noexcept
{
  return lastCaptureTick64 != 0 && nowTick64 >= lastCaptureTick64 &&
         nowTick64 - lastCaptureTick64 < kManualCaptureDebounceMs;
}

// RegisterHotKey's modifiers for the manual capture hotkey: MOD_NOREPEAT, so
// holding the chord down on a frozen game does not deliver WM_HOTKEY again
// for each key repeat (each one a dump). Not part of the configured chord.
constexpr UINT ManualCaptureHotkeyRegistrationModifiers(const ManualCaptureHotkey& hotkey) noexcept
{
  return static_cast<UINT>(hotkey.modifiers) | MOD_NOREPEAT;
}

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
  // The still-frozen crash generation InitializeLoopState signaled again
  // (RearmFrozenCrashFromEarlierHelper), 0 when none.
  std::uint32_t rearmedFrozenCrashSeq = 0;
  // ADR-0024: when the hook stub record is read next, and the page count last
  // logged (none yet: SIZE_MAX).
  std::uint64_t nextHookPageScanTick64 = 0;
  std::size_t loggedHookPageCount = static_cast<std::size_t>(-1);
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
void InitializeLoopState(const AttachedProcess& proc, const std::filesystem::path& outBase, HelperLoopState* state);
// The marker files RearmFrozenCrashFromEarlierHelper leaves in the output
// folder: <prefix><pid>_<game creation time>_<crash_seq>.marker.
inline constexpr std::wstring_view kFrozenCrashRearmMarkerPrefix = L"SkyrimDiag_CrashRearm_";
// The marker name up to the crash_seq for this game process.
std::wstring FrozenCrashRearmMarkerPrefix(const AttachedProcess& proc);
// A committed crash generation that is still Frozen while the game runs was
// published and claimed, but the helper handling it is gone (it was killed or
// crashed, and the plugin's watchdog started this one). Nothing would signal
// it again, and while it stays Frozen the plugin records no later crash, so
// its event is signaled again for this helper to handle it from the start; a
// second dump of it is the worst case. That happens once per generation (a
// marker file in outBase): found still Frozen after that, it is thawed
// instead. Returns that generation's crash_seq, or 0 when nothing was
// signaled.
std::uint32_t RearmFrozenCrashFromEarlierHelper(const AttachedProcess& proc, const std::filesystem::path& outBase);
// Deletes this game process's rearm markers (once it has exited).
void RemoveFrozenCrashRearmMarkers(const AttachedProcess& proc, const std::filesystem::path& outBase);
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

// Deletes the dump held for a crash released on a brief heartbeat once the
// game has outlived it (as late recovery: a minute alive, 30 s of advancing
// heartbeat); after that the exit path no longer reports it at the usual
// abnormal exit.
void ExpireReleasedCrashDumpIfOutlived(
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
