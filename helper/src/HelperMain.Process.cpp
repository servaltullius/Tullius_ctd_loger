#include "HelperMainInternal.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

#include "CrashCapture.h"
#include "DumpToolLaunch.h"
#include "HelperLog.h"

namespace skydiag::helper::internal {

void DrainCrashEventBeforeExit(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase,
  DWORD exitCode,
  HelperLoopState* state)
{
  if (!state) {
    return;
  }

  const bool processExited =
    proc.process && WaitForSingleObject(proc.process, 0) == WAIT_OBJECT_0;
  if (!processExited) {
    HandleCrashEventTick(
      cfg,
      proc,
      outBase,
      /*waitMs=*/0,
      &state->crashCaptured,
      &state->pendingCrashEtw,
      &state->pendingCrashAnalysis,
      &state->capturedCrashDumpPath,
      &state->pendingHangViewerDumpPath,
      &state->pendingCrashViewerDumpPath);
    return;
  }

  if (proc.crashEvent && WaitForSingleObject(proc.crashEvent, 0) == WAIT_OBJECT_0) {
    if (!ResetEvent(proc.crashEvent)) {
      AppendLogLine(
        outBase,
        L"Failed to reset post-exit crash event: " + std::to_wstring(GetLastError()));
    }
  }
  if (state->crashCaptured.latched) {
    return;
  }

  CrashEventInfo info{};
  if (!TryCaptureCommittedCrashInfo(proc.shm ? &proc.shm->header : nullptr, &info)) {
    return;
  }

  const bool sameCapturedSequence =
    state->crashCaptured.capturedInfo.crashSeq == info.crashSeq;
  if (exitCode == 0u) {
    if (!info.isStrong) {
      return;
    }
    const bool priorEvidenceAttempt =
      state->crashCaptured.cleanExitEvidenceWritten ||
      state->crashCaptured.cleanExitEvidenceFinalized ||
      !state->crashCaptured.cleanExitEvidencePath.empty();
    if (sameCapturedSequence && priorEvidenceAttempt) {
      AppendLogLine(
        outBase,
        L"Preserving the existing clean-exit evidence attempt for crash_seq="
          + std::to_wstring(info.crashSeq)
          + L"; post-exit drain will not relabel it as not_captured.");
      return;
    }

    state->crashCaptured.latched = true;
    state->crashCaptured.capturedInfo = info;
    state->crashCaptured.cleanExitEvidenceWritten = false;
    state->crashCaptured.cleanExitEvidenceFinalized = false;
    state->crashCaptured.cleanExitEvidencePath.clear();
    state->crashCaptured.cleanExitDumpIdentity = CleanExitDumpIdentity{};
    state->crashCaptured.cleanExitFilterContext = L"process_exit_metadata_drain";
    AppendLogLine(
      outBase,
      L"Drained committed strong-fault metadata after process exit (exit_code=0, crash_seq="
        + std::to_wstring(info.crashSeq)
        + L"); the process address space is already gone, so dump capture was not attempted.");
    return;
  }

  if (state->postExitEvidenceSeq == info.crashSeq) {
    return;
  }
  if (TryWritePostExitCrashEvidenceRecord(outBase, info, exitCode)) {
    state->postExitEvidenceSeq = info.crashSeq;
  }
}

void CleanupCrashArtifactsAfterZeroExit(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase,
  HelperLoopState* state)
{
  if (!state) {
    return;
  }
  if (!state->crashCaptured.latched) {
    return;
  }

  if (state->capturedCrashDumpPath.empty() && !state->pendingCrashViewerDumpPath.empty()) {
    state->capturedCrashDumpPath = state->pendingCrashViewerDumpPath;
  }
  const std::filesystem::path capturedDumpPath(state->capturedCrashDumpPath);

  // Preserve the capture-time fault metadata before terminating analysis or
  // deleting any filtered artifacts. When deletion is intended, commit a
  // pending record first and finalize it only after observing the filesystem.
  const bool evidenceRequired = IsCleanExitEvidenceRequired(cfg, &state->crashCaptured);
  bool preserveDump = cfg.preserveFilteredCrashDumps;
  bool pendingDeleteRecorded = false;
  bool dumpDeletionFailed = false;
  if (evidenceRequired && capturedDumpPath.empty()) {
    (void)TryWriteCleanExitEvidenceRecord(
      cfg,
      outBase,
      &state->crashCaptured,
      L"process_exit_metadata_drain",
      {},
      CleanExitDumpState::kNotCaptured);
  } else if (evidenceRequired && cfg.preserveFilteredCrashDumps) {
    (void)TryWriteCleanExitEvidenceRecord(
      cfg,
      outBase,
      &state->crashCaptured,
      L"process_exit",
      capturedDumpPath,
      CleanExitDumpState::kPreserved);
  } else if (evidenceRequired) {
    pendingDeleteRecorded = TryWriteCleanExitEvidenceRecord(
      cfg,
      outBase,
      &state->crashCaptured,
      L"process_exit",
      capturedDumpPath,
      CleanExitDumpState::kPendingDelete);
    if (!pendingDeleteRecorded) {
      preserveDump = true;
    }
  }

  if (state->pendingCrashAnalysis.active) {
    if (state->pendingCrashAnalysis.process) {
      if (!TerminateProcess(state->pendingCrashAnalysis.process, 1)) {
        AppendLogLine(
          outBase,
          L"exit_code=0 after crash capture; failed to terminate pending crash analysis process: "
            + std::to_wstring(GetLastError()));
      } else {
        AppendLogLine(outBase, L"exit_code=0 after crash capture; terminated pending crash analysis process.");
      }
    }
    ClearPendingCrashAnalysis(&state->pendingCrashAnalysis);
  }

  const std::filesystem::path crashEtwPath = state->pendingCrashEtw.etwPath;
  MaybeStopPendingCrashEtwCapture(cfg, proc, outBase, /*force=*/true, &state->pendingCrashEtw);
  const std::filesystem::path removableCrashEtwPath =
    state->pendingCrashEtw.active ? std::filesystem::path{} : crashEtwPath;
  if (state->pendingCrashEtw.active && !crashEtwPath.empty()) {
    AppendLogLine(
      outBase,
      L"Keeping ETW path out of artifact deletion because WPR cleanup is still unconfirmed: "
        + crashEtwPath.filename().wstring());
  }

  if (!state->capturedCrashDumpPath.empty()) {
    const auto removal = RemoveCrashArtifactsForDump(
      outBase,
      state->capturedCrashDumpPath,
      removableCrashEtwPath,
      preserveDump);
    if (!preserveDump && removal.dumpExistsAfter) {
      dumpDeletionFailed = true;
      preserveDump = true;
    }
    if (evidenceRequired && pendingDeleteRecorded) {
      const auto finalState = removal.dumpExistsAfter
        ? CleanExitDumpState::kDeleteFailed
        : CleanExitDumpState::kDiscarded;
      if (!TryWriteCleanExitEvidenceRecord(
            cfg,
            outBase,
            &state->crashCaptured,
            L"process_exit",
            capturedDumpPath,
            finalState)) {
        AppendLogLine(
          outBase,
          L"Clean-exit evidence finalization failed; the durable pending_delete record "
          L"does not claim an unobserved dump state.");
      }
      preserveDump = removal.dumpExistsAfter;
    }
    if (preserveDump) {
      AppendLogLine(
        outBase,
        (cfg.preserveFilteredCrashDumps
           ? L"exit_code=0 after filtered crash; dump file preserved, removed "
           : (dumpDeletionFailed
                ? L"exit_code=0 after filtered crash; dump deletion failed or was not verifiable, removed "
                : L"exit_code=0 clean-exit evidence write failed; dump preserved as a fail-safe, removed "))
          + std::to_wstring(removal.removedCount)
          + L" derived crash artifact(s), without keeping the crashCaptured latch "
          + (cfg.preserveFilteredCrashDumps ? L"(PreserveFilteredCrashDumps=1): " : L": ")
          + std::filesystem::path(state->capturedCrashDumpPath).filename().wstring());
    } else {
      AppendLogLine(
        outBase,
        L"exit_code=0 after crash capture; removed "
          + std::to_wstring(removal.removedCount)
          + L" crash artifact(s): "
          + std::filesystem::path(state->capturedCrashDumpPath).filename().wstring());
    }
  }
  state->capturedCrashDumpPath.clear();
  state->pendingCrashViewerDumpPath.clear();
  state->crashCaptured.latched = false;
}

void AppendExitClassificationLog(const std::filesystem::path& outBase)
{
  AppendLogLine(
    outBase,
    L"Process exited normally (exit_code=0); treating captured first-chance exceptions as handled "
    L"and suppressing crash viewer/deferred behavior.");
}

void LaunchDeferredViewersAfterExit(
  const HelperConfig& cfg,
  const std::filesystem::path& outBase,
  DWORD exitCode,
  HelperLoopState* state)
{
  if (!state) {
    return;
  }

  if (!state->pendingCrashViewerDumpPath.empty() &&
      cfg.autoOpenViewerOnCrash &&
      exitCode != 0) {
    const std::wstring deferredDumpPath = state->pendingCrashViewerDumpPath;
    const auto launch = StartDumpToolViewer(
      cfg,
      deferredDumpPath,
      outBase,
      L"crash_deferred_exit");
    if (launch == DumpToolViewerLaunchResult::kLaunched) {
      AppendLogLine(
        outBase,
        L"Deferred crash viewer launched after process exit (exit_code="
          + std::to_wstring(exitCode)
          + L", dump="
          + std::filesystem::path(deferredDumpPath).filename().wstring()
          + L").");
    } else {
      AppendLogLine(
        outBase,
        L"Deferred crash viewer launch failed after process exit (exit_code="
          + std::to_wstring(exitCode)
          + L", dump="
          + std::filesystem::path(deferredDumpPath).filename().wstring()
          + L").");
    }
    state->pendingCrashViewerDumpPath.clear();
  } else if (!state->pendingCrashViewerDumpPath.empty() && cfg.autoOpenViewerOnCrash && exitCode == 0) {
    AppendLogLine(
      outBase,
      L"Suppressed deferred crash viewer launch on normal process exit (exit_code=0, dump="
        + std::filesystem::path(state->pendingCrashViewerDumpPath).filename().wstring()
        + L").");
    state->pendingCrashViewerDumpPath.clear();
  }

  if (!state->pendingHangViewerDumpPath.empty() && cfg.autoOpenViewerOnHang && cfg.autoOpenHangAfterProcessExit) {
    const DWORD delayMs = static_cast<DWORD>(std::min<std::uint32_t>(cfg.autoOpenHangDelayMs, 10000u));
    if (delayMs > 0) {
      Sleep(delayMs);
    }
    const auto launch = StartDumpToolViewer(cfg, state->pendingHangViewerDumpPath, outBase, L"hang_exit");
    if (launch == DumpToolViewerLaunchResult::kLaunched) {
      AppendLogLine(outBase, L"Auto-opened DumpTool viewer for latest hang dump after process exit.");
    } else {
      AppendLogLine(outBase, L"Hang viewer auto-open attempt failed after process exit.");
    }
  }
}

void HandleProcessWaitFailed(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase,
  DWORD waitError,
  HelperLoopState* state)
{
  if (!state) {
    return;
  }
  DrainCrashEventBeforeExit(cfg, proc, outBase, STILL_ACTIVE, state);
  MaybeStopPendingCrashEtwCapture(cfg, proc, outBase, /*force=*/true, &state->pendingCrashEtw);
  std::wcerr << L"[SkyrimDiagHelper] Target process wait failed (err=" << waitError << L").\n";
  AppendLogLine(outBase, L"Target process wait failed: " + std::to_wstring(waitError));
}

bool HandleProcessExitTick(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase,
  HelperLoopState* state)
{
  if (!state || !proc.process) {
    return false;
  }

  auto& pendingCrashEtw = state->pendingCrashEtw;

  const DWORD w = WaitForSingleObject(proc.process, 0);
  if (w == WAIT_OBJECT_0) {
    DWORD exitCode = STILL_ACTIVE;
    if (!GetExitCodeProcess(proc.process, &exitCode)) {
      AppendLogLine(
        outBase,
        L"GetExitCodeProcess failed after process signal (error="
          + std::to_wstring(GetLastError())
          + L"); treating exit code as unavailable/non-zero.");
      exitCode = STILL_ACTIVE;
    }
    // Always drain the committed crash generation after observing process exit.
    // This is metadata-only because MiniDumpWriteDump cannot read a dead process.
    DrainCrashEventBeforeExit(cfg, proc, outBase, exitCode, state);
    if (exitCode != 0 &&
        !state->crashCaptured.latched &&
        cfg.enableWerDumpFallbackHint) {
      WriteWerFallbackHint(outBase);
      AppendLogLine(
        outBase,
        L"Abnormal process exit had no internal crash dump; wrote WER LocalDumps fallback guidance: "
          + (outBase / L"SkyrimDiag_WER_LocalDumps_Hint.txt").wstring());
    }
    if (exitCode == 0) {
      CleanupCrashArtifactsAfterZeroExit(cfg, proc, outBase, state);
      AppendExitClassificationLog(outBase);
    }
    MaybeStopPendingCrashEtwCapture(cfg, proc, outBase, /*force=*/true, &pendingCrashEtw);
    std::wcerr << L"[SkyrimDiagHelper] Target process exited (exit_code=" << exitCode << L").\n";
    AppendLogLine(outBase, L"Target process exited (exit_code=" + std::to_wstring(exitCode) + L").");
    LaunchDeferredViewersAfterExit(cfg, outBase, exitCode, state);
    return true;
  }
  if (w == WAIT_FAILED) {
    const DWORD le = GetLastError();
    HandleProcessWaitFailed(cfg, proc, outBase, le, state);
    return true;
  }
  return false;
}

}  // namespace skydiag::helper::internal
