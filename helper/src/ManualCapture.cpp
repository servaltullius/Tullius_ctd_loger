#include "ManualCapture.h"

#include <Windows.h>

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "CaptureCommon.h"
#include "DumpToolLaunch.h"
#include "FreezeCapture.h"
#include "HelperCommon.h"
#include "HelperLog.h"
#include "IncidentManifest.h"
#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/HangDetect.h"
#include "SkyrimDiagHelper/HeadlessAnalysisPolicy.h"
#include "SkyrimDiagHelper/LoadStats.h"
#include "SkyrimDiagHelper/ManualCaptureHotkey.h"
#include "SkyrimDiagHelper/PluginScanner.h"
#include "SkyrimDiagHelper/ProcessAttach.h"
#include "SkyrimDiagShared.h"

namespace skydiag::helper::internal {

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
  const auto thresholds = skydiag::helper::ComputeHangThresholds(
    cfg, stateFlags, loadStats.HasSamples(), adaptiveLoadingThresholdSec);
  const auto decision = skydiag::helper::EvaluateHang(
    static_cast<std::uint64_t>(now.QuadPart),
    proc.shm->header.last_heartbeat_qpc,
    proc.shm->header.qpc_freq,
    stateFlags,
    thresholds.inGameSec,
    thresholds.loadingSec);

  AppendLogLine(outBase, L"Manual capture triggered via " + std::wstring(trigger) +
    L" (secondsSinceHeartbeat=" + std::to_wstring(decision.secondsSinceHeartbeat) +
    L", threshold=" + std::to_wstring(decision.thresholdSec) +
    L", loading=" + std::to_wstring(decision.isLoading ? 1 : 0) +
    L", inMenu=" + std::to_wstring(inMenu ? 1 : 0) + L")");

  // Once per session: the press may also have started Crash Logger's thread
  // dump, which can deadlock the game (the preflight warns about it too).
  static bool s_crashLoggerHotkeyChecked = false;
  if (!s_crashLoggerHotkeyChecked) {
    s_crashLoggerHotkeyChecked = true;
    std::filesystem::path gameExeDir;
    if (skydiag::helper::TryResolveGameExeDir(proc.process, gameExeDir)) {
      const auto crashLoggerHotkey = skydiag::helper::CheckCrashLoggerThreadDumpHotkey(
        gameExeDir / L"Data" / L"SKSE" / L"Plugins", cfg.manualCaptureHotkey);
      if (crashLoggerHotkey.firesOnManualCapture) {
        AppendLogLine(
          outBase,
          L"Warning: " + skydiag::helper::CrashLoggerHotkeyCollisionWarning(crashLoggerHotkey, cfg.manualCaptureHotkey));
      }
    }
  }

  FreezeCaptureRequest request{};
  request.kind = "manual";
  request.logName = L"Manual";
  request.profileKind = skydiag::helper::CaptureKind::Manual;
  request.wctPath = wctPath;
  request.dumpPath = dumpPath;
  const auto capture = CaptureFreezeDumpAndWct(cfg, proc, outBase, decision, stateFlags, request);
  if (!capture.dumpWritten) {
    return;
  }

  std::wcout << L"[SkyrimDiagHelper] Manual dump written: " << dumpPath << L"\n";
  std::wcout << L"[SkyrimDiagHelper] WCT written: " << wctPath.wstring() << L"\n";
  AppendLogLine(outBase, L"Manual dump written: " + dumpPath);
  LogDumpObjectMemory(outBase, capture.objectMemory);
  AppendLogLine(outBase, L"WCT written: " + wctPath.wstring());

  if (cfg.enableIncidentManifest) {
    const auto manifestPath = outBase / (L"SkyrimDiag_Incident_Manual_" + ts + L".json");
    nlohmann::json ctx = FreezeManifestContext(decision, inMenu, capture);
    ctx["trigger"] = WideToUtf8(trigger);

    const auto writtenProfile = WrittenDumpProfile(capture.dumpProfile, capture.objectMemory);
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
      &writtenProfile,
      &capture.recapture,
      cfg,
      cfg.incidentManifestIncludeConfigSnapshot);
    WriteTextFileUtf8(manifestPath, DumpJsonForFile(manifest));
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
}  // namespace skydiag::helper::internal
