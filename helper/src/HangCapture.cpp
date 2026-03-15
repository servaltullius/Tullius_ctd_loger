#include "HangCapture.h"
#include "HangCaptureInternal.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

#include "CaptureCommon.h"
#include "HelperLog.h"
#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/HangDetect.h"
#include "SkyrimDiagHelper/LoadStats.h"
#include "SkyrimDiagShared.h"

namespace skydiag::helper::internal {

HangTickResult HandleHangTick(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  skydiag::helper::LoadStats* loadStats,
  const std::filesystem::path& loadStatsPath,
  std::uint32_t* adaptiveLoadingThresholdSec,
  std::uint64_t attachNowQpc,
  std::wstring* pendingHangViewerDumpPath,
  HangCaptureState* state)
{

  if (!state || !adaptiveLoadingThresholdSec || !loadStats) {
    // Helper should always have these; keep behavior safe if wiring is wrong.
    return HangTickResult::kContinue;
  }

  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);

  const auto stateFlags = proc.shm->header.state_flags;

  if (cfg.enableAdaptiveLoadingThreshold) {
    const bool isLoading = (stateFlags & skydiag::kState_Loading) != 0u;
    if (!state->wasLoading && isLoading) {
      state->loadStartQpc = static_cast<std::uint64_t>(now.QuadPart);
    } else if (state->wasLoading && !isLoading && state->loadStartQpc != 0 && proc.shm->header.qpc_freq != 0) {
      const auto deltaQpc = static_cast<std::uint64_t>(now.QuadPart) - state->loadStartQpc;
      const double seconds = static_cast<double>(deltaQpc) / static_cast<double>(proc.shm->header.qpc_freq);
      const auto secRounded = static_cast<std::uint32_t>(seconds + 0.5);
      if (secRounded > 0) {
        loadStats->AddLoadingSampleSeconds(secRounded);
        loadStats->SaveToFile(loadStatsPath);
        *adaptiveLoadingThresholdSec = loadStats->SuggestedLoadingThresholdSec(cfg);
        std::wcout << L"[SkyrimDiagHelper] Observed loading duration=" << secRounded
                   << L"s -> new adaptive threshold=" << *adaptiveLoadingThresholdSec << L"s\n";
      }
      state->loadStartQpc = 0;
    }
    state->wasLoading = isLoading;
  }

  const std::uint32_t loadingThresholdSec = (cfg.enableAdaptiveLoadingThreshold && loadStats->HasSamples())
    ? *adaptiveLoadingThresholdSec
    : cfg.hangThresholdLoadingSec;

  // Check plugin heartbeat initialization: if last_heartbeat_qpc is still 0,
  // the plugin hasn't started its heartbeat scheduler yet.  Auto hang capture
  // is unreliable without a working heartbeat, so skip until it initializes.
  //
  // Warn after kHeartbeatInitWarnDelaySec so that the plugin has enough time
  // to register its SKSE task-queue callback and send the first heartbeat.
  // 10s is generous; typical init is < 3s even with heavy mod lists.
  constexpr double kHeartbeatInitWarnDelaySec = 10.0;
  if (proc.shm->header.last_heartbeat_qpc == 0) {
    if (!state->warnedHeartbeatNotInitialized && proc.shm->header.qpc_freq != 0) {
      const std::uint64_t deltaQpc = static_cast<std::uint64_t>(now.QuadPart) - attachNowQpc;
      const double secondsSinceAttach = static_cast<double>(deltaQpc) / static_cast<double>(proc.shm->header.qpc_freq);
      if (secondsSinceAttach >= kHeartbeatInitWarnDelaySec) {
        state->warnedHeartbeatNotInitialized = true;
        AppendLogLine(outBase, L"Warning: plugin heartbeat not initialized (last_heartbeat_qpc=0); "
          L"auto hang capture unavailable until heartbeat starts.");
      }
    }
    return HangTickResult::kContinue;
  }

  const auto decision = skydiag::helper::EvaluateHang(
    static_cast<std::uint64_t>(now.QuadPart),
    proc.shm->header.last_heartbeat_qpc,
    proc.shm->header.qpc_freq,
    stateFlags,
    ((stateFlags & skydiag::kState_InMenu) != 0u)
      ? std::max(cfg.hangThresholdInGameSec, cfg.hangThresholdInMenuSec)
      : cfg.hangThresholdInGameSec,
    loadingThresholdSec);

  if (!decision.isHang) {
    ResetHangCaptureEpisode(state);
    return HangTickResult::kContinue;
  }

  if (state->hangCapturedThisEpisode) {
    return HangTickResult::kContinue;
  }

  // Common case: users Alt-Tab away while Skyrim is intentionally paused.
  // In that state, the heartbeat can stop, but it is not actionable to create a hang dump.
  // Default: suppress auto hang dumps while Skyrim is not the foreground process.
  // (Optional) If suppression is disabled, we still try to detect "background pause" by checking
  // whether the game window is responsive.
  if (SuppressHangAndLogIfNeeded(
        cfg,
        proc,
        outBase,
        decision,
        stateFlags,
        static_cast<std::uint64_t>(now.QuadPart),
        /*confirmedPhase=*/false,
        state)) {
    return HangTickResult::kContinue;
  }

  // Avoid generating hang dumps during normal shutdown or transient stalls:
  // Re-check after a short grace period. If the process exits or heartbeats recover, skip capture.
  if (proc.process) {
    const DWORD w = WaitForSingleObject(proc.process, 1500);
    if (w == WAIT_OBJECT_0) {
      AppendLogLine(outBase, L"Hang detected but target process exited during grace period; skipping hang dump.");
      return HangTickResult::kBreak;
    }
    if (w == WAIT_FAILED) {
      const DWORD le = GetLastError();
      std::wcerr << L"[SkyrimDiagHelper] Target process wait failed (err=" << le << L").\n";
      AppendLogLine(outBase, L"Target process wait failed: " + std::to_wstring(le));
      return HangTickResult::kBreak;
    }
  }

  LARGE_INTEGER now2{};
  QueryPerformanceCounter(&now2);
  const auto stateFlags2 = proc.shm->header.state_flags;
  const std::uint32_t inGameThresholdSec2 = ((stateFlags2 & skydiag::kState_InMenu) != 0u)
    ? std::max(cfg.hangThresholdInGameSec, cfg.hangThresholdInMenuSec)
    : cfg.hangThresholdInGameSec;
  const std::uint32_t loadingThresholdSec2 = (cfg.enableAdaptiveLoadingThreshold && loadStats->HasSamples())
    ? *adaptiveLoadingThresholdSec
    : cfg.hangThresholdLoadingSec;
  const auto decision2 = skydiag::helper::EvaluateHang(
    static_cast<std::uint64_t>(now2.QuadPart),
    proc.shm->header.last_heartbeat_qpc,
    proc.shm->header.qpc_freq,
    stateFlags2,
    inGameThresholdSec2,
    loadingThresholdSec2);
  if (!decision2.isHang) {
    AppendLogLine(outBase, L"Hang detected but recovered during grace period; skipping hang dump.");
    ResetHangCaptureEpisode(state);
    return HangTickResult::kContinue;
  }

  if (SuppressHangAndLogIfNeeded(
        cfg,
        proc,
        outBase,
        decision2,
        stateFlags2,
        static_cast<std::uint64_t>(now2.QuadPart),
        /*confirmedPhase=*/true,
        state)) {
    return HangTickResult::kContinue;
  }

  return ExecuteConfirmedHangCapture(
    cfg,
    proc,
    outBase,
    decision2,
    stateFlags2,
    pendingHangViewerDumpPath,
    state);
}

}  // namespace skydiag::helper::internal
