#include "PendingCrashAnalysis.h"

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include "CaptureCommon.h"
#include "DumpToolLaunch.h"
#include "HelperCommon.h"
#include "HelperLog.h"
#include "IncidentManifest.h"
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

std::filesystem::path CrashManifestPathForDump(const std::wstring& dumpPath, const std::filesystem::path& outBase)
{
  const auto dumpFs = std::filesystem::path(dumpPath);
  const auto stem = dumpFs.stem().wstring();
  constexpr wchar_t kPrefix[] = L"SkyrimDiag_Crash_";
  if (stem.rfind(kPrefix, 0) != 0) {
    return {};
  }
  const std::wstring suffix = stem.substr((sizeof(kPrefix) / sizeof(kPrefix[0])) - 1u);
  return outBase / (L"SkyrimDiag_Incident_Crash_" + suffix + L".json");
}

DumpMode DumpModeForRecaptureTarget(skydiag::helper::RecaptureTargetProfile targetProfile, DumpMode fallbackMode)
{
  switch (targetProfile) {
    case skydiag::helper::RecaptureTargetProfile::kCrashRicher:
      return DumpMode::kDefault;
    case skydiag::helper::RecaptureTargetProfile::kCrashFull:
      return DumpMode::kFull;
    case skydiag::helper::RecaptureTargetProfile::kNone:
    case skydiag::helper::RecaptureTargetProfile::kFreezeSnapshotRicher:
      return fallbackMode;
  }
  return fallbackMode;
}

std::wstring RecaptureSuffixForTarget(skydiag::helper::RecaptureTargetProfile targetProfile)
{
  switch (targetProfile) {
    case skydiag::helper::RecaptureTargetProfile::kCrashRicher:
      return L"_Richer";
    case skydiag::helper::RecaptureTargetProfile::kCrashFull:
      return L"_Full";
    case skydiag::helper::RecaptureTargetProfile::kNone:
    case skydiag::helper::RecaptureTargetProfile::kFreezeSnapshotRicher:
      return L"_Recapture";
  }
  return L"_Recapture";
}

std::filesystem::path CrashRecaptureManifestPathForTimestamp(
  std::wstring_view timestamp,
  const std::filesystem::path& outBase)
{
  return outBase / (L"SkyrimDiag_Incident_CrashRecapture_" + std::wstring(timestamp) + L".json");
}

}  // namespace

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
  std::uint32_t bucketSeenCount = 0;
  std::wstring statsErr;
  if (!UpdateCrashBucketStats(outBase, summaryInfo, &unknownStreak, &bucketSeenCount, &statsErr)) {
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
      L", candidateConflict=" + std::to_wstring(summaryInfo.candidateConflict ? 1 : 0) +
      L", referenceClueOnly=" + std::to_wstring(summaryInfo.referenceClueOnly ? 1 : 0) +
      L", stackwalkDegraded=" + std::to_wstring(summaryInfo.stackwalkDegraded ? 1 : 0) +
      L", symbolRuntimeDegraded=" + std::to_wstring(summaryInfo.symbolRuntimeDegraded ? 1 : 0) +
      L", firstChanceCandidateWeak=" + std::to_wstring(summaryInfo.firstChanceCandidateWeak ? 1 : 0) +
      L", bucketSeenCount=" + std::to_wstring(bucketSeenCount) +
      L", unknownStreak=" + std::to_wstring(unknownStreak));

  const auto recaptureDecision = skydiag::helper::DecideCrashRecapture(
    cfg.enableAutoRecaptureOnUnknownCrash,
    cfg.autoAnalyzeDump,
    summaryInfo.unknownFaultModule,
    unknownStreak,
    bucketSeenCount,
    cfg.autoRecaptureUnknownBucketThreshold,
    summaryInfo.candidateConflict,
    summaryInfo.referenceClueOnly,
    summaryInfo.stackwalkDegraded,
    summaryInfo.symbolRuntimeDegraded,
    summaryInfo.firstChanceCandidateWeak,
    cfg.dumpMode);

  const auto manifestPath = CrashManifestPathForDump(task->dumpPath, outBase);
  if (!manifestPath.empty() && std::filesystem::exists(manifestPath)) {
    std::wstring manifestErr;
    if (!TryUpdateIncidentManifestRecaptureEvaluation(manifestPath, recaptureDecision, &manifestErr)) {
      AppendLogLine(outBase, L"Incident manifest recapture evaluation update failed: " + manifestErr);
    }
  }

  bool recaptureDumpWritten = false;
  if (recaptureDecision.shouldRecapture) {
    std::wstring aliveErr;
    if (IsProcessStillAlive(proc.process, &aliveErr)) {
      const auto tsFull = Timestamp();
      const auto recaptureDumpFs =
        outBase / (L"SkyrimDiag_Crash_" + tsFull + RecaptureSuffixForTarget(recaptureDecision.targetProfile) + L".dmp");
      const auto recaptureDumpPath = recaptureDumpFs.wstring();
      const auto recaptureDumpMode = DumpModeForRecaptureTarget(recaptureDecision.targetProfile, cfg.dumpMode);
      const auto dumpProfile = skydiag::helper::ResolveDumpProfile(
        recaptureDumpMode,
        skydiag::helper::CaptureKind::CrashRecapture);

      const std::string pluginScanJson = CollectPluginScanJson(
        proc,
        outBase,
        L"PluginScanner skipped for full recapture: failed to resolve game exe directory.");

      std::wstring fullDumpErr;
      if (!skydiag::helper::WriteDumpWithStreams(
            proc.process,
            proc.pid,
            recaptureDumpPath,
            proc.shm,
            proc.shmSize,
            /*wctJsonUtf8=*/{},
            pluginScanJson,
            /*isCrash=*/true,
            dumpProfile,
            /*isProcessSnapshot=*/false,
            &fullDumpErr)) {
        AppendLogLine(outBase, L"Crash recapture failed: " + fullDumpErr);
      } else {
        recaptureDumpWritten = true;
        const std::string targetProfileAscii =
          skydiag::helper::RecaptureTargetProfileToString(recaptureDecision.targetProfile);
        const std::wstring targetProfileW(targetProfileAscii.begin(), targetProfileAscii.end());
        AppendLogLine(
          outBase,
          L"Crash recapture written: " + recaptureDumpPath +
            L" (targetProfile=" + targetProfileW + L")");
        if (cfg.enableIncidentManifest) {
          nlohmann::json ctx = nlohmann::json::object();
          ctx["reason"] = "auto_recapture";
          ctx["source_dump"] = WideToUtf8(std::filesystem::path(task->dumpPath).filename().wstring());
          ctx["source_bucket_key"] = summaryInfo.bucketKey;
          ctx["source_summary_schema"] = summaryInfo.schemaVersion;
          const auto recaptureManifestPath = CrashRecaptureManifestPathForTimestamp(tsFull, outBase);
          const auto manifest = MakeIncidentManifestV1(
            "crash_recapture",
            tsFull,
            proc.pid,
            recaptureDumpFs,
            std::nullopt,
            std::nullopt,
            /*etwStatus=*/"disabled",
            /*stateFlags=*/0u,
            ctx,
            &dumpProfile,
            &recaptureDecision,
            cfg,
            cfg.incidentManifestIncludeConfigSnapshot);
          WriteTextFileUtf8(recaptureManifestPath, manifest.dump(2));
          AppendLogLine(outBase, L"Crash recapture incident manifest written: " + recaptureManifestPath.wstring());
        }
        StartDumpToolHeadlessIfConfigured(cfg, recaptureDumpPath, outBase);
      }
    } else {
      AppendLogLine(outBase, L"Crash recapture skipped: " + aliveErr);
    }
  }

  if (recaptureDumpWritten) {
    ApplyRetentionFromConfig(cfg, outBase);
  }

  ClearPendingCrashAnalysis(task);
}

}  // namespace skydiag::helper::internal
