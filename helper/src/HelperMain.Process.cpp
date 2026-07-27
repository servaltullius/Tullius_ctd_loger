#include "HelperMainInternal.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

#include "CrashCapture.h"
#include "DumpToolLaunch.h"
#include "HelperLog.h"

namespace {

using skydiag::helper::internal::AppendLogLine;
using skydiag::helper::internal::ClearPendingCrashAnalysis;
using skydiag::helper::internal::HandleCrashEventTick;
using skydiag::helper::internal::MaybeStopPendingCrashEtwCapture;
using skydiag::helper::internal::StartDumpToolViewer;

}  // namespace

namespace skydiag::helper::internal {

void DrainCrashEventBeforeExit(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase,
  HelperLoopState* state)
{
  if (!state) {
    return;
  }
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

  // Preserve the capture-time fault metadata before terminating analysis or
  // deleting any filtered artifacts. If metadata cannot be written, retain the
  // dump as a fail-safe even when the INI normally allows deletion.
  const bool evidenceRequired = IsCleanExitEvidenceRequired(cfg, &state->crashCaptured);
  const bool evidenceWritten = TryWriteCleanExitEvidenceRecord(
    cfg,
    outBase,
    &state->crashCaptured,
    L"process_exit",
    cfg.preserveFilteredCrashDumps);
  const bool preserveDump = ShouldPreserveFilteredDump(
    cfg.preserveFilteredCrashDumps,
    evidenceRequired,
    evidenceWritten);

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

  if (state->capturedCrashDumpPath.empty() && !state->pendingCrashViewerDumpPath.empty()) {
    state->capturedCrashDumpPath = state->pendingCrashViewerDumpPath;
  }
  if (!state->capturedCrashDumpPath.empty()) {
    const std::uint32_t removed = RemoveCrashArtifactsForDump(
      outBase,
      state->capturedCrashDumpPath,
      crashEtwPath,
      preserveDump);
    if (preserveDump) {
      AppendLogLine(
        outBase,
        (cfg.preserveFilteredCrashDumps
           ? L"exit_code=0 after filtered crash; dump file preserved, removed "
           : L"exit_code=0 clean-exit evidence write failed; dump preserved as a fail-safe, removed ")
          + std::to_wstring(removed)
          + L" derived crash artifact(s), without keeping the crashCaptured latch "
          + (cfg.preserveFilteredCrashDumps ? L"(PreserveFilteredCrashDumps=1): " : L": ")
          + std::filesystem::path(state->capturedCrashDumpPath).filename().wstring());
    } else {
      AppendLogLine(
        outBase,
        L"exit_code=0 after crash capture; removed "
          + std::to_wstring(removed)
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
  DrainCrashEventBeforeExit(cfg, proc, outBase, state);
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
    GetExitCodeProcess(proc.process, &exitCode);
    if (exitCode != 0) {
      DrainCrashEventBeforeExit(cfg, proc, outBase, state);
    }
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
