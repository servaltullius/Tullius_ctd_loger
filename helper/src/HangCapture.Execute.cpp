#include "HangCaptureInternal.h"

#include <Windows.h>

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "CaptureCommon.h"
#include "DumpToolLaunch.h"
#include "EtwCapture.h"
#include "HelperCommon.h"
#include "HelperLog.h"
#include "IncidentManifest.h"
#include "PssSnapshot.h"
#include "SkyrimDiagHelper/DumpWriter.h"
#include "SkyrimDiagHelper/HeadlessAnalysisPolicy.h"
#include "SkyrimDiagHelper/WctCapture.h"

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

HangTickResult ExecuteConfirmedHangCapture(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  const skydiag::helper::HangDecision& decision,
  std::uint32_t stateFlags,
  std::wstring* pendingHangViewerDumpPath,
  HangCaptureState* state)
{
  const auto ts = Timestamp();
  const auto wctPath = outBase / (L"SkyrimDiag_WCT_" + ts + L".json");
  const auto dumpPath = (outBase / (L"SkyrimDiag_Hang_" + ts + L".dmp")).wstring();
  const auto etwPath = outBase / (L"SkyrimDiag_Hang_" + ts + L".etl");
  const auto manifestPath = outBase / (L"SkyrimDiag_Incident_Hang_" + ts + L".json");
  bool manifestWritten = false;

  bool etwStarted = false;
  if (cfg.enableEtwCaptureOnHang) {
    std::wstring etwUsedProfile;
    std::wstring etwErr;
    if (StartEtwCaptureForHang(cfg, outBase, &etwUsedProfile, &etwErr)) {
      etwStarted = true;
      AppendLogLine(outBase, L"ETW hang capture started (profile=" + etwUsedProfile + L").");
    } else {
      AppendLogLine(outBase, L"ETW hang capture start failed: " + etwErr);
    }
  }
  std::string etwStatus = cfg.enableEtwCaptureOnHang ? (etwStarted ? "recording" : "start_failed") : "disabled";

  nlohmann::json wctJson;
  std::wstring wctErr;
  if (!skydiag::helper::CaptureWct(proc.pid, &proc.shm->header.state_flags, wctJson, &wctErr)) {
    std::wcerr << L"[SkyrimDiagHelper] WCT capture failed: " << wctErr << L"\n";
    wctJson = nlohmann::json::object();
    wctJson["pid"] = proc.pid;
    wctJson["error"] = "capture_failed";
  }
  if (wctJson.contains("debugPrivilegeEnabled") && wctJson["debugPrivilegeEnabled"].is_boolean() &&
      !wctJson["debugPrivilegeEnabled"].get<bool>()) {
    AppendLogLine(outBase, L"Warning: EnableDebugPrivilege failed; WCT capture may be incomplete.");
  }

  wctJson["capture"] = nlohmann::json::object();
  wctJson["capture"]["kind"] = "hang";
  wctJson["capture"]["secondsSinceHeartbeat"] = decision.secondsSinceHeartbeat;
  wctJson["capture"]["thresholdSec"] = decision.thresholdSec;
  wctJson["capture"]["isLoading"] = decision.isLoading;
  wctJson["capture"]["stateFlags"] = stateFlags;

  const std::string pluginScanJson = CollectPluginScanJson(proc, outBase);
  const auto dumpProfile = skydiag::helper::ResolveDumpProfile(
    cfg.dumpMode,
    skydiag::helper::CaptureKind::Hang);
  auto pssSnapshot = TryCapturePssSnapshotForFreeze(cfg.enablePssSnapshotForFreeze, proc.process);
  if (pssSnapshot.requested) {
    if (pssSnapshot.used) {
      AppendLogLine(
        outBase,
        L"PSS snapshot captured for hang dump (durationMs="
          + std::to_wstring(pssSnapshot.captureDurationMs)
          + L").");
    } else {
      AppendLogLine(
        outBase,
        L"PSS snapshot unavailable for hang dump; falling back to live-process dump (status="
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

  bool dumpWritten = false;
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
    std::wcerr << L"[SkyrimDiagHelper] Hang dump failed: " << dumpErr << L"\n";
    AppendLogLine(outBase, L"Hang dump failed: " + dumpErr);
  } else {
    ReleasePssSnapshotForFreeze(proc.process, pssSnapshot.snapshotHandle);
    dumpWritten = true;
    std::wcout << L"[SkyrimDiagHelper] Hang dump written: " << dumpPath << L"\n";
    std::wcout << L"[SkyrimDiagHelper] WCT written: " << wctPath.wstring() << L"\n";
    std::wcout << L"[SkyrimDiagHelper] Hang decision: secondsSinceHeartbeat=" << decision.secondsSinceHeartbeat
               << L" threshold=" << decision.thresholdSec << L" loading=" << (decision.isLoading ? L"1" : L"0") << L"\n";
    AppendLogLine(outBase, L"Hang dump written: " + dumpPath);
    AppendLogLine(outBase, L"WCT written: " + wctPath.wstring());
    AppendLogLine(outBase, L"Hang decision: secondsSinceHeartbeat=" + std::to_wstring(decision.secondsSinceHeartbeat) +
      L" threshold=" + std::to_wstring(decision.thresholdSec) + L" loading=" + std::to_wstring(decision.isLoading ? 1 : 0));
    if (cfg.enableIncidentManifest) {
      const bool inMenu = (stateFlags & skydiag::kState_InMenu) != 0u;
      nlohmann::json ctx = nlohmann::json::object();
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
        "hang",
        ts,
        proc.pid,
        std::filesystem::path(dumpPath),
        std::optional<std::filesystem::path>(wctPath),
        etwStarted ? std::optional<std::filesystem::path>(etwPath) : std::nullopt,
        etwStatus,
        stateFlags,
        ctx,
        &dumpProfile,
        &freezeRecaptureDecision,
        cfg,
        cfg.incidentManifestIncludeConfigSnapshot);
      WriteTextFileUtf8(manifestPath, manifest.dump(2));
      AppendLogLine(outBase, L"Incident manifest written: " + manifestPath.wstring());
      manifestWritten = true;
    }

    bool viewerNow = cfg.autoOpenViewerOnHang && !cfg.autoOpenHangAfterProcessExit;
    if (cfg.autoOpenViewerOnHang) {
      if (cfg.autoOpenHangAfterProcessExit) {
        if (pendingHangViewerDumpPath) {
          *pendingHangViewerDumpPath = dumpPath;
        }
        AppendLogLine(outBase, L"Queued hang dump for viewer auto-open on process exit.");
      } else {
        const auto launch = StartDumpToolViewer(cfg, dumpPath, outBase, L"hang");
        viewerNow = (launch == DumpToolViewerLaunchResult::kLaunched);
      }
    }
    if (ShouldRunHeadlessDumpAnalysis(cfg, viewerNow, /*analysisRequired=*/false)) {
      StartDumpToolHeadlessIfConfigured(cfg, dumpPath, outBase);
    } else if (viewerNow && cfg.autoAnalyzeDump) {
      AppendLogLine(outBase, L"Skipped headless analysis: viewer auto-open is enabled.");
    }
  }

  if (etwStarted) {
    std::wstring etwErr;
    if (StopEtwCaptureToPath(cfg, outBase, etwPath, &etwErr)) {
      AppendLogLine(outBase, L"ETW hang capture written: " + etwPath.wstring());
      etwStatus = "written";
      if (manifestWritten) {
        std::wstring updErr;
        if (!TryUpdateIncidentManifestEtw(manifestPath, etwPath, "written", &updErr)) {
          AppendLogLine(outBase, L"Incident manifest ETW update failed: " + updErr);
        }
      }
    } else {
      AppendLogLine(outBase, L"ETW hang capture stop failed: " + etwErr);
      etwStatus = "stop_failed";
      if (manifestWritten) {
        std::wstring updErr;
        if (!TryUpdateIncidentManifestEtw(manifestPath, etwPath, "stop_failed", &updErr)) {
          AppendLogLine(outBase, L"Incident manifest ETW update failed: " + updErr);
        }
      }
    }
  }

  if (dumpWritten) {
    ApplyRetentionFromConfig(cfg, outBase);
  }

  if (state) {
    state->hangCapturedThisEpisode = true;
  }
  return HangTickResult::kContinue;
}

}  // namespace skydiag::helper::internal
