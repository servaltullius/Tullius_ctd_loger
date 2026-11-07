#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "SkyrimDiagHelper/HangSuppression.h"
#include "SkyrimDiagShared.h"

namespace skydiag::helper {
struct AttachedProcess;
struct HelperConfig;
class LoadStats;
}

namespace skydiag::helper::internal {

// A hang capture's ETW trace still to be stopped. It starts after the hang
// dump and records for captureSeconds before the helper loop stops it, one
// wpr call per tick (MaybeFinalizePendingHangEtwCapture), so the crash event
// is still polled in between instead of only after several blocking wpr
// calls in a row.
struct PendingHangEtwCapture
{
  bool active = false;
  std::filesystem::path etwPath;
  // The hang's incident manifest, empty when none was written.
  std::filesystem::path manifestPath;
  std::uint32_t stopAttempts = 0;
  // When the trace started (GetTickCount64) and how long it records before
  // it is stopped, unless the helper shuts down first.
  std::uint64_t startedAtTick64 = 0;
  std::uint32_t captureSeconds = 0;
};

// How long a hang's ETW trace records: EtwMaxDurationSec, at most
// kHangEtwCaptureMaxSeconds (the freeze is already in the dump; this is the
// stretch right after it).
inline constexpr std::uint32_t kHangEtwCaptureMaxSeconds = 10u;
constexpr std::uint32_t HangEtwCaptureSeconds(std::uint32_t etwMaxDurationSec) noexcept
{
  return etwMaxDurationSec < kHangEtwCaptureMaxSeconds ? etwMaxDurationSec : kHangEtwCaptureMaxSeconds;
}

// A hang dump nothing has analyzed yet, kept back until its hang settles. A
// freeze in a menu that lasts until the game exits with code 0 was a slow
// shutdown, and its dump is removed before an analysis puts it in
// crash_history.json (unless KeepSlowShutdownHangDumps). It is analyzed when
// the heartbeat comes back, when the game exits otherwise (by the exit viewer
// when one opens), or when the helper stops first. Only a capture that may be
// a slow shutdown is held (MayBeSlowShutdown); any other is analyzed at once.
struct HeldHangDump
{
  std::wstring dumpPath;
  // Run the headless analysis when it is released (AutoAnalyzeDump and no
  // viewer that analyzes it).
  bool analyze = false;
};

// Whether a confirmed hang may be the game quitting slowly: quitting runs from
// a menu (the pause menu, the main menu, the journal, the console's qqq), so
// the state flags have a menu open and no load. Every slow shutdown seen
// (2026-09-18..28, 11-01) was one. A freeze during play is never one, even if
// something then ends the game with exit code 0 (a quit hotkey, a launcher).
constexpr bool MayBeSlowShutdown(std::uint32_t stateFlags, bool isLoading) noexcept
{
  return (stateFlags & skydiag::kState_InMenu) != 0u && (stateFlags & skydiag::kState_Loading) == 0u && !isLoading;
}

struct HangCaptureState
{
  bool hangCapturedThisEpisode = false;
  // A dialog window was on screen at this episode's hang capture (e.g. a
  // plugin's error dialog that may then end the game).
  bool modalDialogSeenThisEpisode = false;
  bool warnedHeartbeatNotInitialized = false;

  bool hangSuppressedNotForegroundThisEpisode = false;
  bool hangSuppressedForegroundGraceThisEpisode = false;
  bool hangSuppressedForegroundResponsiveThisEpisode = false;
  // A dialog window lifted the responsive-window suppression this episode
  // (logged once).
  bool hangSuppressionLiftedForDialogThisEpisode = false;
  // The game was already closing when this episode's hang was confirmed
  // (IsGameProcessClosing): no dump, logged once. Not a captured hang, so
  // the exit classification still sees only the stalled heartbeat.
  bool hangSkippedAsClosingThisEpisode = false;
  // This episode's hang was captured in a state that may be a slow shutdown
  // (MayBeSlowShutdown). Only such a hang counts as a slow shutdown at a
  // normal exit (IsSlowShutdownAtExit).
  bool captureMayBeSlowShutdownThisEpisode = false;
  // The threshold this episode's hang was captured at. The exit judges the
  // stall by it, not by the flags left at the exit (a load flag there would
  // switch to the loading threshold and hide a slow shutdown).
  std::uint32_t captureThresholdSecThisEpisode = 0;

  skydiag::helper::HangSuppressionState hangSuppressionState{};
  HWND targetWindow = nullptr;

  bool wasLoading = false;
  std::uint64_t loadStartQpc = 0;

  PendingHangEtwCapture pendingEtw{};
  HeldHangDump heldDump{};
};

enum class HangTickResult : std::uint8_t {
  kContinue = 0,
  kBreak = 1,
};

HangTickResult HandleHangTick(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  skydiag::helper::LoadStats* loadStats,
  const std::filesystem::path& loadStatsPath,
  std::uint32_t* adaptiveLoadingThresholdSec,
  std::uint64_t attachNowQpc,
  std::wstring* pendingHangViewerDumpPath,
  HangCaptureState* state);

// Starts the held hang dump's analysis when it asks for one, and clears it.
// `why` ends the log line ("the game recovered").
void ReleaseHeldHangDump(
  const skydiag::helper::HelperConfig& cfg,
  const std::filesystem::path& outBase,
  std::wstring_view why,
  HangCaptureState* state);

// Stops a hang capture's pending ETW trace once it has recorded for its
// captureSeconds: one wpr -stop per call, and after the last of its bounded
// attempts, wpr -cancel. `force` (at helper shutdown, which follows the game's
// exit) stops it now and runs every remaining step.
void MaybeFinalizePendingHangEtwCapture(
  const skydiag::helper::HelperConfig& cfg,
  const std::filesystem::path& outBase,
  bool force,
  PendingHangEtwCapture* pending);

}  // namespace skydiag::helper::internal
