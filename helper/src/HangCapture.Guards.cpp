#include "HangCaptureInternal.h"

#include <Windows.h>

#include <filesystem>
#include <string>

#include "HelperLog.h"
#include "SkyrimDiagHelper/HangSuppression.h"
#include "WindowHeuristics.h"
#include "SkyrimDiagShared.h"

namespace skydiag::helper::internal {

void ResetHangCaptureEpisode(HangCaptureState* state)
{
  if (!state) {
    return;
  }
  state->hangCapturedThisEpisode = false;
  state->hangSuppressedNotForegroundThisEpisode = false;
  state->hangSuppressedForegroundGraceThisEpisode = false;
  state->hangSuppressedForegroundResponsiveThisEpisode = false;
  state->hangSuppressionState = {};
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
