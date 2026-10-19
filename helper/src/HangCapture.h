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

// A hang capture's ETW trace still to be stopped. The helper loop makes one
// wpr call per tick for it (MaybeFinalizePendingHangEtwCapture), so the crash
// event is still polled in between instead of only after several blocking
// wpr calls in a row.
struct PendingHangEtwCapture
{
  bool active = false;
  std::filesystem::path etwPath;
  // The hang's incident manifest, empty when none was written.
  std::filesystem::path manifestPath;
  std::uint32_t stopAttempts = 0;
};

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

// Stops a hang capture's pending ETW trace: one wpr -stop per call, and after
// the last of its bounded attempts, wpr -cancel. `force` (at helper shutdown)
// runs every remaining step now.
void MaybeFinalizePendingHangEtwCapture(
  const skydiag::helper::HelperConfig& cfg,
  const std::filesystem::path& outBase,
  bool force,
  PendingHangEtwCapture* pending);

}  // namespace skydiag::helper::internal
