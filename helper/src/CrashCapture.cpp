#include "CrashCapture.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "CrashEtwCapture.h"
#include "CaptureCommon.h"
#include "DumpToolLaunch.h"
#include "EtwCapture.h"
#include "HelperCommon.h"
#include "HelperLog.h"
#include "IncidentManifest.h"
#include "PendingCrashAnalysis.h"
#include "PluginScanner.h"
#include "SkyrimDiagHelper/CrashHeuristics.h"
#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/DumpWriter.h"
#include "SkyrimDiagHelper/HeadlessAnalysisPolicy.h"
#include "SkyrimDiagHelper/ProcessAttach.h"
#include "SkyrimDiagShared.h"

namespace skydiag::helper::internal {
namespace {

void WriteWerFallbackHint(const std::filesystem::path& outBase)
{
  const std::string hint =
    "SkyrimDiag dump capture failed. As a fallback, you can enable Windows Error Reporting LocalDumps.\n"
    "Registry path:\n"
    "  HKLM\\SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting\\LocalDumps\\SkyrimSE.exe\n"
    "Recommended values:\n"
    "  DumpType (DWORD) = 2   ; full dump\n"
    "  DumpCount (DWORD) = 10\n"
    "  DumpFolder (EXPAND_SZ) = <your output folder>\n"
    "Reference: https://learn.microsoft.com/windows/win32/wer/collecting-user-mode-dumps\n";
  WriteTextFileUtf8(outBase / L"SkyrimDiag_WER_LocalDumps_Hint.txt", hint);
}

std::wstring Hex32(std::uint32_t v)
{
  wchar_t buf[11]{};
  std::swprintf(buf, 11, L"0x%08X", static_cast<unsigned int>(v));
  return buf;
}

std::wstring Hex64(std::uint64_t v)
{
  wchar_t buf[19]{};
  std::swprintf(buf, 19, L"0x%016llX", static_cast<unsigned long long>(v));
  return buf;
}

}  // namespace

bool HandleCrashEventTick(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  DWORD waitMs,
  bool* crashCaptured,
  PendingCrashEtwCapture* pendingCrashEtw,
  PendingCrashAnalysis* pendingCrashAnalysis,
  std::wstring* lastCrashDumpPath,
  std::wstring* pendingHangViewerDumpPath,
  std::wstring* pendingCrashViewerDumpPath)
{
  if (!proc.crashEvent) {
    Sleep(waitMs);
    return false;
  }

  const DWORD w = WaitForSingleObject(proc.crashEvent, waitMs);
  if (w == WAIT_FAILED) {
    AppendLogLine(outBase, L"Crash event wait failed: " + std::to_wstring(GetLastError()));
    if (waitMs > 0) {
      Sleep(waitMs);
    }
    return false;
  }
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

  const std::uint32_t exceptionCodeAtCrash = proc.shm ? proc.shm->header.crash.exception_code : 0u;
  const std::uint64_t exceptionAddrAtCrash = proc.shm ? proc.shm->header.crash.exception_addr : 0ull;
  const std::uint32_t faultingTidAtCrash = proc.shm ? proc.shm->header.crash.faulting_tid : 0u;
  AppendLogLine(
    outBase,
    L"Crash event signaled (exception_code=" + Hex32(exceptionCodeAtCrash)
      + L", exception_addr=" + Hex64(exceptionAddrAtCrash)
      + L", tid=" + std::to_wstring(faultingTidAtCrash)
      + L").");

  const bool strongExceptionAtCrash = skydiag::helper::IsStrongCrashException(exceptionCodeAtCrash);

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
  const std::filesystem::path dumpFs(dumpPath);
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
    /*pluginScanJson=*/{},
    /*isCrash=*/true,
    cfg.dumpMode,
    &dumpErr);

  if (!dumpOk) {
    AppendLogLine(outBase, L"Crash dump failed: " + dumpErr);
    std::wcerr << L"[SkyrimDiagHelper] Crash dump failed: " << dumpErr << L"\n";
    if (cfg.enableWerDumpFallbackHint) {
      WriteWerFallbackHint(outBase);
      AppendLogLine(
        outBase,
        L"Wrote WER LocalDumps fallback hint: " + (outBase / L"SkyrimDiag_WER_LocalDumps_Hint.txt").wstring());
    }
    // Remove the empty/partial dump file so it doesn't confuse users.
    std::error_code ec;
    std::filesystem::remove(dumpPath, ec);
    if (lastCrashDumpPath) {
      lastCrashDumpPath->clear();
    }
    // Even though the dump failed, still mark as captured so we don't
    // retry on subsequent crash events for the same incident.
    if (crashCaptured) {
      *crashCaptured = true;
    }
    return true;
  }

  if (lastCrashDumpPath) {
    *lastCrashDumpPath = dumpPath;
  }

  const std::uint32_t stateFlagsAtCrash = proc.shm ? proc.shm->header.state_flags : 0u;
  const bool inMenuAtCrash = (stateFlagsAtCrash & skydiag::kState_InMenu) != 0u;

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
      if (exitCode == 0 && !strongExceptionAtCrash) {
        AppendLogLine(outBase, L"Crash event received but process exited normally (exit_code=0); "
          L"deleting dump (likely shutdown exception).");
        std::error_code ec;
        std::filesystem::remove(dumpPath, ec);
        if (lastCrashDumpPath) {
          lastCrashDumpPath->clear();
        }
        return false;
      }
      if (exitCode == 0 && strongExceptionAtCrash) {
        AppendLogLine(
          outBase,
          L"Crash event received but process exited with exit_code=0 and strong exception_code="
            + Hex32(exceptionCodeAtCrash)
            + L"; keeping dump and preserving crash auto-actions.");
      }
      if (inMenuAtCrash && exitCode != 0) {
        AppendLogLine(
          outBase,
          L"Crash event reached menu/shutdown boundary with non-zero exit (exit_code="
            + std::to_wstring(exitCode)
            + L", state_flags="
            + std::to_wstring(stateFlagsAtCrash)
            + L"); keeping dump and preserving crash auto-actions.");
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
          if (exitCode == 0 && !strongExceptionAtCrash) {
            AppendLogLine(outBase, L"Crash event received but process exited normally during "
              L"heartbeat check (exit_code=0, check=" + std::to_wstring(attempt + 1)
              + L"); deleting dump (likely shutdown exception).");
            std::error_code ec;
            std::filesystem::remove(dumpPath, ec);
            if (lastCrashDumpPath) {
              lastCrashDumpPath->clear();
            }
            recovered = true;
          } else if (exitCode == 0 && strongExceptionAtCrash) {
            AppendLogLine(
              outBase,
              L"Crash event saw exit_code=0 during heartbeat check but exception_code="
                + Hex32(exceptionCodeAtCrash)
                + L" is strong; keeping dump and preserving crash auto-actions.");
          } else if (inMenuAtCrash) {
            AppendLogLine(
              outBase,
              L"Crash event reached menu/shutdown boundary during heartbeat check with non-zero exit (exit_code="
                + std::to_wstring(exitCode)
                + L", check="
                + std::to_wstring(attempt + 1)
                + L", state_flags="
                + std::to_wstring(stateFlagsAtCrash)
                + L"); keeping dump and preserving crash auto-actions.");
          }
          break;
        }
      }
      if (recovered) {
        if (lastCrashDumpPath) {
          lastCrashDumpPath->clear();
        }
        return false;
      }
      // Heartbeat stalled for entire extended window: real crash / freeze.
    }
  }

  // ---- Dump is valid — proceed with post-processing ------------------------
  {
    std::wcout << L"[SkyrimDiagHelper] Crash dump written: " << dumpPath << L"\n";

    {
      const std::string pluginScanJson = CollectPluginScanJson(proc, outBase);
      if (!pluginScanJson.empty()) {
        const auto pluginScanPath = dumpFs.parent_path() / (dumpFs.stem().wstring() + L"_PluginScan.json");
        WriteTextFileUtf8(pluginScanPath, pluginScanJson);
        AppendLogLine(outBase, L"Plugin scan sidecar written: " + pluginScanPath.wstring());
      }
    }

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
      const auto queueDeferredCrashViewer = [&](std::wstring_view reasonTag) -> bool {
        if (!pendingCrashViewerDumpPath) {
          AppendLogLine(
            outBase,
            L"Crash viewer defer skipped: pending path state missing (reason="
              + std::wstring(reasonTag)
              + L").");
          return false;
        }
        if (pendingCrashViewerDumpPath->empty()) {
          *pendingCrashViewerDumpPath = dumpPath;
          return true;
        }
        if (*pendingCrashViewerDumpPath == dumpPath) {
          return true;
        }
        AppendLogLine(
          outBase,
          L"Deferred crash viewer already queued for previous dump; keeping existing queue (existing="
            + std::filesystem::path(*pendingCrashViewerDumpPath).filename().wstring()
            + L", ignored="
            + dumpFs.filename().wstring()
            + L", reason="
            + std::wstring(reasonTag)
            + L").");
        return false;
      };

      if (!cfg.autoOpenCrashOnlyIfProcessExited) {
        const auto launch = StartDumpToolViewer(cfg, dumpPath, outBase, L"crash");
        viewerNow = (launch == DumpToolViewerLaunchResult::kLaunched);
      } else if (proc.process) {
        const DWORD waitExitMs = static_cast<DWORD>(std::min<std::uint32_t>(cfg.autoOpenCrashWaitForExitMs, 10000u));
        const DWORD wExit = WaitForSingleObject(proc.process, waitExitMs);
        if (wExit == WAIT_OBJECT_0) {
          const auto launch = StartDumpToolViewer(cfg, dumpPath, outBase, L"crash_exit");
          viewerNow = (launch == DumpToolViewerLaunchResult::kLaunched);
          if (viewerNow) {
            AppendLogLine(
              outBase,
              L"Auto-opened DumpTool viewer for crash after process exit during wait window (wait_ms="
                + std::to_wstring(waitExitMs)
                + L", dump="
                + dumpFs.filename().wstring()
                + L").");
          } else {
            AppendLogLine(
              outBase,
              L"Crash viewer auto-open attempt failed after process exit during wait window (wait_ms="
                + std::to_wstring(waitExitMs)
                + L", dump="
                + dumpFs.filename().wstring()
                + L").");
          }
        } else if (wExit == WAIT_TIMEOUT) {
          const bool deferred = queueDeferredCrashViewer(L"wait_timeout");
          AppendLogLine(
            outBase,
            L"Crash dump captured but process is still running after auto-open wait (wait_ms="
              + std::to_wstring(waitExitMs)
              + L", dump="
              + dumpFs.filename().wstring()
              + L"); "
              + (deferred ? L"deferring viewer to process exit." : L"deferred viewer queue unchanged."));
        } else {
          const DWORD le = GetLastError();
          const bool deferred = queueDeferredCrashViewer(L"wait_failed");
          AppendLogLine(
            outBase,
            L"Crash viewer auto-open wait failed (wait_ms="
              + std::to_wstring(waitExitMs)
              + L", err="
              + std::to_wstring(le)
              + L"); "
              + (deferred ? L"deferring viewer to process exit." : L"deferred viewer queue unchanged."));
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

    ApplyRetentionFromConfig(cfg, outBase);

    if (crashCaptured) {
      *crashCaptured = true;
    }
    AppendLogLine(outBase, L"Crash captured; waiting for process exit.");
  }
  return true;
}

}  // namespace skydiag::helper::internal
