#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include "SkyrimDiagHelper/HangSuppression.h"

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

  skydiag::helper::HangSuppressionState hangSuppressionState{};
  HWND targetWindow = nullptr;

  bool wasLoading = false;
  std::uint64_t loadStartQpc = 0;

  PendingHangEtwCapture pendingEtw{};
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
