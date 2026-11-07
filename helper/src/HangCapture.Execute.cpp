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
#include "FreezeCapture.h"
#include "HelperCommon.h"
#include "HelperLog.h"
#include "IncidentManifest.h"
#include "SkyrimDiagHelper/HeadlessAnalysisPolicy.h"

namespace skydiag::helper::internal {

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

  FreezeCaptureRequest request{};
  request.kind = "hang";
  request.logName = L"Hang";
  request.profileKind = skydiag::helper::CaptureKind::Hang;
  request.wctPath = wctPath;
  request.dumpPath = dumpPath;
  request.captureModalDialogs = true;
  const auto capture = CaptureFreezeDumpAndWct(cfg, proc, outBase, decision, stateFlags, request);
  if (capture.modalDialogCount != 0u && state) {
    state->modalDialogSeenThisEpisode = true;
  }
  const bool dumpWritten = capture.dumpWritten;

  // The trace starts after the dump, so wpr -start (up to the ETW timeout)
  // does not delay it, and is stopped from the helper loop once it has
  // recorded for a while (MaybeFinalizePendingHangEtwCapture) rather than here.
  bool etwStarted = false;
  std::uint64_t etwStartedAtTick64 = 0;
  if (cfg.enableEtwCaptureOnHang && state && state->pendingEtw.active) {
    AppendLogLine(outBase, L"ETW hang capture not started: the previous hang's trace is still being stopped.");
  } else if (cfg.enableEtwCaptureOnHang) {
    std::wstring etwUsedProfile;
    std::wstring etwErr;
    if (StartEtwCaptureForHang(cfg, outBase, &etwUsedProfile, &etwErr)) {
      etwStarted = true;
      etwStartedAtTick64 = GetTickCount64();
      AppendLogLine(outBase, L"ETW hang capture started (profile=" + etwUsedProfile + L").");
    } else {
      AppendLogLine(outBase, L"ETW hang capture start failed: " + etwErr);
    }
  }
  const std::string etwStatus = cfg.enableEtwCaptureOnHang ? (etwStarted ? "recording" : "start_failed") : "disabled";

  if (dumpWritten) {
    std::wcout << L"[SkyrimDiagHelper] Hang dump written: " << dumpPath << L"\n";
    std::wcout << L"[SkyrimDiagHelper] WCT written: " << wctPath.wstring() << L"\n";
    std::wcout << L"[SkyrimDiagHelper] Hang decision: secondsSinceHeartbeat=" << decision.secondsSinceHeartbeat
               << L" threshold=" << decision.thresholdSec << L" loading=" << (decision.isLoading ? L"1" : L"0") << L"\n";
    AppendLogLine(outBase, L"Hang dump written: " + dumpPath);
    LogDumpObjectMemory(outBase, capture.objectMemory);
    AppendLogLine(outBase, L"WCT written: " + wctPath.wstring());
    AppendLogLine(outBase, L"Hang decision: secondsSinceHeartbeat=" + std::to_wstring(decision.secondsSinceHeartbeat) +
      L" threshold=" + std::to_wstring(decision.thresholdSec) + L" loading=" + std::to_wstring(decision.isLoading ? 1 : 0));
    if (cfg.enableIncidentManifest) {
      const bool inMenu = (stateFlags & skydiag::kState_InMenu) != 0u;
      const nlohmann::json ctx = FreezeManifestContext(decision, inMenu, capture);

      const auto writtenProfile = WrittenDumpProfile(capture.dumpProfile, capture.objectMemory);
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
        &writtenProfile,
        &capture.recapture,
        cfg,
        cfg.incidentManifestIncludeConfigSnapshot);
      WriteTextFileUtf8(manifestPath, DumpJsonForFile(manifest));
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
    const bool analyze = ShouldRunHeadlessDumpAnalysis(cfg, viewerNow, /*analysisRequired=*/false);
    if (!viewerNow && state) {
      // Nothing analyzes it until the hang settles (HeldHangDump): a freeze
      // that lasts until a normal exit is a slow shutdown.
      ReleaseHeldHangDump(cfg, outBase, L"another hang was captured", state);
      state->heldDump = HeldHangDump{ dumpPath, analyze };
      AppendLogLine(
        outBase,
        std::wstring(analyze ? L"Hang dump analysis waits" : L"Hang dump held")
          + L" until the game recovers or exits; a freeze that lasts until a normal exit is a slow shutdown "
            L"and its dump is removed.");
    } else if (analyze) {
      StartDumpToolHeadlessIfConfigured(cfg, dumpPath, outBase);
    } else if (viewerNow && cfg.autoAnalyzeDump) {
      AppendLogLine(outBase, L"Skipped headless analysis: viewer auto-open is enabled.");
    }
  }

  if (etwStarted) {
    PendingHangEtwCapture pending{};
    pending.active = true;
    pending.etwPath = etwPath;
    pending.manifestPath = manifestWritten ? manifestPath : std::filesystem::path{};
    pending.startedAtTick64 = etwStartedAtTick64;
    pending.captureSeconds = HangEtwCaptureSeconds(cfg.etwMaxDurationSec);
    if (state) {
      state->pendingEtw = pending;
    } else {
      MaybeFinalizePendingHangEtwCapture(cfg, outBase, /*force=*/true, &pending);
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

void ReleaseHeldHangDump(
  const skydiag::helper::HelperConfig& cfg,
  const std::filesystem::path& outBase,
  std::wstring_view why,
  HangCaptureState* state)
{
  if (!state || state->heldDump.dumpPath.empty()) {
    return;
  }
  const HeldHangDump held = state->heldDump;
  state->heldDump = HeldHangDump{};
  if (!held.analyze) {
    return;
  }
  AppendLogLine(
    outBase,
    L"Analyzing the held hang dump (" + std::wstring(why) + L"): " +
      std::filesystem::path(held.dumpPath).filename().wstring());
  StartDumpToolHeadlessIfConfigured(cfg, held.dumpPath, outBase);
}

void MaybeFinalizePendingHangEtwCapture(
  const skydiag::helper::HelperConfig& cfg,
  const std::filesystem::path& outBase,
  bool force,
  PendingHangEtwCapture* pending)
{
  constexpr std::uint32_t kMaxHangEtwStopAttempts = 3u;
  if (!pending || !pending->active) {
    return;
  }
  // Until the first stop, the trace is left to record for its window; a
  // stop right after the start would cover almost nothing.
  if (!force && pending->stopAttempts == 0u) {
    const std::uint64_t nowTick64 = GetTickCount64();
    const std::uint64_t windowMs = static_cast<std::uint64_t>(pending->captureSeconds) * 1000u;
    if (nowTick64 >= pending->startedAtTick64 && nowTick64 - pending->startedAtTick64 < windowMs) {
      return;
    }
  }

  std::string etwStatus;
  while (pending->stopAttempts < kMaxHangEtwStopAttempts) {
    std::wstring stopErr;
    if (StopEtwCaptureToPath(cfg, outBase, pending->etwPath, &stopErr)) {
      AppendLogLine(outBase, L"ETW hang capture written: " + pending->etwPath.wstring());
      etwStatus = "written";
      break;
    }
    ++pending->stopAttempts;
    AppendLogLine(
      outBase,
      L"ETW hang capture stop failed (attempt " + std::to_wstring(pending->stopAttempts) + L" of "
        + std::to_wstring(kMaxHangEtwStopAttempts) + L"): " + stopErr);
    if (!force && pending->stopAttempts < kMaxHangEtwStopAttempts) {
      return;  // the next tick tries again
    }
  }
  if (etwStatus.empty()) {
    std::wstring cancelErr;
    if (CancelEtwCapture(cfg, outBase, &cancelErr)) {
      AppendLogLine(outBase, L"ETW hang capture stop failed repeatedly; WPR cancellation was confirmed.");
      etwStatus = "cancelled_after_stop_failure";
    } else {
      AppendLogLine(
        outBase,
        L"ETW hang capture cleanup remains unconfirmed after stop retries and wpr -cancel: " + cancelErr);
      etwStatus = "cleanup_unconfirmed";
    }
  }

  if (!pending->manifestPath.empty()) {
    std::wstring updErr;
    if (!TryUpdateIncidentManifestEtw(pending->manifestPath, pending->etwPath, etwStatus, &updErr)) {
      AppendLogLine(outBase, L"Incident manifest ETW update failed: " + updErr);
    }
  }
  *pending = PendingHangEtwCapture{};
}

}  // namespace skydiag::helper::internal
