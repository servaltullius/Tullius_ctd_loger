#include "ManualCapture.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "CaptureCommon.h"
#include "DumpToolLaunch.h"
#include "HelperCommon.h"
#include "HelperLog.h"
#include "IncidentManifest.h"
#include "PssSnapshot.h"
#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/DumpWriter.h"
#include "SkyrimDiagHelper/HangDetect.h"
#include "SkyrimDiagHelper/HeadlessAnalysisPolicy.h"
#include "SkyrimDiagHelper/LoadStats.h"
#include "SkyrimDiagHelper/ProcessAttach.h"
#include "SkyrimDiagHelper/WctCapture.h"
#include "SkyrimDiagShared.h"

namespace skydiag::helper::internal {

namespace {

bool WctJsonHasCycle(const nlohmann::json& wctJson)
{
  if (!wctJson.is_object() || !wctJson.contains("threads") || !wctJson["threads"].is_array()) {
    return false;
  }
  for (const auto& thread : wctJson["threads"]) {
    if (thread.is_object() && thread.value("isCycle", false)) {
      return true;
    }
  }
  return false;
}

}  // namespace

void DoManualCapture(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  const skydiag::helper::LoadStats& loadStats,
  std::uint32_t adaptiveLoadingThresholdSec,
  std::wstring_view trigger)
{
  const auto ts = Timestamp();
  const auto wctPath = outBase / (L"SkyrimDiag_WCT_Manual_" + ts + L".json");
  const auto dumpPath = (outBase / (L"SkyrimDiag_Manual_" + ts + L".dmp")).wstring();

  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  const auto stateFlags = proc.shm->header.state_flags;
  const bool inMenu = (stateFlags & skydiag::kState_InMenu) != 0u;
  const std::uint32_t inGameThresholdSec = inMenu
    ? std::max(cfg.hangThresholdInGameSec, cfg.hangThresholdInMenuSec)
    : cfg.hangThresholdInGameSec;
  const std::uint32_t loadingThresholdSec = (cfg.enableAdaptiveLoadingThreshold && loadStats.HasSamples())
    ? adaptiveLoadingThresholdSec
    : cfg.hangThresholdLoadingSec;
  const auto decision = skydiag::helper::EvaluateHang(
    static_cast<std::uint64_t>(now.QuadPart),
    proc.shm->header.last_heartbeat_qpc,
    proc.shm->header.qpc_freq,
    stateFlags,
    inGameThresholdSec,
    loadingThresholdSec);

  AppendLogLine(outBase, L"Manual capture triggered via " + std::wstring(trigger) +
    L" (secondsSinceHeartbeat=" + std::to_wstring(decision.secondsSinceHeartbeat) +
    L", threshold=" + std::to_wstring(decision.thresholdSec) +
    L", loading=" + std::to_wstring(decision.isLoading ? 1 : 0) +
    L", inMenu=" + std::to_wstring(inMenu ? 1 : 0) + L")");

  nlohmann::json wctJson;
  std::wstring wctErr;
  if (!skydiag::helper::CaptureWct(proc.pid, wctJson, &wctErr)) {
    std::wcerr << L"[SkyrimDiagHelper] WCT capture failed: " << wctErr << L"\n";
    AppendLogLine(outBase, L"WCT capture failed: " + wctErr);
    wctJson = nlohmann::json::object();
    wctJson["pid"] = proc.pid;
    wctJson["error"] = "capture_failed";
  }
  if (wctJson.contains("debugPrivilegeEnabled") && wctJson["debugPrivilegeEnabled"].is_boolean() &&
      !wctJson["debugPrivilegeEnabled"].get<bool>()) {
    AppendLogLine(outBase, L"Warning: EnableDebugPrivilege failed; WCT capture may be incomplete.");
  }

  wctJson["capture"] = nlohmann::json::object();
  wctJson["capture"]["kind"] = "manual";
  wctJson["capture"]["secondsSinceHeartbeat"] = decision.secondsSinceHeartbeat;
  wctJson["capture"]["thresholdSec"] = decision.thresholdSec;
  wctJson["capture"]["isLoading"] = decision.isLoading;
  wctJson["capture"]["stateFlags"] = stateFlags;

  const std::string pluginScanJson = CollectPluginScanJson(proc, outBase);
  const auto dumpProfile = skydiag::helper::ResolveDumpProfile(
    cfg.dumpMode,
    skydiag::helper::CaptureKind::Manual);
  auto pssSnapshot = TryCapturePssSnapshotForFreeze(cfg.enablePssSnapshotForFreeze, proc.process);
  if (pssSnapshot.requested) {
    if (pssSnapshot.used) {
      AppendLogLine(
        outBase,
        L"PSS snapshot captured for manual dump (durationMs="
          + std::to_wstring(pssSnapshot.captureDurationMs)
          + L").");
    } else {
      AppendLogLine(
        outBase,
        L"PSS snapshot unavailable for manual dump; falling back to live-process dump (status="
          + pssSnapshot.status
          + L").");
    }
  }

  wctJson["capture"]["pss_snapshot_requested"] = pssSnapshot.requested;
  wctJson["capture"]["pss_snapshot_used"] = pssSnapshot.used;
  wctJson["capture"]["pss_snapshot_capture_ms"] = pssSnapshot.captureDurationMs;
  wctJson["capture"]["pss_snapshot_status"] = WideToUtf8(pssSnapshot.status);
  wctJson["capture"]["dump_transport"] = pssSnapshot.used ? "pss_snapshot" : "live_process";

  const bool strongDeadlock = WctJsonHasCycle(wctJson);
  const bool snapshotBackedLoaderStall = decision.isLoading && pssSnapshot.used;
  const bool freezeAmbiguous = !strongDeadlock && !snapshotBackedLoaderStall;
  const bool freezeSnapshotFallback = pssSnapshot.requested && !pssSnapshot.used;
  const bool freezeCandidateWeak = decision.isLoading && !pssSnapshot.used;
  const auto freezeRecaptureDecision = skydiag::helper::DecideFreezeRecapture(
    cfg.enableAutoRecaptureOnUnknownCrash,
    cfg.autoAnalyzeDump,
    freezeAmbiguous,
    freezeSnapshotFallback,
    freezeCandidateWeak,
    strongDeadlock,
    snapshotBackedLoaderStall,
    /*bucketSeenCount=*/1u,
    cfg.autoRecaptureUnknownBucketThreshold);

  const std::string wctUtf8 = wctJson.dump(2);
  WriteTextFileUtf8(wctPath, wctUtf8);

  std::wstring dumpErr;
  const HANDLE dumpSource = pssSnapshot.used ? pssSnapshot.snapshotHandle : proc.process;
  if (!skydiag::helper::WriteDumpWithStreams(
        dumpSource,
        proc.pid,
        dumpPath,
        proc.shm,
        proc.shmSize,
        wctUtf8,
        pluginScanJson,
        /*isCrash=*/false,
        dumpProfile,
        /*isProcessSnapshot=*/pssSnapshot.used,
        &dumpErr)) {
    ReleasePssSnapshotForFreeze(proc.process, pssSnapshot.snapshotHandle);
    std::wcerr << L"[SkyrimDiagHelper] Manual dump failed: " << dumpErr << L"\n";
    AppendLogLine(outBase, L"Manual dump failed: " + dumpErr);
  } else {
    ReleasePssSnapshotForFreeze(proc.process, pssSnapshot.snapshotHandle);
    std::wcout << L"[SkyrimDiagHelper] Manual dump written: " << dumpPath << L"\n";
    std::wcout << L"[SkyrimDiagHelper] WCT written: " << wctPath.wstring() << L"\n";
    AppendLogLine(outBase, L"Manual dump written: " + dumpPath);
    AppendLogLine(outBase, L"WCT written: " + wctPath.wstring());

    if (cfg.enableIncidentManifest) {
      const auto manifestPath = outBase / (L"SkyrimDiag_Incident_Manual_" + ts + L".json");
      nlohmann::json ctx = nlohmann::json::object();
      ctx["trigger"] = WideToUtf8(trigger);
      ctx["seconds_since_heartbeat"] = decision.secondsSinceHeartbeat;
      ctx["threshold_sec"] = decision.thresholdSec;
      ctx["is_loading"] = decision.isLoading;
      ctx["in_menu"] = inMenu;
      ctx["pss_snapshot_requested"] = pssSnapshot.requested;
      ctx["pss_snapshot_used"] = pssSnapshot.used;
      ctx["pss_snapshot_capture_ms"] = pssSnapshot.captureDurationMs;
      ctx["pss_snapshot_status"] = WideToUtf8(pssSnapshot.status);
      ctx["dump_transport"] = pssSnapshot.used ? "pss_snapshot" : "live_process";

      const auto manifest = MakeIncidentManifestV1(
        "manual",
        ts,
        proc.pid,
        std::filesystem::path(dumpPath),
        std::optional<std::filesystem::path>(wctPath),
        /*etwPath=*/std::nullopt,
        /*etwStatus=*/"disabled",
        stateFlags,
        ctx,
        &dumpProfile,
        &freezeRecaptureDecision,
        cfg,
        cfg.incidentManifestIncludeConfigSnapshot);
      WriteTextFileUtf8(manifestPath, manifest.dump(2));
      AppendLogLine(outBase, L"Incident manifest written: " + manifestPath.wstring());
    }

    bool viewerNow = cfg.autoOpenViewerOnManualCapture;
    if (viewerNow) {
      const auto launch = StartDumpToolViewer(cfg, dumpPath, outBase, L"manual");
      viewerNow = (launch == DumpToolViewerLaunchResult::kLaunched);
    }
    if (ShouldRunHeadlessDumpAnalysis(cfg, viewerNow, /*analysisRequired=*/false)) {
      StartDumpToolHeadlessIfConfigured(cfg, dumpPath, outBase);
    } else if (viewerNow && cfg.autoAnalyzeDump) {
      AppendLogLine(outBase, L"Skipped headless analysis: viewer auto-open is enabled.");
    }
    ApplyRetentionFromConfig(cfg, outBase);
  }
}

}  // namespace skydiag::helper::internal
