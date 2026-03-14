#include "PendingCrashAnalysisInternal.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "CaptureCommon.h"
#include "DumpToolLaunch.h"
#include "HelperLog.h"
#include "IncidentManifest.h"
#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/DumpProfile.h"
#include "SkyrimDiagHelper/DumpWriter.h"
#include "SkyrimDiagHelper/ProcessAttach.h"

namespace skydiag::helper::internal {
namespace {

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

DumpMode DumpModeForRecaptureTarget(
  skydiag::helper::RecaptureTargetProfile targetProfile,
  DumpMode fallbackMode)
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

void ApplyCrashRecaptureDecision(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  const PendingCrashAnalysis& task,
  const PendingCrashRecaptureContext& context)
{
  if (!context.manifestPath.empty() && std::filesystem::exists(context.manifestPath)) {
    std::wstring manifestErr;
    if (!TryUpdateIncidentManifestRecaptureEvaluation(
          context.manifestPath,
          context.recaptureDecision,
          &manifestErr)) {
      AppendLogLine(outBase, L"Incident manifest recapture evaluation update failed: " + manifestErr);
    }
  }

  bool recaptureDumpWritten = false;
  if (context.recaptureDecision.shouldRecapture) {
    std::wstring aliveErr;
    if (IsProcessStillAlive(proc.process, &aliveErr)) {
      const auto tsFull = Timestamp();
      const auto recaptureDumpFs =
        outBase / (L"SkyrimDiag_Crash_" + tsFull +
                   RecaptureSuffixForTarget(context.recaptureDecision.targetProfile) +
                   L".dmp");
      const auto recaptureDumpPath = recaptureDumpFs.wstring();
      const auto recaptureDumpMode =
        DumpModeForRecaptureTarget(context.recaptureDecision.targetProfile, cfg.dumpMode);
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
          skydiag::helper::RecaptureTargetProfileToString(context.recaptureDecision.targetProfile);
        const std::wstring targetProfileW(targetProfileAscii.begin(), targetProfileAscii.end());
        AppendLogLine(
          outBase,
          L"Crash recapture written: " + recaptureDumpPath +
            L" (targetProfile=" + targetProfileW + L")");
        if (cfg.enableIncidentManifest) {
          nlohmann::json ctx = nlohmann::json::object();
          ctx["reason"] = "auto_recapture";
          ctx["source_dump"] = WideToUtf8(std::filesystem::path(task.dumpPath).filename().wstring());
          ctx["source_bucket_key"] = context.summaryInfo.bucketKey;
          ctx["source_summary_schema"] = context.summaryInfo.schemaVersion;
          const auto recaptureManifestPath =
            CrashRecaptureManifestPathForTimestamp(tsFull, outBase);
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
            &context.recaptureDecision,
            cfg,
            cfg.incidentManifestIncludeConfigSnapshot);
          WriteTextFileUtf8(recaptureManifestPath, manifest.dump(2));
          AppendLogLine(
            outBase,
            L"Crash recapture incident manifest written: " + recaptureManifestPath.wstring());
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
}

}  // namespace skydiag::helper::internal
