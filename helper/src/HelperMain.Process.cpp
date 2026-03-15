#include "HelperMainInternal.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

#include "SkyrimDiagHelper/CrashHeuristics.h"

#include "CrashCapture.h"
#include "DumpToolLaunch.h"
#include "HelperLog.h"
#include "HexFormat.h"

namespace {

using skydiag::helper::internal::AppendLogLine;
using skydiag::helper::internal::ClearPendingCrashAnalysis;
using skydiag::helper::internal::HandleCrashEventTick;
using skydiag::helper::internal::Hex32;
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

bool HasSharedMemoryStrongCrashEvidence(const AttachedProcess& proc)
{
  return proc.shm && skydiag::helper::IsStrongCrashException(proc.shm->header.crash.exception_code);
}

void CleanupCrashArtifactsAfterZeroExit(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase,
  bool exitCode0StrongCrash,
  std::uint32_t exceptionCode,
  HelperLoopState* state)
{
  if (!state) {
    return;
  }
  if (exitCode0StrongCrash) {
    AppendLogLine(
      outBase,
      L"exit_code=0 after crash capture but exception_code="
        + Hex32(exceptionCode)
        + L" is strong; preserving crash artifacts and crash auto-actions.");
    return;
  }
  if (!state->crashCaptured) {
    return;
  }
  if (cfg.preserveFilteredCrashDumps) {
    AppendLogLine(outBase, L"exit_code=0 after crash capture; dump preserved (PreserveFilteredCrashDumps=1).");
    return;
  }

  if (state->pendingCrashAnalysis.active && state->pendingCrashAnalysis.process) {
    if (!TerminateProcess(state->pendingCrashAnalysis.process, 1)) {
      AppendLogLine(
        outBase,
        L"exit_code=0 after crash capture; failed to terminate pending crash analysis process: "
          + std::to_wstring(GetLastError()));
    } else {
      AppendLogLine(outBase, L"exit_code=0 after crash capture; terminated pending crash analysis process.");
    }
    ClearPendingCrashAnalysis(&state->pendingCrashAnalysis);
  }

  const std::filesystem::path crashEtwPath = state->pendingCrashEtw.etwPath;
  MaybeStopPendingCrashEtwCapture(cfg, proc, outBase, /*force=*/true, &state->pendingCrashEtw);

  if (state->capturedCrashDumpPath.empty() && !state->pendingCrashViewerDumpPath.empty()) {
    state->capturedCrashDumpPath = state->pendingCrashViewerDumpPath;
  }
  if (!state->capturedCrashDumpPath.empty()) {
    const std::uint32_t removed = RemoveCrashArtifactsForDump(outBase, state->capturedCrashDumpPath, crashEtwPath);
    AppendLogLine(
      outBase,
      L"exit_code=0 after crash capture; removed "
        + std::to_wstring(removed)
        + L" crash artifact(s): "
        + std::filesystem::path(state->capturedCrashDumpPath).filename().wstring());
  }
  state->capturedCrashDumpPath.clear();
  state->pendingCrashViewerDumpPath.clear();
  state->crashCaptured = false;
}

void AppendExitClassificationLog(
  const std::filesystem::path& outBase,
  bool exitCode0StrongCrash,
  std::uint32_t exceptionCode)
{
  if (exitCode0StrongCrash) {
    AppendLogLine(
      outBase,
      L"Process exited with exit_code=0 but crash exception_code="
        + Hex32(exceptionCode)
        + L" is strong; treating as crash for viewer/deferred behavior.");
  } else {
    AppendLogLine(outBase, L"Process exited normally (exit_code=0); skipping crash event drain.");
  }
}

void LaunchDeferredViewersAfterExit(
  const HelperConfig& cfg,
  const std::filesystem::path& outBase,
  DWORD exitCode,
  bool exitCode0StrongCrash,
  HelperLoopState* state)
{
  if (!state) {
    return;
  }

  if (!state->pendingCrashViewerDumpPath.empty() &&
      cfg.autoOpenViewerOnCrash &&
      (exitCode != 0 || exitCode0StrongCrash)) {
    const std::wstring deferredDumpPath = state->pendingCrashViewerDumpPath;
    const auto launch = StartDumpToolViewer(
      cfg,
      deferredDumpPath,
      outBase,
      exitCode0StrongCrash ? L"crash_deferred_exit_code0_strong" : L"crash_deferred_exit");
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
    const std::uint32_t exceptionCode = proc.shm ? proc.shm->header.crash.exception_code : 0u;
    const bool sharedMemoryStrongCrash = (exitCode == 0) && HasSharedMemoryStrongCrashEvidence(proc);
    if (exitCode != 0 || sharedMemoryStrongCrash) {
      DrainCrashEventBeforeExit(cfg, proc, outBase, state);
    }
    const bool exitCode0StrongCrash =
      (exitCode == 0) &&
      (state->crashCaptured || sharedMemoryStrongCrash) &&
      skydiag::helper::IsStrongCrashException(exceptionCode);
    if (exitCode == 0) {
      CleanupCrashArtifactsAfterZeroExit(cfg, proc, outBase, exitCode0StrongCrash, exceptionCode, state);
      AppendExitClassificationLog(outBase, exitCode0StrongCrash, exceptionCode);
    }
    MaybeStopPendingCrashEtwCapture(cfg, proc, outBase, /*force=*/true, &pendingCrashEtw);
    std::wcerr << L"[SkyrimDiagHelper] Target process exited (exit_code=" << exitCode << L").\n";
    AppendLogLine(outBase, L"Target process exited (exit_code=" + std::to_wstring(exitCode) + L").");
    LaunchDeferredViewersAfterExit(cfg, outBase, exitCode, exitCode0StrongCrash, state);
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
