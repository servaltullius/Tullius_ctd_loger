#include "PendingCrashAnalysis.h"

#include <Windows.h>

#include <filesystem>
#include <string>

#include "DumpToolLaunch.h"
#include "HelperCommon.h"
#include "HelperLog.h"
#include "PendingCrashAnalysisInternal.h"
#include "SkyrimDiagHelper/Config.h"

namespace skydiag::helper::internal {

void ClearPendingCrashAnalysis(PendingCrashAnalysis* task)
{
  if (!task) {
    return;
  }
  if (task->process) {
    const DWORD waitState = WaitForSingleObject(task->process, 0);
    if (waitState == WAIT_TIMEOUT) {
      // Ensure no stale headless analyzer process survives across crashes.
      TerminateProcess(task->process, 1);
      WaitForSingleObject(task->process, 1000);
    }
    CloseHandle(task->process);
    task->process = nullptr;
  }
  task->active = false;
  task->dumpPath.clear();
  task->startedAtTick64 = 0;
  task->timeoutMs = 0;
}

bool StartPendingCrashAnalysisTask(
  const skydiag::helper::HelperConfig& cfg,
  const std::wstring& dumpPath,
  const std::filesystem::path& outBase,
  PendingCrashAnalysis* task,
  std::wstring* err)
{
  if (!task) {
    if (err) {
      *err = L"missing pending task state";
    }
    return false;
  }
  if (task->active) {
    ClearPendingCrashAnalysis(task);
  }

  HANDLE processHandle = nullptr;
  if (!StartDumpToolHeadlessAsync(cfg, dumpPath, outBase, &processHandle, err)) {
    return false;
  }
  task->active = true;
  task->dumpPath = dumpPath;
  task->process = processHandle;
  task->startedAtTick64 = GetTickCount64();
  task->timeoutMs = CrashAnalysisTimeoutMs(cfg);
  if (err) {
    err->clear();
  }
  return true;
}

void FinalizePendingCrashAnalysisIfReady(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  PendingCrashAnalysis* task)
{
  if (!task || !task->active || !task->process) {
    return;
  }

  if (task->timeoutMs > 0) {
    const ULONGLONG nowTick = GetTickCount64();
    if (nowTick >= task->startedAtTick64 &&
        (nowTick - task->startedAtTick64) > static_cast<ULONGLONG>(task->timeoutMs)) {
      TerminateProcess(task->process, 1);
      AppendLogLine(outBase, L"Crash headless analysis timeout; process terminated.");
      ClearPendingCrashAnalysis(task);
      return;
    }
  }

  const DWORD w = WaitForSingleObject(task->process, 0);
  if (w == WAIT_TIMEOUT) {
    return;
  }
  if (w == WAIT_FAILED) {
    AppendLogLine(outBase, L"Crash headless analysis wait failed: " + std::to_wstring(GetLastError()));
    ClearPendingCrashAnalysis(task);
    return;
  }

  DWORD exitCode = 0;
  if (!GetExitCodeProcess(task->process, &exitCode)) {
    AppendLogLine(outBase, L"Crash headless analysis exit code read failed: " + std::to_wstring(GetLastError()));
    ClearPendingCrashAnalysis(task);
    return;
  }
  if (exitCode != 0) {
    AppendLogLine(outBase, L"Crash headless analysis finished with non-zero exit code: " + std::to_wstring(exitCode));
    ClearPendingCrashAnalysis(task);
    return;
  }

  PendingCrashRecaptureContext recaptureContext{};
  std::wstring summaryErr;
  if (!TryEvaluateCrashRecapture(cfg, *task, outBase, &recaptureContext, &summaryErr)) {
    AppendLogLine(outBase, L"Crash summary parse failed for recapture policy: " + summaryErr);
    ClearPendingCrashAnalysis(task);
    return;
  }
  const auto& summaryInfo = recaptureContext.summaryInfo;
  const std::wstring bucketW(recaptureContext.summaryInfo.bucketKey.begin(), recaptureContext.summaryInfo.bucketKey.end());
  AppendLogLine(
    outBase,
    L"Crash bucket stats updated: bucket=" + bucketW +
      L", schemaVersion=" + std::to_wstring(summaryInfo.schemaVersion) +
      L", unknownFaultModule=" + std::to_wstring(summaryInfo.unknownFaultModule ? 1 : 0) +
      L", candidateConflict=" + std::to_wstring(summaryInfo.candidateConflict ? 1 : 0) +
      L", referenceClueOnly=" + std::to_wstring(summaryInfo.referenceClueOnly ? 1 : 0) +
      L", stackwalkDegraded=" + std::to_wstring(summaryInfo.stackwalkDegraded ? 1 : 0) +
      L", symbolRuntimeDegraded=" + std::to_wstring(summaryInfo.symbolRuntimeDegraded ? 1 : 0) +
      L", firstChanceCandidateWeak=" + std::to_wstring(summaryInfo.firstChanceCandidateWeak ? 1 : 0) +
      L", bucketSeenCount=" + std::to_wstring(recaptureContext.bucketSeenCount) +
      L", unknownStreak=" + std::to_wstring(recaptureContext.unknownStreak));

  ApplyCrashRecaptureDecision(cfg, proc, outBase, *task, recaptureContext);

  ClearPendingCrashAnalysis(task);
}

}  // namespace skydiag::helper::internal
