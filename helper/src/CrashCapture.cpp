#include "CrashCapture.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <new>
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
#include "HexFormat.h"
#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/DumpWriter.h"
#include "SkyrimDiagHelper/HeadlessAnalysisPolicy.h"
#include "SkyrimDiagHelper/ProcessAttach.h"
#include "SkyrimDiagShared.h"

namespace skydiag::helper::internal {
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

namespace {

constexpr DWORD kShutdownWaitMs = 3000;
constexpr int kMaxHeartbeatChecks = 4;
constexpr DWORD kHeartbeatCheckIntervalMs = 2000;
constexpr int kRequiredHeartbeatAdvances = 2;
constexpr int kStableSnapshotAttempts = 64;

// The retry window is deliberately short: the faulting game process is usually
// seconds from exiting, and once it does the dump can no longer be produced.
constexpr DWORD kDumpRetryBackoffMs = 250;

bool IsProcessStillActive(HANDLE process) noexcept
{
  if (!process) {
    return false;
  }
  return WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}

std::uint32_t ReadCrashSequence(const skydiag::SharedHeader* header) noexcept
{
  if (!header) {
    return 0u;
  }
  auto* const sequence = reinterpret_cast<volatile LONG*>(
    const_cast<volatile std::uint32_t*>(&header->crash_seq));
  return static_cast<std::uint32_t>(InterlockedCompareExchange(sequence, 0, 0));
}

using skydiag::helper::internal::Hex32;
using skydiag::helper::internal::Hex64;

// Records the evidence of a strong fault that a zero exit code caused us to
// filter. The metadata states whether PreserveFilteredCrashDumps kept the dump;
// derived artifacts and automatic crash actions remain filtered either way.
bool WriteCleanExitEvidenceRecord(
  const std::filesystem::path& outBase,
  const CrashEventInfo& info,
  std::wstring_view context,
  bool dumpPreserved)
{
  const auto ts = Timestamp();
  nlohmann::json j = nlohmann::json::object();
  j["schema"] = "skydiag.clean_exit_evidence.v1";
  j["reason"] = dumpPreserved
    ? "strong_fault_published_but_process_exited_zero_dump_preserved"
    : "strong_fault_published_but_process_exited_zero_dump_discarded";
  j["captured_at"] = WideToUtf8(ts);
  j["filter_context"] = WideToUtf8(context);
  j["dump_preserved"] = dumpPreserved;
  j["exception_code"] = info.exceptionCode;
  j["exception_addr"] = info.exceptionAddr;
  j["faulting_tid"] = info.faultingTid;
  j["state_flags"] = info.stateFlags;
  j["crash_seq"] = info.crashSeq;
  j["in_menu"] = info.inMenu;
  j["note"] = dumpPreserved
    ? "The game published a strong fault record and no heartbeat recovery was observed, "
      "but the process exited with code 0. The filtered dump remains available; derived "
      "crash artifacts and automatic crash actions were suppressed."
    : "The game published a strong fault record and no heartbeat recovery was observed, "
      "but the process exited with code 0, so the dump was discarded as a handled exception. "
      "Set PreserveFilteredCrashDumps=1 to keep the dump itself if this repeats.";

  const auto recordPath = outBase / (L"SkyrimDiag_CleanExitEvidence_" + ts + L".json");
  if (!WriteTextFileUtf8(recordPath, j.dump(2))) {
    AppendLogLine(
      outBase,
      L"Failed to write clean-exit evidence metadata; the filtered dump must be preserved: "
        + recordPath.wstring());
    return false;
  }
  AppendLogLine(
    outBase,
    (dumpPreserved
       ? L"Zero-exit filter preserved the dump for a strong fault record; wrote evidence metadata: "
       : L"Zero-exit filter discarded the dump for a strong fault record; wrote evidence metadata: ")
      + recordPath.wstring());
  return true;
}

FilterVerdict ClassifyExitCodeVerdictWithContext(
  const skydiag::helper::HelperConfig& cfg,
  std::uint32_t exitCode,
  const CrashEventInfo& info,
  const std::filesystem::path& outBase,
  std::wstring_view context,
  int checkIndex,
  CrashCaptureState* crashState)
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
        + L"); deleting dump (handled first-chance or shutdown exception)."
    );
    if (crashState && cfg.enableCleanExitEvidenceQuarantine &&
        ShouldQuarantineCleanExitEvidence(exitCode, info)) {
      crashState->cleanExitFilterContext.assign(context);
    }
    return verdict;
  }

  if (info.inMenu && exitCode != 0) {
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

bool HasNewerCrashRecord(
  const skydiag::SharedHeader* shm,
  std::uint32_t expectedCrashSeq) noexcept
{
  const auto current = ReadCrashSequence(shm);
  return current != 0u && current != expectedCrashSeq;
}

DWORD WaitForCrashOrProcess(HANDLE crashEvent, HANDLE process, DWORD waitMs) noexcept
{
  if (crashEvent && process) {
    // Crash event first: if both handles become signaled together, Windows
    // returns the lowest index and we get a chance to snapshot the newer crash
    // before the process address space disappears.
    const HANDLE handles[] = { crashEvent, process };
    return WaitForMultipleObjects(static_cast<DWORD>(std::size(handles)), handles, FALSE, waitMs);
  }
  if (process) {
    const DWORD result = WaitForSingleObject(process, waitMs);
    if (result == WAIT_OBJECT_0) {
      return WAIT_OBJECT_0 + 1u;
    }
    return result;
  }
  return WAIT_FAILED;
}

// Reads the exit code, reporting whether it is trustworthy. A failed query used
// to leave the STILL_ACTIVE initializer in place, which reads as a non-zero
// (crash-like) exit; callers must not treat that as an observed exit code.
bool TryReadExitCode(
  HANDLE process,
  const std::filesystem::path& outBase,
  DWORD* outExitCode) noexcept
{
  DWORD exitCode = STILL_ACTIVE;
  if (!GetExitCodeProcess(process, &exitCode)) {
    AppendLogLine(
      outBase,
      L"GetExitCodeProcess failed (error=" + std::to_wstring(GetLastError())
        + L"); treating the exit code as unknown and keeping the dump.");
    return false;
  }
  if (outExitCode) {
    *outExitCode = exitCode;
  }
  return true;
}

FilterVerdict FilterShutdownException(
  const skydiag::helper::HelperConfig& cfg,
  HANDLE process,
  HANDLE crashEvent,
  const skydiag::SharedHeader* shm,
  const CrashEventInfo& info,
  const std::filesystem::path& outBase,
  CrashCaptureState* crashState)
{
  const DWORD pw = WaitForCrashOrProcess(crashEvent, process, kShutdownWaitMs);
  if (pw == WAIT_OBJECT_0 && crashEvent) {
    if (HasNewerCrashRecord(shm, info.crashSeq)) {
      AppendLogLine(outBase, L"A newer crash record arrived during shutdown filtering; prioritizing the newer CTD.");
      return FilterVerdict::kRetryNewerCrash;
    }
  }
  if (pw == WAIT_OBJECT_0 + 1u) {
    DWORD exitCode = STILL_ACTIVE;
    if (!TryReadExitCode(process, outBase, &exitCode)) {
      return FilterVerdict::kKeepDump;
    }
    return ClassifyExitCodeVerdictWithContext(
      cfg, exitCode, info, outBase, L"shutdown", -1, crashState);
  }
  if (pw == WAIT_TIMEOUT) {
    return FilterVerdict::kKeepDump;
  }
  return FilterVerdict::kKeepDump;
}

FilterVerdict FilterFirstChanceException(
  const skydiag::helper::HelperConfig& cfg,
  HANDLE process,
  HANDLE crashEvent,
  const skydiag::SharedHeader* shm,
  const CrashEventInfo& info,
  const std::filesystem::path& outBase,
  CrashCaptureState* crashState)
{
  if (!shm) {
    return FilterVerdict::kKeepDump;
  }

  int heartbeatAdvanceCount = 0;
  for (int attempt = 0; attempt < kMaxHeartbeatChecks; ++attempt) {
    const auto hb0 = shm->last_heartbeat_qpc;
    const DWORD waitResult = WaitForCrashOrProcess(crashEvent, process, kHeartbeatCheckIntervalMs);
    if (waitResult == WAIT_OBJECT_0 && crashEvent) {
      if (HasNewerCrashRecord(shm, info.crashSeq)) {
        AppendLogLine(
          outBase,
          L"A newer crash record arrived during first-chance recovery filtering; prioritizing the newer CTD.");
        return FilterVerdict::kRetryNewerCrash;
      }
    }
    if (waitResult == WAIT_OBJECT_0 + 1u) {
      DWORD exitCode = STILL_ACTIVE;
      if (!TryReadExitCode(process, outBase, &exitCode)) {
        return FilterVerdict::kKeepDump;
      }
      return ClassifyExitCodeVerdictWithContext(
        cfg, exitCode, info, outBase, L"heartbeat_check", attempt, crashState);
    }

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
    const auto dumpProfile = skydiag::helper::ResolveDumpProfile(
      cfg.dumpMode,
      skydiag::helper::CaptureKind::Crash);
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
      &dumpProfile,
      /*recaptureDecision=*/nullptr,
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
        DWORD processExitCode = STILL_ACTIVE;
        if (!GetExitCodeProcess(proc.process, &processExitCode)) {
          processExitCode = 0xFFFFFFFFu;
        }
        if (processExitCode == 0) {
          AppendLogLine(
            outBase,
            L"Process exited with exit_code=0 during wait window; "
              L"suppressing deferred crash viewer; normal-exit cleanup will remove filtered crash artifacts (wait_ms="
              + std::to_wstring(waitExitMs)
              + L", dump="
              + dumpFs.filename().wstring()
              + L").");
        } else {
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
      std::wstring analyzeQueueErr;
      if (StartPendingCrashAnalysisTask(cfg, dumpPath, outBase, pendingCrashAnalysis, &analyzeQueueErr)) {
        crashAnalysisQueued = true;
        AppendLogLine(outBase, L"Crash headless analysis queued with tracked process lifetime.");
      } else {
        AppendLogLine(outBase, L"Crash headless analysis queue failed: " + analyzeQueueErr);
      }
    } else if (viewerNow && cfg.autoAnalyzeDump) {
      AppendLogLine(outBase, L"Skipped headless analysis: viewer auto-open is enabled.");
    }
  }

  ApplyRetentionFromConfig(cfg, outBase);
}

}

bool IsCleanExitEvidenceRequired(
  const skydiag::helper::HelperConfig& cfg,
  const CrashCaptureState* crashState) noexcept
{
  return crashState &&
         cfg.enableCleanExitEvidenceQuarantine &&
         ShouldQuarantineCleanExitEvidence(0u, crashState->capturedInfo);
}

bool TryWriteCleanExitEvidenceRecord(
  const skydiag::helper::HelperConfig& cfg,
  const std::filesystem::path& outBase,
  CrashCaptureState* crashState,
  std::wstring_view context,
  bool dumpPreserved)
{
  if (!IsCleanExitEvidenceRequired(cfg, crashState)) {
    return false;
  }
  if (crashState->cleanExitEvidenceWritten) {
    return true;
  }

  if (!WriteCleanExitEvidenceRecord(
        outBase,
        crashState->capturedInfo,
        context,
        dumpPreserved)) {
    return false;
  }
  crashState->cleanExitEvidenceWritten = true;
  return true;
}

const skydiag::SharedLayout* StableSharedSnapshot::layout() const noexcept
{
  if (byteSize < sizeof(skydiag::SharedLayout) || !storage) {
    return nullptr;
  }
  return std::launder(reinterpret_cast<const skydiag::SharedLayout*>(storage.get()));
}

std::size_t StableSharedSnapshot::size() const noexcept
{
  return byteSize;
}

void StableSharedSnapshot::AlignedByteDeleter::operator()(std::byte* ptr) const noexcept
{
  if (ptr) {
    ::operator delete(ptr, std::align_val_t{alignment});
  }
}

bool CaptureStableSharedSnapshot(
  const skydiag::SharedLayout* shm,
  std::size_t shmBytes,
  StableSharedSnapshot* out) noexcept
{
  if (!shm || !out || shmBytes < sizeof(skydiag::SharedLayout)) {
    return false;
  }

  out->storage.reset();
  out->byteSize = 0;
  constexpr std::size_t kSnapshotAlignment = alignof(skydiag::SharedLayout);
  auto* rawStorage = static_cast<std::byte*>(::operator new(
    sizeof(skydiag::SharedLayout),
    std::align_val_t{kSnapshotAlignment},
    std::nothrow));
  if (!rawStorage) {
    return false;
  }
  out->storage = StableSharedSnapshot::Storage(
    rawStorage,
    StableSharedSnapshot::AlignedByteDeleter{kSnapshotAlignment});
  out->byteSize = sizeof(skydiag::SharedLayout);

  for (int attempt = 0; attempt < kStableSnapshotAttempts; ++attempt) {
    const std::uint32_t before = ReadCrashSequence(&shm->header);
    if ((before & 1u) != 0u) {
      SwitchToThread();
      continue;
    }

    std::memcpy(out->storage.get(), shm, sizeof(skydiag::SharedLayout));
    MemoryBarrier();
    const std::uint32_t after = ReadCrashSequence(&shm->header);
    if (before == after && (after & 1u) == 0u) {
      return true;
    }
    SwitchToThread();
  }

  out->storage.reset();
  out->byteSize = 0;
  return false;
}

bool TryClearRecoveredCrashFreeze(
  skydiag::SharedLayout* shm,
  std::uint32_t expectedCrashSeq) noexcept
{
  if (!shm || expectedCrashSeq == 0u || (expectedCrashSeq & 1u) != 0u) {
    return false;
  }

  if (ReadCrashSequence(&shm->header) != expectedCrashSeq) {
    return false;
  }

  InterlockedAnd(
    reinterpret_cast<volatile LONG*>(&shm->header.state_flags),
    ~static_cast<LONG>(skydiag::kState_Frozen));
  MemoryBarrier();

  if (ReadCrashSequence(&shm->header) != expectedCrashSeq) {
    // A newer crash began or committed while the recovered record was being
    // thawed. Restore the freeze so its evidence cannot be overwritten.
    InterlockedOr(
      reinterpret_cast<volatile LONG*>(&shm->header.state_flags),
      static_cast<LONG>(skydiag::kState_Frozen));
    return false;
  }
  return true;
}

CrashEventInfo ExtractCrashInfo(const skydiag::SharedHeader* shm) noexcept
{
  if (!shm) {
    return {};
  }
  auto info = BuildCrashEventInfo(
    shm->crash.exception_code,
    shm->crash.exception_addr,
    shm->crash.faulting_tid,
    shm->state_flags);
  info.crashSeq = ReadCrashSequence(shm);
  return info;
}

bool HandleCrashEventTick(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  DWORD waitMs,
  CrashCaptureState* crashState,
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

  if (crashState && crashState->latched) {
    AppendLogLine(outBase, L"Crash event signaled again; ignoring (already captured).");
    return true;
  }

  StableSharedSnapshot stableSnapshot{};
  const skydiag::SharedLayout* dumpSnapshot = nullptr;
  std::size_t dumpSnapshotBytes = 0;
  if (proc.shm) {
    if (!CaptureStableSharedSnapshot(proc.shm, proc.shmSize, &stableSnapshot)) {
      const bool processStillActive =
        proc.process && WaitForSingleObject(proc.process, 0) == WAIT_TIMEOUT;
      AppendLogLine(
        outBase,
        processStillActive
          ? L"Crash event snapshot was not stable; scheduling a retry before dump capture."
          : L"Crash event snapshot was not stable after process exit; allowing exit fallback handling.");
      if (processStillActive) {
        SetEvent(proc.crashEvent);
      }
      return false;
    }
    dumpSnapshot = stableSnapshot.layout();
    dumpSnapshotBytes = stableSnapshot.size();
  }

  const auto info = ExtractCrashInfo(dumpSnapshot ? &dumpSnapshot->header : nullptr);
  if (!IsCommittedCrashSequence(info.crashSeq)) {
    AppendLogLine(
      outBase,
      L"Crash event rejected before dump capture because crash_seq is not a non-zero committed sequence "
        L"(crash_seq=" + std::to_wstring(info.crashSeq) + L").");
    return false;
  }
  if (crashState) {
    crashState->capturedInfo = info;
    crashState->cleanExitEvidenceWritten = false;
    crashState->cleanExitFilterContext.clear();
  }
  AppendLogLine(
    outBase,
    L"Crash event signaled (exception_code=" + Hex32(info.exceptionCode)
      + L", exception_addr=" + Hex64(info.exceptionAddr)
      + L", tid=" + std::to_wstring(info.faultingTid)
      + L", crash_seq=" + std::to_wstring(info.crashSeq)
      + L").");

  const auto ts = Timestamp();
  const auto dumpPath = (outBase / (L"SkyrimDiag_Crash_" + ts + L".dmp")).wstring();
  const auto dumpProfile = skydiag::helper::ResolveDumpProfile(
    cfg.dumpMode,
    skydiag::helper::CaptureKind::Crash);
  if (pendingHangViewerDumpPath) {
    pendingHangViewerDumpPath->clear();
  }

  // The crash event was already consumed and reset, and the game has committed
  // its record and will not signal again for this fault. Nothing would drive a
  // later attempt, so a transient failure (sharing violation, momentary disk
  // pressure) has to be retried here or the incident is lost outright.
  std::wstring dumpErr;
  bool dumpOk = false;
  for (int attempt = 0; attempt < kDumpWriteAttempts; ++attempt) {
    if (attempt > 0) {
      if (!ShouldAttemptDumpWrite(attempt, IsProcessStillActive(proc.process))) {
        AppendLogLine(
          outBase,
          L"Skipping remaining crash dump retries because the game process already exited; "
            L"its address space is gone and no dump can be produced.");
        break;
      }
      Sleep(kDumpRetryBackoffMs);
      std::error_code retryEc;
      std::filesystem::remove(dumpPath, retryEc);
      AppendLogLine(
        outBase,
        L"Retrying crash dump write (attempt " + std::to_wstring(attempt + 1)
          + L" of " + std::to_wstring(kDumpWriteAttempts) + L").");
    }

    dumpErr.clear();
    dumpOk = skydiag::helper::WriteDumpWithStreams(
      proc.process,
      proc.pid,
      dumpPath,
      dumpSnapshot,
      dumpSnapshotBytes,
      {},
      {},
      true,
      dumpProfile,
      /*isProcessSnapshot=*/false,
      &dumpErr);
    if (dumpOk) {
      if (attempt > 0) {
        AppendLogLine(
          outBase,
          L"Crash dump succeeded on attempt " + std::to_wstring(attempt + 1) + L".");
      }
      break;
    }

    AppendLogLine(
      outBase,
      L"Crash dump attempt " + std::to_wstring(attempt + 1) + L" failed: " + dumpErr);
  }

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

    // Terminal state for this incident: no dump exists, so there is no evidence
    // left to protect. Leaving the game frozen would keep blackbox recording
    // stopped and, with strong-fault preservation active, would block every
    // later CTD from being published. Thawing restores normal capture.
    if (proc.shmWritable) {
      const bool thawed = TryClearRecoveredCrashFreeze(proc.shmWritable, info.crashSeq);
      AppendLogLine(
        outBase,
        thawed
          ? L"Crash dump write failed after all retries; thawed capture so a later CTD can still be recorded."
          : L"Crash dump write failed after all retries; capture was not thawed because a newer crash record "
            L"is already present.");
    }
    if (crashState) {
      crashState->latched = false;
    }
    return true;
  }

  if (lastCrashDumpPath) {
    *lastCrashDumpPath = dumpPath;
  }

  auto verdict = FilterVerdict::kKeepDump;
  if (proc.process) {
    verdict = FilterShutdownException(
      cfg,
      proc.process,
      proc.crashEvent,
      proc.shm ? &proc.shm->header : nullptr,
      info,
      outBase,
      crashState);
    if (verdict == FilterVerdict::kKeepDump && proc.shm) {
      verdict = FilterFirstChanceException(
        cfg, proc.process, proc.crashEvent, &proc.shm->header, info, outBase, crashState);
    }
    if (verdict != FilterVerdict::kKeepDump) {
      if (verdict == FilterVerdict::kDeleteRecovered) {
        const bool thawed = TryClearRecoveredCrashFreeze(proc.shmWritable, info.crashSeq);
        AppendLogLine(
          outBase,
          thawed
            ? L"Recovered first-chance exception thawed blackbox/resource capture for a later real CTD."
            : L"Recovered first-chance exception was not thawed because a newer crash record is already present.");
      } else if (verdict == FilterVerdict::kRetryNewerCrash) {
        AppendLogLine(
          outBase,
          L"Discarding the superseded first-chance dump state and immediately retrying the newer crash event.");
      }

      bool preserveFilteredDump = cfg.preserveFilteredCrashDumps;
      if (verdict == FilterVerdict::kDeleteBenign) {
        const std::wstring_view context =
          (crashState && !crashState->cleanExitFilterContext.empty())
            ? std::wstring_view(crashState->cleanExitFilterContext)
            : std::wstring_view(L"filtered_exit");
        const bool evidenceRequired = IsCleanExitEvidenceRequired(cfg, crashState);
        const bool evidenceWritten = TryWriteCleanExitEvidenceRecord(
          cfg,
          outBase,
          crashState,
          context,
          cfg.preserveFilteredCrashDumps);
        preserveFilteredDump = ShouldPreserveFilteredDump(
          cfg.preserveFilteredCrashDumps,
          evidenceRequired,
          evidenceWritten);
      }
      if (preserveFilteredDump) {
        AppendLogLine(
          outBase,
          cfg.preserveFilteredCrashDumps
            ? L"Filtered dump file preserved without crash post-processing or capture latch "
              L"(PreserveFilteredCrashDumps=1)."
            : L"Clean-exit evidence metadata write failed; preserved the filtered dump as a fail-safe "
              L"without crash post-processing or capture latch.");
      } else {
        std::error_code ec;
        std::filesystem::remove(dumpPath, ec);
      }
      if (lastCrashDumpPath) {
        lastCrashDumpPath->clear();
      }
      if (crashState) {
        crashState->latched = false;
      }
      AppendLogLine(outBase, L"Filtered crash event consumed; capture remains ready for a later CTD.");
      return true;
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

  if (crashState) {
    crashState->latched = ShouldLatchCrashCapture(verdict);
  }
  AppendLogLine(outBase, L"Crash captured; waiting for process exit.");
  return true;
}

}
