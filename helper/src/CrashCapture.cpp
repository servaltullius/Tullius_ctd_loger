#include "CrashCapture.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "CrashEtwCapture.h"
#include "DumpToolLaunch.h"
#include "EtwCapture.h"
#include "HelperCommon.h"
#include "HelperLog.h"
#include "IncidentManifest.h"
#include "PendingCrashAnalysis.h"
#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/DumpWriter.h"
#include "SkyrimDiagHelper/HeadlessAnalysisPolicy.h"
#include "SkyrimDiagHelper/ProcessAttach.h"
#include "SkyrimDiagHelper/Retention.h"
#include "SkyrimDiagShared.h"

namespace skydiag::helper::internal {

bool HandleCrashEventTick(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  DWORD waitMs,
  bool* crashCaptured,
  PendingCrashEtwCapture* pendingCrashEtw,
  PendingCrashAnalysis* pendingCrashAnalysis,
  std::wstring* pendingHangViewerDumpPath,
  std::wstring* pendingCrashViewerDumpPath)
{
  if (!proc.crashEvent) {
    Sleep(waitMs);
    return false;
  }

  const DWORD w = WaitForSingleObject(proc.crashEvent, waitMs);
  if (w != WAIT_OBJECT_0) {
    return false;
  }

  // Consume the manual-reset crash event immediately to avoid re-handling the
  // same signal in subsequent ticks.
  if (!ResetEvent(proc.crashEvent)) {
    AppendLogLine(outBase, L"Failed to reset crash event: " + std::to_wstring(GetLastError()));
  }

  if (crashCaptured && *crashCaptured) {
    AppendLogLine(outBase, L"Crash event signaled again; ignoring (already captured).");
    return true;
  }

  // ---- Dump-first strategy ------------------------------------------------
  // Write the dump IMMEDIATELY while the process is (likely) still alive.
  // Filtering for false positives (shutdown exceptions, handled first-chance
  // exceptions) happens AFTER the dump, deleting it if unnecessary.
  //
  // Previous approach waited up to 4.5 s before dumping, which caused a race:
  // fast-terminating crashes killed the process before MiniDumpWriteDump ran,
  // producing 0-byte files.
  // --------------------------------------------------------------------------
  const auto ts = Timestamp();
  const auto dumpPath = (outBase / (L"SkyrimDiag_Crash_" + ts + L".dmp")).wstring();
  const auto etwPath = outBase / (L"SkyrimDiag_Crash_" + ts + L".etl");
  const auto manifestPath = outBase / (L"SkyrimDiag_Incident_Crash_" + ts + L".json");
  if (pendingHangViewerDumpPath) {
    pendingHangViewerDumpPath->clear();
  }

  std::wstring dumpErr;
  const bool dumpOk = skydiag::helper::WriteDumpWithStreams(
    proc.process,
    proc.pid,
    dumpPath,
    proc.shm,
    proc.shmSize,
    /*wctJsonUtf8=*/{},
    /*isCrash=*/true,
    cfg.dumpMode,
    &dumpErr);

  if (!dumpOk) {
    AppendLogLine(outBase, L"Crash dump failed: " + dumpErr);
    std::wcerr << L"[SkyrimDiagHelper] Crash dump failed: " << dumpErr << L"\n";
    // Remove the empty/partial dump file so it doesn't confuse users.
    std::error_code ec;
    std::filesystem::remove(dumpPath, ec);
    // Even though the dump failed, still mark as captured so we don't
    // retry on subsequent crash events for the same incident.
    if (crashCaptured) {
      *crashCaptured = true;
    }
    return true;
  }

  const std::uint32_t stateFlagsAtCrash = proc.shm ? proc.shm->header.state_flags : 0u;
  const bool inMenuAtCrash = (stateFlagsAtCrash & skydiag::kState_InMenu) != 0u;
  bool suppressCrashAutomationForLikelyShutdownException = false;

  // ---- Post-dump false-positive filtering ----------------------------------
  // Now that the dump is safely written, check whether this was actually a
  // benign event and delete the dump if so.
  // --------------------------------------------------------------------------
  if (proc.process) {
    // (1) Shutdown-exception filter: during normal exit, DLL cleanup can raise
    //     exceptions that the VEH intercepts.  If the process exits with
    //     exit_code 0, treat it as benign.
    const DWORD pw = WaitForSingleObject(proc.process, 3000);
    if (pw == WAIT_OBJECT_0) {
      DWORD exitCode = STILL_ACTIVE;
      GetExitCodeProcess(proc.process, &exitCode);
      if (exitCode == 0) {
        AppendLogLine(outBase, L"Crash event received but process exited normally (exit_code=0); "
          L"deleting dump (likely shutdown exception).");
        std::error_code ec;
        std::filesystem::remove(dumpPath, ec);
        return false;
      }
      // Non-zero exit code near menu/shutdown can still be a benign quit path.
      // Keep the dump for manual forensics, but suppress aggressive auto-actions.
      if (inMenuAtCrash) {
        suppressCrashAutomationForLikelyShutdownException = true;
        AppendLogLine(
          outBase,
          L"Crash event received near menu/shutdown boundary (exit_code="
            + std::to_wstring(exitCode)
            + L", state_flags="
            + std::to_wstring(stateFlagsAtCrash)
            + L"); keeping dump but suppressing crash auto-actions.");
      }
    }

    // (2) Handled first-chance exception filter: if the process is still
    //     running and the main-thread heartbeat advances, the game recovered.
    //     Heavy transitions (e.g. returning to main menu with many mods) can
    //     stall the main thread for several seconds while cell/world unloads,
    //     so we check heartbeat multiple times over an extended window.
    if (pw == WAIT_TIMEOUT && proc.shm) {
      constexpr int kMaxHeartbeatChecks = 4;
      constexpr DWORD kHeartbeatCheckIntervalMs = 2000;
      constexpr int kRequiredHeartbeatAdvances = 2;
      bool recovered = false;
      int heartbeatAdvanceCount = 0;
      for (int attempt = 0; attempt < kMaxHeartbeatChecks; ++attempt) {
        const auto hb0 = proc.shm->header.last_heartbeat_qpc;
        Sleep(kHeartbeatCheckIntervalMs);
        const auto hb1 = proc.shm->header.last_heartbeat_qpc;
        if (hb1 > hb0) {
          ++heartbeatAdvanceCount;
          if (heartbeatAdvanceCount >= kRequiredHeartbeatAdvances) {
            AppendLogLine(outBase, L"Crash event received but heartbeat is still advancing across multiple checks (hb0="
              + std::to_wstring(hb0) + L" hb1=" + std::to_wstring(hb1)
              + L", check=" + std::to_wstring(attempt + 1)
              + L", advances=" + std::to_wstring(heartbeatAdvanceCount)
              + L"); deleting dump (likely handled first-chance exception).");
            std::error_code ec;
            std::filesystem::remove(dumpPath, ec);
            recovered = true;
            break;
          }
        }
        // Process may have exited normally during our extended check.
        if (WaitForSingleObject(proc.process, 0) == WAIT_OBJECT_0) {
          DWORD exitCode = STILL_ACTIVE;
          GetExitCodeProcess(proc.process, &exitCode);
          if (exitCode == 0) {
            AppendLogLine(outBase, L"Crash event received but process exited normally during "
              L"heartbeat check (exit_code=0, check=" + std::to_wstring(attempt + 1)
              + L"); deleting dump (likely shutdown exception).");
            std::error_code ec;
            std::filesystem::remove(dumpPath, ec);
            recovered = true;
          } else if (inMenuAtCrash) {
            suppressCrashAutomationForLikelyShutdownException = true;
            AppendLogLine(
              outBase,
              L"Crash event received near menu/shutdown boundary during heartbeat check (exit_code="
                + std::to_wstring(exitCode)
                + L", check="
                + std::to_wstring(attempt + 1)
                + L", state_flags="
                + std::to_wstring(stateFlagsAtCrash)
                + L"); keeping dump but suppressing crash auto-actions.");
          }
          break;
        }
      }
      if (recovered) {
        return false;
      }
      // Heartbeat stalled for entire extended window: real crash / freeze.
    }
  }

  // ---- Dump is valid — proceed with post-processing ------------------------
  {
    std::wcout << L"[SkyrimDiagHelper] Crash dump written: " << dumpPath << L"\n";

    const auto stateFlags = stateFlagsAtCrash;

    bool etwStarted = false;
    std::string etwStatus = cfg.enableEtwCaptureOnCrash ? "start_failed" : "disabled";
    if (cfg.enableEtwCaptureOnCrash && pendingCrashEtw && !pendingCrashEtw->active) {
      const std::wstring effectiveProfile = cfg.etwCrashProfile.empty() ? L"GeneralProfile" : cfg.etwCrashProfile;
      std::wstring etwErr;
      if (StartEtwCaptureWithProfile(cfg, outBase, effectiveProfile, &etwErr)) {
        etwStarted = true;
        etwStatus = "recording";

        pendingCrashEtw->active = true;
        pendingCrashEtw->etwPath = etwPath;
        pendingCrashEtw->manifestPath = cfg.enableIncidentManifest ? manifestPath : std::filesystem::path{};
        pendingCrashEtw->startedAtTick64 = GetTickCount64();
        pendingCrashEtw->captureSeconds = cfg.etwCrashCaptureSeconds;
        pendingCrashEtw->profileUsed = effectiveProfile;

        AppendLogLine(
          outBase,
          L"ETW crash capture started (profile=" + effectiveProfile +
            L", seconds=" + std::to_wstring(pendingCrashEtw->captureSeconds) + L").");
      } else {
        AppendLogLine(outBase, L"ETW crash capture start failed: " + etwErr);
      }
    }

    if (cfg.enableIncidentManifest) {
      nlohmann::json ctx = nlohmann::json::object();
      ctx["reason"] = "crash_event";
      const auto manifest = MakeIncidentManifestV1(
        "crash",
        ts,
        proc.pid,
        std::filesystem::path(dumpPath),
        /*wctPath=*/std::nullopt,
        etwStarted ? std::optional<std::filesystem::path>(etwPath) : std::nullopt,
        etwStatus,
        stateFlags,
        ctx,
        cfg,
        cfg.incidentManifestIncludeConfigSnapshot);
      WriteTextFileUtf8(manifestPath, manifest.dump(2));
      AppendLogLine(outBase, L"Incident manifest written: " + manifestPath.wstring());
    }

    bool crashAnalysisQueued = false;
    if (cfg.autoAnalyzeDump && cfg.enableAutoRecaptureOnUnknownCrash && !suppressCrashAutomationForLikelyShutdownException) {
      std::wstring analyzeQueueErr;
      if (StartPendingCrashAnalysisTask(cfg, dumpPath, outBase, pendingCrashAnalysis, &analyzeQueueErr)) {
        crashAnalysisQueued = true;
        AppendLogLine(outBase, L"Crash headless analysis queued for unknown-bucket recapture policy.");
      } else {
        AppendLogLine(outBase, L"Crash headless analysis queue failed: " + analyzeQueueErr);
      }
    } else if (cfg.autoAnalyzeDump && suppressCrashAutomationForLikelyShutdownException) {
      AppendLogLine(outBase, L"Crash headless analysis suppressed for likely shutdown/menu exception.");
    }

    bool viewerNow = false;
    if (cfg.autoOpenViewerOnCrash && !suppressCrashAutomationForLikelyShutdownException) {
      if (!cfg.autoOpenCrashOnlyIfProcessExited) {
        StartDumpToolViewer(cfg, dumpPath, outBase, L"crash");
        viewerNow = true;
      } else if (proc.process) {
        const DWORD waitExitMs = static_cast<DWORD>(std::min<std::uint32_t>(cfg.autoOpenCrashWaitForExitMs, 10000u));
        const DWORD wExit = WaitForSingleObject(proc.process, waitExitMs);
        if (wExit == WAIT_OBJECT_0) {
          StartDumpToolViewer(cfg, dumpPath, outBase, L"crash_exit");
          viewerNow = true;
          AppendLogLine(outBase, L"Auto-opened DumpTool viewer for crash after process exit.");
        } else if (wExit == WAIT_TIMEOUT) {
          if (pendingCrashViewerDumpPath) {
            *pendingCrashViewerDumpPath = dumpPath;
          }
          AppendLogLine(outBase, L"Crash dump captured but process is still running; deferring viewer to process exit.");
        } else {
          const DWORD le = GetLastError();
          AppendLogLine(outBase, L"Crash viewer auto-open suppressed due to wait failure: " + std::to_wstring(le));
        }
      } else {
        AppendLogLine(outBase, L"Crash viewer auto-open suppressed: missing process handle.");
      }
    } else if (cfg.autoOpenViewerOnCrash && suppressCrashAutomationForLikelyShutdownException) {
      AppendLogLine(outBase, L"Crash viewer auto-open suppressed for likely shutdown/menu exception.");
    }

    if (!crashAnalysisQueued && !suppressCrashAutomationForLikelyShutdownException) {
      if (ShouldRunHeadlessDumpAnalysis(cfg, viewerNow, /*analysisRequired=*/false)) {
        StartDumpToolHeadlessIfConfigured(cfg, dumpPath, outBase);
      } else if (viewerNow && cfg.autoAnalyzeDump) {
        AppendLogLine(outBase, L"Skipped headless analysis: viewer auto-open is enabled.");
      }
    } else if (cfg.autoAnalyzeDump && suppressCrashAutomationForLikelyShutdownException) {
      AppendLogLine(outBase, L"Skipped fallback headless analysis for likely shutdown/menu exception.");
    }

    skydiag::helper::RetentionLimits limits{};
    limits.maxCrashDumps = cfg.maxCrashDumps;
    limits.maxHangDumps = cfg.maxHangDumps;
    limits.maxManualDumps = cfg.maxManualDumps;
    limits.maxEtwTraces = cfg.maxEtwTraces;
    skydiag::helper::ApplyRetentionToOutputDir(outBase, limits);

    if (crashCaptured) {
      *crashCaptured = true;
    }
    AppendLogLine(outBase, L"Crash captured; waiting for process exit.");
  }
  return true;
}

}  // namespace skydiag::helper::internal
