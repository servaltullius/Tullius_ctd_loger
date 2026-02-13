#include "CrashCapture.h"

#include <Windows.h>

#include <algorithm>
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

namespace skydiag::helper::internal {

bool HandleCrashEventTick(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  DWORD waitMs,
  bool* crashCaptured,
  PendingCrashEtwCapture* pendingCrashEtw,
  PendingCrashAnalysis* pendingCrashAnalysis,
  std::wstring* pendingHangViewerDumpPath)
{
  if (!proc.crashEvent) {
    Sleep(waitMs);
    return false;
  }

  const DWORD w = WaitForSingleObject(proc.crashEvent, waitMs);
  if (w != WAIT_OBJECT_0) {
    return false;
  }

  // Guard against shutdown exceptions: during normal process exit, DLL
  // cleanup can raise exceptions that the VEH intercepts.  If the process
  // exits within a short window with exit code 0, treat it as a benign
  // shutdown exception rather than a real crash.
  if (proc.process) {
    const DWORD pw = WaitForSingleObject(proc.process, 500);
    if (pw == WAIT_OBJECT_0) {
      DWORD exitCode = STILL_ACTIVE;
      GetExitCodeProcess(proc.process, &exitCode);
      if (exitCode == 0) {
        AppendLogLine(outBase, L"Crash event received but process exited normally (exit_code=0); "
          L"skipping crash dump (likely shutdown exception).");
        return false;
      }
      // Non-zero exit code: process crashed and terminated. Proceed with dump.
    }
    // WAIT_TIMEOUT: process still running (real crash). Proceed with dump.
  }

  if (crashCaptured && *crashCaptured) {
    AppendLogLine(outBase, L"Crash event signaled again; ignoring (already captured).");
    return true;
  }

  const auto ts = Timestamp();
  const auto dumpPath = (outBase / (L"SkyrimDiag_Crash_" + ts + L".dmp")).wstring();
  const auto etwPath = outBase / (L"SkyrimDiag_Crash_" + ts + L".etl");
  const auto manifestPath = outBase / (L"SkyrimDiag_Incident_Crash_" + ts + L".json");
  if (pendingHangViewerDumpPath) {
    pendingHangViewerDumpPath->clear();
  }

  std::wstring dumpErr;
  if (!skydiag::helper::WriteDumpWithStreams(
        proc.process,
        proc.pid,
        dumpPath,
        proc.shm,
        proc.shmSize,
        /*wctJsonUtf8=*/{},
        /*isCrash=*/true,
        cfg.dumpMode,
        &dumpErr)) {
    std::wcerr << L"[SkyrimDiagHelper] Crash dump failed: " << dumpErr << L"\n";
  } else {
    std::wcout << L"[SkyrimDiagHelper] Crash dump written: " << dumpPath << L"\n";

    const auto stateFlags = proc.shm->header.state_flags;

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
    if (cfg.autoAnalyzeDump && cfg.enableAutoRecaptureOnUnknownCrash) {
      std::wstring analyzeQueueErr;
      if (StartPendingCrashAnalysisTask(cfg, dumpPath, outBase, pendingCrashAnalysis, &analyzeQueueErr)) {
        crashAnalysisQueued = true;
        AppendLogLine(outBase, L"Crash headless analysis queued for unknown-bucket recapture policy.");
      } else {
        AppendLogLine(outBase, L"Crash headless analysis queue failed: " + analyzeQueueErr);
      }
    }

    bool viewerNow = false;
    if (cfg.autoOpenViewerOnCrash) {
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
          AppendLogLine(outBase, L"Crash dump captured but process is still running; skipping viewer auto-open.");
        } else {
          const DWORD le = GetLastError();
          AppendLogLine(outBase, L"Crash viewer auto-open suppressed due to wait failure: " + std::to_wstring(le));
        }
      } else {
        AppendLogLine(outBase, L"Crash viewer auto-open suppressed: missing process handle.");
      }
    }

    if (!crashAnalysisQueued) {
      if (ShouldRunHeadlessDumpAnalysis(cfg, viewerNow, /*analysisRequired=*/false)) {
        StartDumpToolHeadlessIfConfigured(cfg, dumpPath, outBase);
      } else if (viewerNow && cfg.autoAnalyzeDump) {
        AppendLogLine(outBase, L"Skipped headless analysis: viewer auto-open is enabled.");
      }
    }

    skydiag::helper::RetentionLimits limits{};
    limits.maxCrashDumps = cfg.maxCrashDumps;
    limits.maxHangDumps = cfg.maxHangDumps;
    limits.maxManualDumps = cfg.maxManualDumps;
    limits.maxEtwTraces = cfg.maxEtwTraces;
    skydiag::helper::ApplyRetentionToOutputDir(outBase, limits);
  }

  if (crashCaptured) {
    *crashCaptured = true;
  }
  AppendLogLine(outBase, L"Crash captured; waiting for process exit.");
  return true;
}

}  // namespace skydiag::helper::internal

