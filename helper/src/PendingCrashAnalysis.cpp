#include "PendingCrashAnalysis.h"

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include "DumpToolLaunch.h"
#include "HelperCommon.h"
#include "HelperLog.h"
#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/CrashRecapturePolicy.h"
#include "SkyrimDiagHelper/DumpWriter.h"
#include "SkyrimDiagHelper/ProcessAttach.h"

namespace skydiag::helper::internal {
namespace {

DWORD CrashAnalysisTimeoutMs(const skydiag::helper::HelperConfig& cfg)
{
  std::uint32_t sec = cfg.autoRecaptureAnalysisTimeoutSec;
  if (sec < 5u) {
    sec = 5u;
  }
  if (sec > 180u) {
    sec = 180u;
  }
  return static_cast<DWORD>(sec * 1000u);
}

bool IsProcessStillAlive(HANDLE process, std::wstring* err)
{
  if (!process) {
    if (err) {
      *err = L"missing process handle";
    }
    return false;
  }
  const DWORD w = WaitForSingleObject(process, 0);
  if (w == WAIT_TIMEOUT) {
    if (err) {
      err->clear();
    }
    return true;
  }
  if (w == WAIT_OBJECT_0) {
    if (err) {
      *err = L"process already exited";
    }
    return false;
  }
  const DWORD le = GetLastError();
  if (err) {
    *err = L"WaitForSingleObject failed: " + std::to_wstring(le);
  }
  return false;
}

}  // namespace

void ClearPendingCrashAnalysis(PendingCrashAnalysis* task)
{
  if (!task) {
    return;
  }
  if (task->process) {
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

  const auto summaryPath = SummaryPathForDump(task->dumpPath, outBase);
  CrashSummaryInfo summaryInfo{};
  std::wstring summaryErr;
  if (!TryLoadCrashSummaryInfo(summaryPath, &summaryInfo, &summaryErr)) {
    AppendLogLine(outBase, L"Crash summary parse failed for recapture policy: " + summaryErr);
    ClearPendingCrashAnalysis(task);
    return;
  }
  if (summaryInfo.bucketKey.empty()) {
    AppendLogLine(outBase, L"Crash summary has empty crash_bucket_key; recapture policy skipped.");
    ClearPendingCrashAnalysis(task);
    return;
  }

  std::uint32_t unknownStreak = 0;
  std::wstring statsErr;
  if (!UpdateCrashBucketStats(outBase, summaryInfo, &unknownStreak, &statsErr)) {
    AppendLogLine(outBase, L"Crash bucket stats update failed: " + statsErr);
    ClearPendingCrashAnalysis(task);
    return;
  }

  const std::wstring bucketW(summaryInfo.bucketKey.begin(), summaryInfo.bucketKey.end());
  AppendLogLine(
    outBase,
    L"Crash bucket stats updated: bucket=" + bucketW +
      L", schemaVersion=" + std::to_wstring(summaryInfo.schemaVersion) +
      L", unknownFaultModule=" + std::to_wstring(summaryInfo.unknownFaultModule ? 1 : 0) +
      L", unknownStreak=" + std::to_wstring(unknownStreak));

  const auto recaptureDecision = skydiag::helper::DecideCrashFullRecapture(
    cfg.enableAutoRecaptureOnUnknownCrash,
    cfg.autoAnalyzeDump,
    summaryInfo.unknownFaultModule,
    unknownStreak,
    cfg.autoRecaptureUnknownBucketThreshold,
    cfg.dumpMode);
  if (recaptureDecision.shouldRecaptureFullDump) {
    std::wstring aliveErr;
    if (IsProcessStillAlive(proc.process, &aliveErr)) {
      const auto tsFull = Timestamp();
      const auto fullDumpPath = (outBase / (L"SkyrimDiag_Crash_" + tsFull + L"_Full.dmp")).wstring();
      std::wstring fullDumpErr;
      if (!skydiag::helper::WriteDumpWithStreams(
            proc.process,
            proc.pid,
            fullDumpPath,
            proc.shm,
            proc.shmSize,
            /*wctJsonUtf8=*/{},
            /*isCrash=*/true,
            skydiag::helper::DumpMode::kFull,
            &fullDumpErr)) {
        AppendLogLine(outBase, L"Crash full recapture failed: " + fullDumpErr);
      } else {
        AppendLogLine(outBase, L"Crash full recapture written: " + fullDumpPath);
        StartDumpToolHeadlessIfConfigured(cfg, fullDumpPath, outBase);
      }
    } else {
      AppendLogLine(outBase, L"Crash full recapture skipped: " + aliveErr);
    }
  }

  ClearPendingCrashAnalysis(task);
}

}  // namespace skydiag::helper::internal

