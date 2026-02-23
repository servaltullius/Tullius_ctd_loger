#include "HangCapture.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "CaptureCommon.h"
#include "DumpToolLaunch.h"
#include "EtwCapture.h"
#include "HelperCommon.h"
#include "HelperLog.h"
#include "IncidentManifest.h"
#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/DumpWriter.h"
#include "SkyrimDiagHelper/HangDetect.h"
#include "SkyrimDiagHelper/HangSuppression.h"
#include "SkyrimDiagHelper/HeadlessAnalysisPolicy.h"
#include "SkyrimDiagHelper/LoadStats.h"
#include "SkyrimDiagHelper/ProcessAttach.h"
#include "SkyrimDiagHelper/WctCapture.h"
#include "SkyrimDiagShared.h"
#include "WindowHeuristics.h"

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
    state->hangCapturedThisEpisode = false;
    state->hangSuppressedNotForegroundThisEpisode = false;
    state->hangSuppressedForegroundGraceThisEpisode = false;
    state->hangSuppressedForegroundResponsiveThisEpisode = false;
    state->hangSuppressionState = {};
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
  const bool isForeground = IsPidInForeground(proc.pid);
  if (!state->targetWindow || !IsWindow(state->targetWindow)) {
    state->targetWindow = FindMainWindowForPid(proc.pid);
  }
  const bool isWindowResponsive = state->targetWindow && IsWindowResponsive(state->targetWindow, 250);
  const auto hangSup = skydiag::helper::EvaluateHangSuppression(
    state->hangSuppressionState,
    decision.isHang,
    isForeground,
    decision.isLoading,
    isWindowResponsive,
    cfg.suppressHangWhenNotForeground,
    static_cast<std::uint64_t>(now.QuadPart),
    proc.shm->header.last_heartbeat_qpc,
    proc.shm->header.qpc_freq,
    cfg.foregroundGraceSec);
  if (hangSup.suppress) {
    if (hangSup.reason == skydiag::helper::HangSuppressionReason::kNotForeground) {
      if (!state->hangSuppressedNotForegroundThisEpisode) {
        state->hangSuppressedNotForegroundThisEpisode = true;
        const bool inMenu = (stateFlags & skydiag::kState_InMenu) != 0u;
        AppendLogLine(outBase, L"Hang detected but Skyrim is not foreground; suppressing hang dump. "
          L"(secondsSinceHeartbeat=" + std::to_wstring(decision.secondsSinceHeartbeat) +
          L", threshold=" + std::to_wstring(decision.thresholdSec) +
          L", loading=" + std::to_wstring(decision.isLoading ? 1 : 0) +
          L", inMenu=" + std::to_wstring(inMenu ? 1 : 0) + L")");
      }
    } else if (hangSup.reason == skydiag::helper::HangSuppressionReason::kForegroundGrace) {
      if (!state->hangSuppressedForegroundGraceThisEpisode) {
        state->hangSuppressedForegroundGraceThisEpisode = true;
        AppendLogLine(outBase, L"Hang detected after returning to foreground, but heartbeat has not advanced yet; waiting for grace period before capturing hang dump.");
      }
    } else if (hangSup.reason == skydiag::helper::HangSuppressionReason::kForegroundResponsive) {
      if (!state->hangSuppressedForegroundResponsiveThisEpisode) {
        state->hangSuppressedForegroundResponsiveThisEpisode = true;
        AppendLogLine(outBase, L"Hang detected after returning to foreground, but the window is responsive; assuming Alt-Tab/pause and skipping hang dump.");
      }
    }
    state->hangCapturedThisEpisode = false;
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
    state->hangCapturedThisEpisode = false;
    state->hangSuppressedNotForegroundThisEpisode = false;
    state->hangSuppressedForegroundGraceThisEpisode = false;
    state->hangSuppressedForegroundResponsiveThisEpisode = false;
    state->hangSuppressionState = {};
    return HangTickResult::kContinue;
  }

  const bool isForeground2 = IsPidInForeground(proc.pid);
  if (!state->targetWindow || !IsWindow(state->targetWindow)) {
    state->targetWindow = FindMainWindowForPid(proc.pid);
  }
  const bool isWindowResponsive2 = state->targetWindow && IsWindowResponsive(state->targetWindow, 250);
  const auto hangSup2 = skydiag::helper::EvaluateHangSuppression(
    state->hangSuppressionState,
    decision2.isHang,
    isForeground2,
    decision2.isLoading,
    isWindowResponsive2,
    cfg.suppressHangWhenNotForeground,
    static_cast<std::uint64_t>(now2.QuadPart),
    proc.shm->header.last_heartbeat_qpc,
    proc.shm->header.qpc_freq,
    cfg.foregroundGraceSec);
  if (hangSup2.suppress) {
    if (hangSup2.reason == skydiag::helper::HangSuppressionReason::kNotForeground) {
      if (!state->hangSuppressedNotForegroundThisEpisode) {
        state->hangSuppressedNotForegroundThisEpisode = true;
        const bool inMenu = (stateFlags2 & skydiag::kState_InMenu) != 0u;
        AppendLogLine(outBase, L"Hang confirmed but Skyrim is not foreground; suppressing hang dump. "
          L"(secondsSinceHeartbeat=" + std::to_wstring(decision2.secondsSinceHeartbeat) +
          L", threshold=" + std::to_wstring(decision2.thresholdSec) +
          L", loading=" + std::to_wstring(decision2.isLoading ? 1 : 0) +
          L", inMenu=" + std::to_wstring(inMenu ? 1 : 0) + L")");
      }
    } else if (hangSup2.reason == skydiag::helper::HangSuppressionReason::kForegroundGrace) {
      if (!state->hangSuppressedForegroundGraceThisEpisode) {
        state->hangSuppressedForegroundGraceThisEpisode = true;
        AppendLogLine(outBase, L"Hang confirmed after returning to foreground, but heartbeat has not advanced yet; waiting for grace period before capturing hang dump.");
      }
    } else if (hangSup2.reason == skydiag::helper::HangSuppressionReason::kForegroundResponsive) {
      if (!state->hangSuppressedForegroundResponsiveThisEpisode) {
        state->hangSuppressedForegroundResponsiveThisEpisode = true;
        AppendLogLine(outBase, L"Hang confirmed after returning to foreground, but the window is responsive; assuming Alt-Tab/pause and skipping hang dump.");
      }
    }
    state->hangCapturedThisEpisode = false;
    return HangTickResult::kContinue;
  }

  const auto ts = Timestamp();
  const auto wctPath = outBase / (L"SkyrimDiag_WCT_" + ts + L".json");
  const auto dumpPath = (outBase / (L"SkyrimDiag_Hang_" + ts + L".dmp")).wstring();
  const auto etwPath = outBase / (L"SkyrimDiag_Hang_" + ts + L".etl");
  const auto manifestPath = outBase / (L"SkyrimDiag_Incident_Hang_" + ts + L".json");
  bool manifestWritten = false;

  bool etwStarted = false;
  if (cfg.enableEtwCaptureOnHang) {
    std::wstring etwUsedProfile;
    std::wstring etwErr;
    if (StartEtwCaptureForHang(cfg, outBase, &etwUsedProfile, &etwErr)) {
      etwStarted = true;
      AppendLogLine(outBase, L"ETW hang capture started (profile=" + etwUsedProfile + L").");
    } else {
      AppendLogLine(outBase, L"ETW hang capture start failed: " + etwErr);
    }
  }
  std::string etwStatus = cfg.enableEtwCaptureOnHang ? (etwStarted ? "recording" : "start_failed") : "disabled";

  nlohmann::json wctJson;
  std::wstring wctErr;
  if (!skydiag::helper::CaptureWct(proc.pid, wctJson, &wctErr)) {
    std::wcerr << L"[SkyrimDiagHelper] WCT capture failed: " << wctErr << L"\n";
    wctJson = nlohmann::json::object();
    wctJson["pid"] = proc.pid;
    wctJson["error"] = "capture_failed";
  }
  if (wctJson.contains("debugPrivilegeEnabled") && wctJson["debugPrivilegeEnabled"].is_boolean() &&
      !wctJson["debugPrivilegeEnabled"].get<bool>()) {
    AppendLogLine(outBase, L"Warning: EnableDebugPrivilege failed; WCT capture may be incomplete.");
  }

  wctJson["capture"] = nlohmann::json::object();
  wctJson["capture"]["kind"] = "hang";
  wctJson["capture"]["secondsSinceHeartbeat"] = decision2.secondsSinceHeartbeat;
  wctJson["capture"]["thresholdSec"] = decision2.thresholdSec;
  wctJson["capture"]["isLoading"] = decision2.isLoading;
  wctJson["capture"]["stateFlags"] = stateFlags2;

  const std::string wctUtf8 = wctJson.dump(2);
  WriteTextFileUtf8(wctPath, wctUtf8);

  const std::string pluginScanJson = CollectPluginScanJson(proc, outBase);

  bool dumpWritten = false;
  std::wstring dumpErr;
  if (!skydiag::helper::WriteDumpWithStreams(
        proc.process,
        proc.pid,
        dumpPath,
        proc.shm,
        proc.shmSize,
        wctUtf8,
        pluginScanJson,
        /*isCrash=*/false,
        cfg.dumpMode,
        &dumpErr)) {
    std::wcerr << L"[SkyrimDiagHelper] Hang dump failed: " << dumpErr << L"\n";
    AppendLogLine(outBase, L"Hang dump failed: " + dumpErr);
  } else {
    dumpWritten = true;
    std::wcout << L"[SkyrimDiagHelper] Hang dump written: " << dumpPath << L"\n";
    std::wcout << L"[SkyrimDiagHelper] WCT written: " << wctPath.wstring() << L"\n";
    std::wcout << L"[SkyrimDiagHelper] Hang decision: secondsSinceHeartbeat=" << decision2.secondsSinceHeartbeat
               << L" threshold=" << decision2.thresholdSec << L" loading=" << (decision2.isLoading ? L"1" : L"0") << L"\n";
    AppendLogLine(outBase, L"Hang dump written: " + dumpPath);
    AppendLogLine(outBase, L"WCT written: " + wctPath.wstring());
    AppendLogLine(outBase, L"Hang decision: secondsSinceHeartbeat=" + std::to_wstring(decision2.secondsSinceHeartbeat) +
      L" threshold=" + std::to_wstring(decision2.thresholdSec) + L" loading=" + std::to_wstring(decision2.isLoading ? 1 : 0));
    if (cfg.enableIncidentManifest) {
      const bool inMenu2 = (stateFlags2 & skydiag::kState_InMenu) != 0u;
      nlohmann::json ctx = nlohmann::json::object();
      ctx["seconds_since_heartbeat"] = decision2.secondsSinceHeartbeat;
      ctx["threshold_sec"] = decision2.thresholdSec;
      ctx["is_loading"] = decision2.isLoading;
      ctx["in_menu"] = inMenu2;

      const auto manifest = MakeIncidentManifestV1(
        "hang",
        ts,
        proc.pid,
        std::filesystem::path(dumpPath),
        std::optional<std::filesystem::path>(wctPath),
        etwStarted ? std::optional<std::filesystem::path>(etwPath) : std::nullopt,
        etwStatus,
        stateFlags2,
        ctx,
        cfg,
        cfg.incidentManifestIncludeConfigSnapshot);
      WriteTextFileUtf8(manifestPath, manifest.dump(2));
      AppendLogLine(outBase, L"Incident manifest written: " + manifestPath.wstring());
      manifestWritten = true;
    }

    bool viewerNow = cfg.autoOpenViewerOnHang && !cfg.autoOpenHangAfterProcessExit;
    if (cfg.autoOpenViewerOnHang) {
      if (cfg.autoOpenHangAfterProcessExit) {
        if (pendingHangViewerDumpPath) {
          *pendingHangViewerDumpPath = dumpPath;
        }
        AppendLogLine(outBase, L"Queued hang dump for viewer auto-open on process exit.");
      } else {
        const auto launch = StartDumpToolViewer(cfg, dumpPath, outBase, L"hang");
        viewerNow = (launch == DumpToolViewerLaunchResult::kLaunched);
      }
    }
    if (ShouldRunHeadlessDumpAnalysis(cfg, viewerNow, /*analysisRequired=*/false)) {
      StartDumpToolHeadlessIfConfigured(cfg, dumpPath, outBase);
    } else if (viewerNow && cfg.autoAnalyzeDump) {
      AppendLogLine(outBase, L"Skipped headless analysis: viewer auto-open is enabled.");
    }
  }

  if (etwStarted) {
    std::wstring etwErr;
    if (StopEtwCaptureToPath(cfg, outBase, etwPath, &etwErr)) {
      AppendLogLine(outBase, L"ETW hang capture written: " + etwPath.wstring());
      etwStatus = "written";
      if (manifestWritten) {
        std::wstring updErr;
        if (!TryUpdateIncidentManifestEtw(manifestPath, etwPath, "written", &updErr)) {
          AppendLogLine(outBase, L"Incident manifest ETW update failed: " + updErr);
        }
      }
    } else {
      AppendLogLine(outBase, L"ETW hang capture stop failed: " + etwErr);
      etwStatus = "stop_failed";
      if (manifestWritten) {
        std::wstring updErr;
        if (!TryUpdateIncidentManifestEtw(manifestPath, etwPath, "stop_failed", &updErr)) {
          AppendLogLine(outBase, L"Incident manifest ETW update failed: " + updErr);
        }
      }
    }
  }

  if (dumpWritten) {
    ApplyRetentionFromConfig(cfg, outBase);
  }

  state->hangCapturedThisEpisode = true;
  return HangTickResult::kContinue;
}

}  // namespace skydiag::helper::internal
