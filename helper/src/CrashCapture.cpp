#include "CrashCapture.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

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
#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/DumpWriter.h"
#include "SkyrimDiagHelper/HeadlessAnalysisPolicy.h"
#include "SkyrimDiagHelper/ProcessAttach.h"
#include "SkyrimDiagShared.h"

namespace skydiag::helper::internal {
namespace {

constexpr DWORD kShutdownWaitMs = 3000;
constexpr int kMaxHeartbeatChecks = 4;
constexpr DWORD kHeartbeatCheckIntervalMs = 2000;
constexpr int kRequiredHeartbeatAdvances = 2;

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

FilterVerdict ClassifyExitCodeVerdictWithContext(
  std::uint32_t exitCode,
  const CrashEventInfo& info,
  const std::filesystem::path& outBase,
  std::wstring_view context,
  int checkIndex)
{
  const auto verdict = ClassifyExitCodeVerdict(exitCode, info, outBase);
  const std::wstring checkSuffix = (checkIndex >= 0)
    ? (L", check=" + std::to_wstring(checkIndex + 1))
    : L"";

  if (verdict == FilterVerdict::kDeleteBenign) {
    AppendLogLine(
      outBase,
      L"Crash event received but process exited normally (context=" + std::wstring(context)
        + L", exit_code=0"
        + checkSuffix
        + L"); deleting dump (likely shutdown exception)."
    );
    return verdict;
  }

  if (exitCode == 0 && info.isStrong) {
    AppendLogLine(
      outBase,
      L"Crash event received with exit_code=0 but strong exception_code="
        + Hex32(info.exceptionCode)
        + L" (context="
        + std::wstring(context)
        + checkSuffix
        + L"); keeping dump and preserving crash auto-actions.");
  } else if (info.inMenu && exitCode != 0) {
    AppendLogLine(
      outBase,
      L"Crash event reached menu/shutdown boundary with non-zero exit (context="
        + std::wstring(context)
        + L", exit_code="
        + std::to_wstring(exitCode)
        + checkSuffix
        + L", state_flags="
        + std::to_wstring(info.stateFlags)
        + L"); keeping dump and preserving crash auto-actions.");
  }

  return verdict;
}

FilterVerdict FilterShutdownException(
  HANDLE process,
  const CrashEventInfo& info,
  const std::filesystem::path& outBase)
{
  const DWORD pw = WaitForSingleObject(process, kShutdownWaitMs);
  if (pw == WAIT_OBJECT_0) {
    DWORD exitCode = STILL_ACTIVE;
    GetExitCodeProcess(process, &exitCode);
    return ClassifyExitCodeVerdictWithContext(exitCode, info, outBase, L"shutdown", -1);
  }
  if (pw == WAIT_TIMEOUT) {
    return FilterVerdict::kKeepDump;
  }
  return FilterVerdict::kKeepDump;
}

FilterVerdict FilterFirstChanceException(
  HANDLE process,
  const skydiag::SharedHeader* shm,
  const CrashEventInfo& info,
  const std::filesystem::path& outBase)
{
  if (!shm) {
    return FilterVerdict::kKeepDump;
  }

  int heartbeatAdvanceCount = 0;
  for (int attempt = 0; attempt < kMaxHeartbeatChecks; ++attempt) {
    if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
      DWORD exitCode = STILL_ACTIVE;
      GetExitCodeProcess(process, &exitCode);
      return ClassifyExitCodeVerdictWithContext(exitCode, info, outBase, L"heartbeat_check", attempt);
    }

    const auto hb0 = shm->last_heartbeat_qpc;
    Sleep(kHeartbeatCheckIntervalMs);
    const auto hb1 = shm->last_heartbeat_qpc;
    if (hb1 > hb0) {
      ++heartbeatAdvanceCount;
      if (heartbeatAdvanceCount >= kRequiredHeartbeatAdvances) {
        AppendLogLine(
          outBase,
          L"Crash event received but heartbeat is still advancing across multiple checks (hb0="
            + std::to_wstring(hb0)
            + L" hb1="
            + std::to_wstring(hb1)
            + L", check="
            + std::to_wstring(attempt + 1)
            + L", advances="
            + std::to_wstring(heartbeatAdvanceCount)
            + L"); deleting dump (likely handled first-chance exception)."
        );
        return FilterVerdict::kDeleteRecovered;
      }
    }
  }

  return FilterVerdict::kKeepDump;
}

void ProcessValidCrashDump(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  const std::wstring& dumpPath,
  const std::wstring& ts,
  const CrashEventInfo& info,
  PendingCrashEtwCapture* pendingCrashEtw,
  PendingCrashAnalysis* pendingCrashAnalysis,
  std::wstring* pendingCrashViewerDumpPath)
{
  const auto etwPath = outBase / (L"SkyrimDiag_Crash_" + ts + L".etl");
  const auto manifestPath = outBase / (L"SkyrimDiag_Incident_Crash_" + ts + L".json");
  const std::filesystem::path dumpFs(dumpPath);

  std::wcout << L"[SkyrimDiagHelper] Crash dump written: " << dumpPath << L"\n";

  {
    const std::string pluginScanJson = CollectPluginScanJson(proc, outBase);
    if (!pluginScanJson.empty()) {
      const auto pluginScanPath = dumpFs.parent_path() / (dumpFs.stem().wstring() + L"_PluginScan.json");
      WriteTextFileUtf8(pluginScanPath, pluginScanJson);
      AppendLogLine(outBase, L"Plugin scan sidecar written: " + pluginScanPath.wstring());
    }
  }

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
        L"ETW crash capture started (profile=" + effectiveProfile
          + L", seconds="
          + std::to_wstring(pendingCrashEtw->captureSeconds)
          + L").");
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
      std::nullopt,
      etwStarted ? std::optional<std::filesystem::path>(etwPath) : std::nullopt,
      etwStatus,
      info.stateFlags,
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
        const bool deferred = QueueDeferredCrashViewer(dumpPath, pendingCrashViewerDumpPath);
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
        const bool deferred = QueueDeferredCrashViewer(dumpPath, pendingCrashViewerDumpPath);
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
    if (ShouldRunHeadlessDumpAnalysis(cfg, viewerNow, false)) {
      StartDumpToolHeadlessIfConfigured(cfg, dumpPath, outBase);
    } else if (viewerNow && cfg.autoAnalyzeDump) {
      AppendLogLine(outBase, L"Skipped headless analysis: viewer auto-open is enabled.");
    }
  }

  ApplyRetentionFromConfig(cfg, outBase);
}

}

CrashEventInfo ExtractCrashInfo(const skydiag::SharedHeader* shm) noexcept
{
  if (!shm) {
    return {};
  }
  return BuildCrashEventInfo(
    shm->crash.exception_code,
    shm->crash.exception_addr,
    shm->crash.faulting_tid,
    shm->state_flags);
}

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

  if (!ResetEvent(proc.crashEvent)) {
    AppendLogLine(outBase, L"Failed to reset crash event: " + std::to_wstring(GetLastError()));
  }

  if (crashCaptured && *crashCaptured) {
    AppendLogLine(outBase, L"Crash event signaled again; ignoring (already captured).");
    return true;
  }

  const auto info = ExtractCrashInfo(proc.shm ? &proc.shm->header : nullptr);
  AppendLogLine(
    outBase,
    L"Crash event signaled (exception_code=" + Hex32(info.exceptionCode)
      + L", exception_addr=" + Hex64(info.exceptionAddr)
      + L", tid=" + std::to_wstring(info.faultingTid)
      + L").");

  const auto ts = Timestamp();
  const auto dumpPath = (outBase / (L"SkyrimDiag_Crash_" + ts + L".dmp")).wstring();
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
    {},
    {},
    true,
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
    std::error_code ec;
    std::filesystem::remove(dumpPath, ec);
    if (lastCrashDumpPath) {
      lastCrashDumpPath->clear();
    }
    if (crashCaptured) {
      *crashCaptured = true;
    }
    return true;
  }

  if (lastCrashDumpPath) {
    *lastCrashDumpPath = dumpPath;
  }

  if (proc.process) {
    auto verdict = FilterShutdownException(proc.process, info, outBase);
    if (verdict == FilterVerdict::kKeepDump && proc.shm) {
      verdict = FilterFirstChanceException(proc.process, &proc.shm->header, info, outBase);
    }
    if (verdict != FilterVerdict::kKeepDump) {
      if (cfg.preserveFilteredCrashDumps) {
        AppendLogLine(
          outBase,
          L"Crash dump would have been deleted by false-positive filter but preserved (PreserveFilteredCrashDumps=1).");
      } else {
        std::error_code ec;
        std::filesystem::remove(dumpPath, ec);
        if (lastCrashDumpPath) {
          lastCrashDumpPath->clear();
        }
        return false;
      }
    }
  }

  ProcessValidCrashDump(
    cfg,
    proc,
    outBase,
    dumpPath,
    ts,
    info,
    pendingCrashEtw,
    pendingCrashAnalysis,
    pendingCrashViewerDumpPath);

  if (crashCaptured) {
    *crashCaptured = true;
  }
  AppendLogLine(outBase, L"Crash captured; waiting for process exit.");
  return true;
}

}
