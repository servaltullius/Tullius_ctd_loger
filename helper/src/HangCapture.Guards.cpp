#include "HangCaptureInternal.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "HelperLog.h"
#include "SkyrimDiagHelper/HangSuppression.h"
#include "SkyrimDiagHelper/ModalDialogProbe.h"
#include "SkyrimDiagHelper/WctCapture.h"
#include "WindowHeuristics.h"
#include "SkyrimDiagShared.h"

namespace skydiag::helper::internal {

void ResetHangCaptureEpisode(HangCaptureState* state)
{
  if (!state) {
    return;
  }
  state->hangCapturedThisEpisode = false;
  state->modalDialogSeenThisEpisode = false;
  state->hangSuppressedNotForegroundThisEpisode = false;
  state->hangSuppressedForegroundGraceThisEpisode = false;
  state->hangSuppressedForegroundResponsiveThisEpisode = false;
  state->hangSuppressionLiftedForDialogThisEpisode = false;
  state->hangSkippedAsClosingThisEpisode = false;
  state->captureMayBeSlowShutdownThisEpisode = false;
  state->captureThresholdSecThisEpisode = 0;
  state->closingSeenAfterCaptureThisEpisode = false;
  state->nextClosingCheckTick64 = 0;
  state->episodeHeartbeatQpc = 0;
  state->hangSuppressionState = {};
}

std::optional<std::size_t> CountProcessThreads(std::uint32_t pid)
{
  // Only a full walk can say the process is down to one thread.
  const auto threads = skydiag::helper::EnumerateProcessThreads(pid);
  if (!threads.complete) {
    return std::nullopt;
  }
  return threads.ids.size();
}

bool SkipHangIfGameIsClosing(
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  const skydiag::helper::HangDecision& decision,
  HangCaptureState* state)
{
  if (!state) {
    return false;
  }
  if (!skydiag::helper::IsGameProcessClosing(CountProcessThreads(proc.pid))) {
    return false;
  }
  state->hangSkippedAsClosingThisEpisode = true;
  state->episodeHeartbeatQpc = proc.shm ? proc.shm->header.last_heartbeat_qpc : 0u;
  AppendLogLine(
    outBase,
    L"Hang detected while the game is closing (1 thread left, secondsSinceHeartbeat="
      + std::to_wstring(decision.secondsSinceHeartbeat)
      + L", threshold="
      + std::to_wstring(decision.thresholdSec)
      + L"); its exit is not a freeze, skipping hang dump.");
  return true;
}

void NoteGameClosingAfterCapture(
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  HangCaptureState* state)
{
  // A thread snapshot covers every process, so once a second is enough for
  // an exit that outlasted a hang threshold.
  constexpr std::uint64_t kClosingCheckIntervalMs = 1000;
  if (!state || !state->hangCapturedThisEpisode || state->closingSeenAfterCaptureThisEpisode) {
    return;
  }
  const std::uint64_t nowTick64 = GetTickCount64();
  if (nowTick64 < state->nextClosingCheckTick64) {
    return;
  }
  state->nextClosingCheckTick64 = nowTick64 + kClosingCheckIntervalMs;
  if (!skydiag::helper::IsGameProcessClosing(CountProcessThreads(proc.pid))) {
    return;
  }
  state->closingSeenAfterCaptureThisEpisode = true;
  AppendLogLine(
    outBase,
    L"The game entered its exit after the hang capture (1 thread left); a normal exit from here is a slow "
    L"shutdown, not a freeze.");
}

void EnsureTargetWindowForPid(HangCaptureState* state, std::uint32_t pid)
{
  if (!state) {
    return;
  }
  if (!state->targetWindow || !IsWindow(state->targetWindow)) {
    state->targetWindow = FindMainWindowForPid(pid);
  }
}

bool SuppressHangAndLogIfNeeded(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  const skydiag::helper::HangDecision& decision,
  std::uint32_t stateFlags,
  std::uint64_t nowQpc,
  bool confirmedPhase,
  HangCaptureState* state)
{
  if (!state) {
    return false;
  }

  EnsureTargetWindowForPid(state, proc.pid);
  const bool isForeground = IsPidInForeground(proc.pid);
  const bool isWindowResponsive = state->targetWindow && IsWindowResponsive(state->targetWindow, 250);
  const auto hangSup = skydiag::helper::EvaluateHangSuppression(
    state->hangSuppressionState,
    decision.isHang,
    isForeground,
    decision.isLoading,
    isWindowResponsive,
    cfg.suppressHangWhenNotForeground,
    nowQpc,
    proc.shm->header.last_heartbeat_qpc,
    proc.shm->header.qpc_freq,
    cfg.foregroundGraceSec);
  if (!hangSup.suppress) {
    return false;
  }
  // Only probed in that one state, since it enumerates windows.
  if (hangSup.reason == skydiag::helper::HangSuppressionReason::kForegroundResponsive &&
      skydiag::helper::IsHangSuppressionLiftedByModalDialog(
        hangSup.reason, !skydiag::helper::CaptureModalDialogs(proc.pid).empty())) {
    if (!state->hangSuppressionLiftedForDialogThisEpisode) {
      state->hangSuppressionLiftedForDialogThisEpisode = true;
      AppendLogLine(
        outBase,
        L"Hang after returning to foreground with a responsive window, but a dialog window of the game is "
        L"visible (e.g. a plugin's error message); not treating it as an Alt-Tab pause.");
    }
    return false;
  }

  if (hangSup.reason == skydiag::helper::HangSuppressionReason::kNotForeground) {
    if (!state->hangSuppressedNotForegroundThisEpisode) {
      state->hangSuppressedNotForegroundThisEpisode = true;
      const bool inMenu = (stateFlags & skydiag::kState_InMenu) != 0u;
      AppendLogLine(
        outBase,
        std::wstring(confirmedPhase ? L"Hang confirmed but Skyrim is not foreground; suppressing hang dump. "
                                    : L"Hang detected but Skyrim is not foreground; suppressing hang dump. ")
          + L"(secondsSinceHeartbeat="
          + std::to_wstring(decision.secondsSinceHeartbeat)
          + L", threshold="
          + std::to_wstring(decision.thresholdSec)
          + L", loading="
          + std::to_wstring(decision.isLoading ? 1 : 0)
          + L", inMenu="
          + std::to_wstring(inMenu ? 1 : 0)
          + L")");
    }
  } else if (hangSup.reason == skydiag::helper::HangSuppressionReason::kForegroundGrace) {
    if (!state->hangSuppressedForegroundGraceThisEpisode) {
      state->hangSuppressedForegroundGraceThisEpisode = true;
      AppendLogLine(
        outBase,
        confirmedPhase
          ? L"Hang confirmed after returning to foreground, but heartbeat has not advanced yet; waiting for grace period before capturing hang dump."
          : L"Hang detected after returning to foreground, but heartbeat has not advanced yet; waiting for grace period before capturing hang dump.");
    }
  } else if (hangSup.reason == skydiag::helper::HangSuppressionReason::kForegroundResponsive) {
    if (!state->hangSuppressedForegroundResponsiveThisEpisode) {
      state->hangSuppressedForegroundResponsiveThisEpisode = true;
      AppendLogLine(
        outBase,
        confirmedPhase
          ? L"Hang confirmed after returning to foreground, but the window is responsive; assuming Alt-Tab/pause and skipping hang dump."
          : L"Hang detected after returning to foreground, but the window is responsive; assuming Alt-Tab/pause and skipping hang dump.");
    }
  }

  state->hangCapturedThisEpisode = false;
  return true;
}

}  // namespace skydiag::helper::internal
