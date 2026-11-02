#include "FreezeCapture.h"

#include <Windows.h>

#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "CaptureCommon.h"
#include "HelperCommon.h"
#include "HelperLog.h"
#include "PssSnapshot.h"
#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/HangDetect.h"
#include "SkyrimDiagHelper/ModalDialogProbe.h"
#include "SkyrimDiagHelper/ProcessAttach.h"
#include "SkyrimDiagHelper/WctCapture.h"

namespace skydiag::helper::internal {
namespace {

std::wstring LowerAscii(std::wstring_view s)
{
  std::wstring out(s);
  for (auto& c : out) {
    if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c - L'A' + L'a');
    }
  }
  return out;
}

}  // namespace

FreezeCaptureResult CaptureFreezeDumpAndWct(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  const skydiag::helper::HangDecision& decision,
  std::uint32_t stateFlags,
  const FreezeCaptureRequest& request)
{
  FreezeCaptureResult result{};
  const std::wstring lowerName = LowerAscii(request.logName);

  nlohmann::json wctJson;
  std::wstring wctErr;
  if (!skydiag::helper::CaptureWct(proc.pid, &proc.shm->header.state_flags, wctJson, &wctErr)) {
    std::wcerr << L"[SkyrimDiagHelper] WCT capture failed: " << wctErr << L"\n";
    AppendLogLine(outBase, L"WCT capture failed: " + wctErr);
    wctJson = nlohmann::json::object();
    wctJson["pid"] = proc.pid;
    wctJson["error"] = "capture_failed";
  }
  // A normal (non-admin) user never holds SeDebugPrivilege, so the request
  // ends in ERROR_NOT_ALL_ASSIGNED on every capture. The helper runs as the
  // same user as the game, so the game's own wait chains are still read; only
  // other errors are worth a warning.
  if (wctJson.contains("debugPrivilegeEnabled") && wctJson["debugPrivilegeEnabled"].is_boolean() &&
      !wctJson["debugPrivilegeEnabled"].get<bool>() &&
      wctJson.value("debugPrivilegeError", 0u) != ERROR_NOT_ALL_ASSIGNED) {
    AppendLogLine(
      outBase,
      L"Warning: EnableDebugPrivilege failed (error " + std::to_wstring(wctJson.value("debugPrivilegeError", 0u)) +
        L"); WCT capture may be incomplete.");
  }

  if (request.captureModalDialogs) {
    // A main thread parked in a modal loop (e.g. a plugin's error MessageBox)
    // stops the heartbeat while its stack only shows bystander modules. The
    // visible dialog itself is the symbol-independent evidence for that state.
    const auto modalDialogs = skydiag::helper::CaptureModalDialogs(proc.pid);
    wctJson["modal_dialogs"] = skydiag::helper::ModalDialogsToJson(modalDialogs);
    result.modalDialogCount = modalDialogs.size();
    if (!modalDialogs.empty()) {
      AppendLogLine(
        outBase,
        L"Visible dialog windows at " + lowerName + L" capture: " + std::to_wstring(modalDialogs.size()));
    }
  }

  wctJson["capture"] = nlohmann::json::object();
  wctJson["capture"]["kind"] = request.kind;
  wctJson["capture"]["secondsSinceHeartbeat"] = decision.secondsSinceHeartbeat;
  wctJson["capture"]["thresholdSec"] = decision.thresholdSec;
  wctJson["capture"]["isLoading"] = decision.isLoading;
  wctJson["capture"]["stateFlags"] = stateFlags;

  const std::string pluginScanJson = CollectPluginScanJson(proc, outBase);
  result.dumpProfile = skydiag::helper::ResolveDumpProfile(cfg.dumpMode, request.profileKind, cfg.dumpAllModuleCode);
  auto pssSnapshot = TryCapturePssSnapshotForFreeze(cfg.enablePssSnapshotForFreeze, proc.process);
  if (pssSnapshot.requested) {
    if (pssSnapshot.used) {
      AppendLogLine(
        outBase,
        L"PSS snapshot captured for " + lowerName + L" dump (durationMs="
          + std::to_wstring(pssSnapshot.captureDurationMs)
          + L").");
    } else {
      AppendLogLine(
        outBase,
        L"PSS snapshot unavailable for " + lowerName + L" dump; falling back to live-process dump (status="
          + pssSnapshot.status
          + L").");
    }
  }
  result.pssRequested = pssSnapshot.requested;
  result.pssUsed = pssSnapshot.used;
  result.pssCaptureMs = pssSnapshot.captureDurationMs;
  result.pssStatus = pssSnapshot.status;

  wctJson["capture"]["pss_snapshot_requested"] = pssSnapshot.requested;
  wctJson["capture"]["pss_snapshot_used"] = pssSnapshot.used;
  wctJson["capture"]["pss_snapshot_capture_ms"] = pssSnapshot.captureDurationMs;
  wctJson["capture"]["pss_snapshot_status"] = WideToUtf8(pssSnapshot.status);
  wctJson["capture"]["dump_transport"] = pssSnapshot.used ? "pss_snapshot" : "live_process";

  const bool strongDeadlock = skydiag::helper::WctHasCycle(wctJson);
  const bool snapshotBackedLoaderStall = decision.isLoading && pssSnapshot.used;
  const bool freezeAmbiguous = !strongDeadlock && !snapshotBackedLoaderStall;
  const bool freezeSnapshotFallback = pssSnapshot.requested && !pssSnapshot.used;
  const bool freezeCandidateWeak = decision.isLoading && !pssSnapshot.used;
  result.recapture = skydiag::helper::DecideFreezeRecapture(
    cfg.enableAutoRecaptureOnUnknownCrash,
    cfg.autoAnalyzeDump,
    freezeAmbiguous,
    freezeSnapshotFallback,
    freezeCandidateWeak,
    strongDeadlock,
    snapshotBackedLoaderStall,
    /*bucketSeenCount=*/1u,
    cfg.autoRecaptureUnknownBucketThreshold);

  const std::string wctUtf8 = DumpJsonForFile(wctJson);
  WriteTextFileUtf8(request.wctPath, wctUtf8);

  std::wstring dumpErr;
  const HANDLE dumpSource = pssSnapshot.used ? pssSnapshot.snapshotHandle : proc.process;
  result.dumpWritten = skydiag::helper::WriteDumpWithStreams(
    dumpSource,
    proc.pid,
    request.dumpPath,
    proc.shm,
    proc.shmSize,
    wctUtf8,
    pluginScanJson,
    /*isCrash=*/false,
    result.dumpProfile,
    /*isProcessSnapshot=*/pssSnapshot.used,
    &dumpErr,
    &result.objectMemory);
  ReleasePssSnapshotForFreeze(proc.process, pssSnapshot.snapshotHandle);
  if (!result.dumpWritten) {
    std::wcerr << L"[SkyrimDiagHelper] " << request.logName << L" dump failed: " << dumpErr << L"\n";
    AppendLogLine(outBase, std::wstring(request.logName) + L" dump failed: " + dumpErr);
  }
  return result;
}

nlohmann::json FreezeManifestContext(
  const skydiag::helper::HangDecision& decision,
  bool inMenu,
  const FreezeCaptureResult& capture)
{
  nlohmann::json ctx = nlohmann::json::object();
  ctx["seconds_since_heartbeat"] = decision.secondsSinceHeartbeat;
  ctx["threshold_sec"] = decision.thresholdSec;
  ctx["is_loading"] = decision.isLoading;
  ctx["in_menu"] = inMenu;
  ctx["pss_snapshot_requested"] = capture.pssRequested;
  ctx["pss_snapshot_used"] = capture.pssUsed;
  ctx["pss_snapshot_capture_ms"] = capture.pssCaptureMs;
  ctx["pss_snapshot_status"] = WideToUtf8(capture.pssStatus);
  ctx["dump_transport"] = capture.pssUsed ? "pss_snapshot" : "live_process";
  return ctx;
}

}  // namespace skydiag::helper::internal
