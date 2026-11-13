#include "HelperMainInternal.h"
#include "SkyrimDiagLongPath.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "CaptureCommon.h"
#include "CrashCapture.h"
#include "DumpToolLaunch.h"
#include "HelperCommon.h"
#include "HelperLog.h"
#include "IncidentManifest.h"
#include "SkyrimDiagHelper/HangDetect.h"
#include "SkyrimDiagHelper/HeadlessAnalysisPolicy.h"
#include "SkyrimDiagHelper/Retention.h"
#include "SkyrimDiagShared.h"

namespace skydiag::helper::internal {
namespace {

struct HeartbeatAtExit
{
  bool known = false;
  bool stalled = false;
  double secondsSince = 0.0;
};

// Whether the heartbeat had already stopped long enough to count as a hang
// when the exit was observed, with HandleHangTick's thresholds (the configured
// loading threshold stands in for the adaptive one).
HeartbeatAtExit ReadHeartbeatAtExit(const HelperConfig& cfg, const AttachedProcess& proc)
{
  HeartbeatAtExit result{};
  if (!proc.shm || proc.shm->header.last_heartbeat_qpc == 0 || proc.shm->header.qpc_freq == 0) {
    return result;
  }
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  const std::uint32_t stateFlags = proc.shm->header.state_flags;
  const auto thresholds = skydiag::helper::ComputeHangThresholds(
    cfg, stateFlags, /*haveLoadingSamples=*/false, /*adaptiveLoadingThresholdSec=*/0u);
  const auto decision = skydiag::helper::EvaluateHang(
    static_cast<std::uint64_t>(now.QuadPart),
    proc.shm->header.last_heartbeat_qpc,
    proc.shm->header.qpc_freq,
    stateFlags,
    thresholds.inGameSec,
    thresholds.loadingSec);
  result.known = true;
  result.stalled = decision.isHang;
  result.secondsSince = decision.secondsSinceHeartbeat;
  return result;
}

// Crash Logger writes a crash it reports to crash-<time>.log (a thread dump
// goes to threaddump-<time>.log) and only then ends the game, so the file is
// last written after the crash event it published. The first one in `dirs`
// last written at or after `sinceFileTimeUtc` (FILETIME, UTC).
std::optional<std::filesystem::path> FindCrashLoggerCrashLogWrittenSince(
  const std::vector<std::filesystem::path>& dirs,
  std::uint64_t sinceFileTimeUtc)
{
  for (const auto& dir : dirs) {
    WIN32_FIND_DATAW found{};
    const HANDLE find = FindFirstFileExW(
      (dir / L"crash-*.log").c_str(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) {
      continue;
    }
    do {
      const std::uint64_t lastWrite =
        (static_cast<std::uint64_t>(found.ftLastWriteTime.dwHighDateTime) << 32) | found.ftLastWriteTime.dwLowDateTime;
      if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && lastWrite >= sinceFileTimeUtc) {
        FindClose(find);
        return dir / found.cFileName;
      }
    } while (FindNextFileW(find, &found));
    FindClose(find);
  }
  return std::nullopt;
}

// Slack for the crash log's time check: file times and the event time come
// from the same clock, so this only absorbs timestamp granularity.
inline constexpr std::uint64_t kCrashLogTimeSlack100ns = 5ull * 10'000'000ull;

// A released crash that the exit path reports after all, whose dump was held
// since the release (CrashCaptureState::releasedDumpPath): that dump becomes
// the crash's dump, with the crash viewer or analysis a captured crash gets.
// False when no held dump is left.
bool PromoteReleasedCrashDump(
  const HelperConfig& cfg,
  const std::filesystem::path& outBase,
  const CrashEventInfo& info,
  HelperLoopState* state)
{
  auto& crash = state->crashCaptured;
  if (crash.releasedDumpPath.empty()) {
    return false;
  }
  std::wstring dumpPath = crash.releasedDumpPath;
  const auto releasedContext = std::move(crash.releasedDumpContext);
  crash.releasedDumpPath.clear();
  crash.releasedDumpExceptionCode = 0;
  crash.releasedDumpExceptionAddr = 0;
  crash.releasedDumpPreserve = false;
  crash.releasedDumpRecovery = LateRecoveryState{};
  crash.releasedDumpContext.reset();
  std::error_code ec;
  const bool exists = ExistsForIo(dumpPath, ec);
  if (ec) {
    // Held open or locked is not gone: report it, and the viewer says if it
    // cannot be read.
    AppendLogLine(
      outBase,
      L"Could not check the dump held since the release (error " + std::to_wstring(ec.value()) +
        L"); reporting it as it is: " + std::filesystem::path(dumpPath).filename().wstring());
  } else if (!exists) {
    AppendLogLine(outBase, L"The dump held since the release is gone: " + std::filesystem::path(dumpPath).filename().wstring());
    return false;
  }
  // Held as SkyrimDiag_Released_<ts>.dmp: it is a crash dump again.
  if (const std::wstring crashPath = CrashDumpPathForReleased(dumpPath); crashPath != dumpPath) {
    if (MoveFileExW(dumpPath.c_str(), crashPath.c_str(), 0)) {
      dumpPath = crashPath;
    } else {
      AppendLogLine(
        outBase,
        L"Could not give the released crash's dump its crash name (err=" + std::to_wstring(GetLastError())
          + L"); reporting it as it is.");
    }
  }

  crash.latched = true;
  crash.capturedInfo = info;
  state->capturedCrashDumpPath = dumpPath;
  AppendLogLine(
    outBase,
    L"Its dump was kept since the release; reporting it as the crash dump: "
      + std::filesystem::path(dumpPath).filename().wstring());
  // A crash dump carries no plugin stream; the analyzer reads the scan from
  // this file beside it (plugin list, missing masters, SKSE log, MO2 mods).
  // The game is gone now, so the scan uses what was read at the release.
  if (releasedContext) {
    const std::string pluginScanJson = CollectPluginScanJson(releasedContext->pluginScan, outBase);
    if (!pluginScanJson.empty()) {
      const std::filesystem::path dumpFs(dumpPath);
      const auto pluginScanPath = dumpFs.parent_path() / (dumpFs.stem().wstring() + L"_PluginScan.json");
      WriteTextFileUtf8(pluginScanPath, pluginScanJson);
      AppendLogLine(outBase, L"Plugin scan sidecar written: " + pluginScanPath.wstring());
    }
    // As a kept crash gets (ProcessValidCrashDump).
    WriteGpuDriverEventsSidecar(outBase, std::filesystem::path(dumpPath), releasedContext->pid);
  }
  // Its incident manifest, as a kept crash gets one: the analyzer reads the
  // capture profile from it. No ETW trace was started for a released
  // exception.
  if (cfg.enableIncidentManifest && releasedContext && !releasedContext->timestamp.empty()) {
    nlohmann::json ctx = nlohmann::json::object();
    ctx["reason"] = "released_exception_reported_at_exit";
    if (crash.releasedForcedClose.matched()) {
      // As ProcessValidCrashDump marks it, for retention.
      ctx["forced_close"] = true;
    }
    const auto manifest = MakeIncidentManifestV1(
      "crash",
      releasedContext->timestamp,
      releasedContext->pid,
      std::filesystem::path(dumpPath),
      std::nullopt,
      std::nullopt,
      /*etwStatus=*/"disabled",
      releasedContext->stateFlags,
      ctx,
      &releasedContext->writtenProfile,
      /*recaptureDecision=*/nullptr,
      cfg,
      cfg.incidentManifestIncludeConfigSnapshot);
    const auto manifestPath = outBase / (L"SkyrimDiag_Incident_Crash_" + releasedContext->timestamp + L".json");
    WriteTextFileUtf8(manifestPath, DumpJsonForFile(manifest));
    AppendLogLine(outBase, L"Incident manifest written: " + manifestPath.wstring());
  }
  // The game has exited, so a queued viewer opens right after this
  // (LaunchDeferredViewersAfterExit) and analyzes the dump itself.
  // ADR-0022: no viewer for an error while the game was being closed.
  if (cfg.autoOpenViewerOnCrash && crash.releasedForcedClose.matched()) {
    AppendLogLine(outBase, L"Skipped viewer auto-open: " + DescribeForcedCloseForLog(crash.releasedForcedClose) + L".");
  }
  const bool viewerQueued = cfg.autoOpenViewerOnCrash && !crash.releasedForcedClose.matched() &&
    QueueDeferredCrashViewer(dumpPath, &state->pendingCrashViewerDumpPath);
  if (ShouldRunHeadlessDumpAnalysis(cfg, viewerQueued, /*analysisRequired=*/false)) {
    std::wstring analyzeErr;
    if (StartPendingCrashAnalysisTask(cfg, dumpPath, outBase, &state->pendingCrashAnalysis, &analyzeErr)) {
      AppendLogLine(outBase, L"Crash headless analysis queued for the released crash's dump.");
    } else {
      AppendLogLine(outBase, L"Crash headless analysis queue failed: " + analyzeErr);
    }
  } else if (viewerQueued) {
    // Nothing else analyzes it: LaunchDeferredViewersAfterExit starts the
    // analysis itself when the viewer does not open.
    state->promotedDumpAwaitsViewer = true;
    AppendLogLine(outBase, L"Queued the released crash's dump for the crash viewer.");
  }
  return true;
}

}  // namespace

bool DrainCrashEventBeforeExit(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase,
  DWORD exitCode,
  HelperLoopState* state)
{
  if (!state) {
    return false;
  }

  const bool processExited =
    proc.process && WaitForSingleObject(proc.process, 0) == WAIT_OBJECT_0;
  if (!processExited) {
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
    return false;
  }

  if (proc.crashEvent && WaitForSingleObject(proc.crashEvent, 0) == WAIT_OBJECT_0) {
    if (!ResetEvent(proc.crashEvent)) {
      AppendLogLine(
        outBase,
        L"Failed to reset post-exit crash event: " + std::to_wstring(GetLastError()));
    }
  }
  if (state->crashCaptured.latched) {
    return true;
  }

  CrashEventInfo info{};
  if (!TryCaptureCommittedCrashInfo(proc.shm ? &proc.shm->header : nullptr, &info)) {
    return false;
  }
  // A handled exception released earlier (the game kept running, or exited 0
  // during the filter) keeps its committed record; it is not a crash at exit,
  // unless the release was weak evidence and the exit says otherwise.
  if (state->crashCaptured.releasedCrashSeq == info.crashSeq) {
    const auto& crash = state->crashCaptured;
    const std::uint64_t nowTick64 = GetTickCount64();
    // A frozen game that was closed, not a crash handler that ended it.
    const bool frozeAfterRelease = state->hangState.hangCapturedThisEpisode ||
      exitCode == kExitCodeClosedAsNotResponding || ReadHeartbeatAtExit(cfg, proc).stalled;
    if (!ShouldReportReleasedCrashAtExit(
          exitCode,
          info.exceptionCode,
          crash.releasedOnBriefHeartbeat,
          frozeAfterRelease,
          crash.releasedCrashTick64,
          nowTick64)) {
      AppendLogLine(
        outBase,
        L"Committed crash_seq=" + std::to_wstring(info.crashSeq)
          + L" was already released as a handled exception; not re-reporting it at process exit (exit_code="
          + std::to_wstring(exitCode)
          + (frozeAfterRelease ? L", the game had stopped responding since" : L"") + L").");
      DiscardReleasedCrashDump(outBase, &state->crashCaptured, L"the exit did not report its exception");
      return false;
    }
    const auto secondsSinceRelease =
      nowTick64 >= crash.releasedCrashTick64 ? (nowTick64 - crash.releasedCrashTick64) / 1000u : 0u;
    // Released early in Crash Logger's thread dump window: a thread dump of a
    // soft-locked game (its main loop still ticking) that was then ended in
    // Task Manager (exit code 1, as Crash Logger's) looks the same in
    // heartbeat and exit code as a worker's crash that Crash Logger ended.
    // Only Crash Logger's crash log tells them apart.
    std::wstring crashLogEvidence;
    if (crash.releasedInThreadDumpWindow && exitCode != info.exceptionCode) {
      const std::uint64_t since = crash.releasedCrashEventFileTimeUtc > kCrashLogTimeSlack100ns
        ? crash.releasedCrashEventFileTimeUtc - kCrashLogTimeSlack100ns
        : 0u;
      const auto crashLog = FindCrashLoggerCrashLogWrittenSince(state->crashLoggerLogDirs, since);
      if (!crashLog) {
        AppendLogLine(
          outBase,
          L"Committed crash_seq=" + std::to_wstring(info.crashSeq)
            + L" was released in Crash Logger's thread dump window, and the game then exited (exit_code="
            + std::to_wstring(exitCode) + L", " + std::to_wstring(secondsSinceRelease)
            + L" s after the release), but no Crash Logger crash log (crash-*.log) written since was found; not "
              L"recording it. "
              L"This is most likely a thread dump of a stuck game that was then ended. If the game crashed "
              L"instead, Crash Logger's crash-*.log from this time has the details.");
        return false;
      }
      crashLogEvidence = L", and Crash Logger wrote " + crashLog->filename().wstring();
    }
    AppendLogLine(
      outBase,
      L"Committed crash_seq=" + std::to_wstring(info.crashSeq)
        + L" was released as a handled exception, but the game then exited abnormally (exit_code="
        + std::to_wstring(exitCode) + L", " + std::to_wstring(secondsSinceRelease) + L" s after the release"
        + crashLogEvidence + L"); recording it after all.");
    if (PromoteReleasedCrashDump(cfg, outBase, info, state)) {
      return true;
    }
    if (state->postExitEvidenceSeq != info.crashSeq &&
        TryWritePostExitCrashEvidenceRecord(
          outBase, info, exitCode, PostExitEvidenceReason::kReleasedFaultFollowedByAbnormalExit)) {
      state->postExitEvidenceSeq = info.crashSeq;
    }
    return true;
  }

  const bool sameCapturedSequence =
    state->crashCaptured.capturedInfo.crashSeq == info.crashSeq;
  if (exitCode == 0u) {
    if (!info.isStrong) {
      return false;
    }
    const bool priorEvidenceAttempt =
      state->crashCaptured.cleanExitEvidenceWritten ||
      state->crashCaptured.cleanExitEvidenceFinalized ||
      !state->crashCaptured.cleanExitEvidencePath.empty();
    if (sameCapturedSequence && priorEvidenceAttempt) {
      AppendLogLine(
        outBase,
        L"Preserving the existing clean-exit evidence attempt for crash_seq="
          + std::to_wstring(info.crashSeq)
          + L"; post-exit drain will not relabel it as not_captured.");
      return true;
    }

    state->crashCaptured.latched = true;
    state->crashCaptured.capturedInfo = info;
    state->crashCaptured.cleanExitEvidenceWritten = false;
    state->crashCaptured.cleanExitEvidenceFinalized = false;
    state->crashCaptured.cleanExitEvidencePath.clear();
    state->crashCaptured.cleanExitDumpIdentity = CleanExitDumpIdentity{};
    state->crashCaptured.cleanExitFilterContext = L"process_exit_metadata_drain";
    AppendLogLine(
      outBase,
      L"Drained committed strong-fault metadata after process exit (exit_code=0, crash_seq="
        + std::to_wstring(info.crashSeq)
        + L"); the process address space is already gone, so dump capture was not attempted.");
    return true;
  }

  auto& crashState = state->crashCaptured;
  const std::uint64_t exitTick64 = GetTickCount64();
  // The dump of this exception could not be written while the game was
  // alive, and the game then ran on for a minute or more: it handled the
  // exception, as with one the filters release. Its record is still the
  // newest at the exit, but this exit is not its crash.
  if (crashState.dumpWriteFailedCrashSeq == info.crashSeq && !crashState.dumpWriteFailedProcessExited &&
      crashState.dumpWriteFailedTick64 != 0u && exitTick64 >= crashState.dumpWriteFailedTick64 &&
      exitTick64 - crashState.dumpWriteFailedTick64 >= kLateRecoveryMinAliveMs) {
    AppendLogLine(
      outBase,
      L"Committed crash_seq=" + std::to_wstring(info.crashSeq) + L", whose dump could not be written, is "
        + std::to_wstring((exitTick64 - crashState.dumpWriteFailedTick64) / 1000u)
        + L" s old and the game kept running after it; not reporting it at process exit (exit_code="
        + std::to_wstring(exitCode) + L").");
    crashState.dumpWriteFailedCrashSeq = 0u;
    return false;
  }
  // The newest crash has no dump (its write failed), but an exception this
  // game raised moments before left one, held since it was released on a few
  // seconds of heartbeat: the same dying game, and the only dump of it.
  if (crashState.dumpWriteFailedCrashSeq == info.crashSeq && !crashState.releasedDumpPath.empty() &&
      exitTick64 >= crashState.releasedCrashTick64 &&
      exitTick64 - crashState.releasedCrashTick64 < kLateRecoveryMinAliveMs) {
    AppendLogLine(
      outBase,
      L"The dump of crash_seq=" + std::to_wstring(info.crashSeq) + L" could not be written; reporting the dump kept "
        L"from the exception released " + std::to_wstring((exitTick64 - crashState.releasedCrashTick64) / 1000u)
        + L" s earlier (crash_seq=" + std::to_wstring(crashState.releasedCrashSeq) + L") as this crash's.");
    (void)PromoteReleasedCrashDump(cfg, outBase, info, state);
  }

  if (state->postExitEvidenceSeq == info.crashSeq) {
    return true;
  }
  auto reason = PostExitEvidenceReason::kPublishedAfterFinalLivePoll;
  if (state->crashCaptured.dumpWriteFailedCrashSeq == info.crashSeq) {
    reason = state->crashCaptured.dumpWriteFailedProcessExited
      ? PostExitEvidenceReason::kDumpWriteFailedProcessExited
      : PostExitEvidenceReason::kDumpWriteFailed;
  }
  if (TryWritePostExitCrashEvidenceRecord(outBase, info, exitCode, reason)) {
    state->postExitEvidenceSeq = info.crashSeq;
  }
  return true;
}

// Releases a latched crash capture that turned out not to be a CTD: the
// process exited with code 0, or it kept running (HandleLateRecoveryTick).
// `context` is the clean-exit evidence filter context; `logPrefix` names the
// trigger in SkyrimDiagHelper.log.
void CleanupFilteredCrashArtifacts(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase,
  HelperLoopState* state,
  std::wstring_view context,
  std::wstring_view logPrefix,
  bool keepDump)
{
  if (!state) {
    return;
  }
  if (!state->crashCaptured.latched) {
    return;
  }

  if (state->capturedCrashDumpPath.empty() && !state->pendingCrashViewerDumpPath.empty()) {
    state->capturedCrashDumpPath = state->pendingCrashViewerDumpPath;
  }
  const std::filesystem::path capturedDumpPath(state->capturedCrashDumpPath);

  // Preserve the capture-time fault metadata before terminating analysis or
  // deleting any filtered artifacts. When deletion is intended, commit a
  // pending record first and finalize it only after observing the filesystem.
  const bool evidenceRequired = IsCleanExitEvidenceRequired(cfg, &state->crashCaptured);
  const bool preserveRequested = cfg.preserveFilteredCrashDumps || keepDump;
  bool preserveDump = preserveRequested;
  bool pendingDeleteRecorded = false;
  bool dumpDeletionFailed = false;
  if (evidenceRequired && capturedDumpPath.empty()) {
    (void)TryWriteCleanExitEvidenceRecord(
      cfg,
      outBase,
      &state->crashCaptured,
      L"process_exit_metadata_drain",
      {},
      CleanExitDumpState::kNotCaptured);
  } else if (evidenceRequired && preserveRequested) {
    (void)TryWriteCleanExitEvidenceRecord(
      cfg,
      outBase,
      &state->crashCaptured,
      context,
      capturedDumpPath,
      CleanExitDumpState::kPreserved);
  } else if (evidenceRequired) {
    pendingDeleteRecorded = TryWriteCleanExitEvidenceRecord(
      cfg,
      outBase,
      &state->crashCaptured,
      context,
      capturedDumpPath,
      CleanExitDumpState::kPendingDelete);
    // Without a durable pending record, keep the dump as the fail-safe.
    preserveDump = ShouldPreserveFilteredDump(preserveRequested, evidenceRequired, pendingDeleteRecorded);
  }

  if (state->pendingCrashAnalysis.active) {
    if (state->pendingCrashAnalysis.process) {
      if (!TerminateProcess(state->pendingCrashAnalysis.process, 1)) {
        AppendLogLine(
          outBase,
          std::wstring(logPrefix) + L" after crash capture; failed to terminate pending crash analysis process: "
            + std::to_wstring(GetLastError()));
      } else {
        AppendLogLine(outBase, std::wstring(logPrefix) + L" after crash capture; terminated pending crash analysis process.");
      }
    }
    ClearPendingCrashAnalysis(&state->pendingCrashAnalysis);
  }

  const std::filesystem::path crashEtwPath = state->pendingCrashEtw.etwPath;
  MaybeStopPendingCrashEtwCapture(cfg, proc, outBase, /*force=*/true, &state->pendingCrashEtw);
  const std::filesystem::path removableCrashEtwPath =
    state->pendingCrashEtw.active ? std::filesystem::path{} : crashEtwPath;
  if (state->pendingCrashEtw.active && !crashEtwPath.empty()) {
    AppendLogLine(
      outBase,
      L"Keeping ETW path out of artifact deletion because WPR cleanup is still unconfirmed: "
        + crashEtwPath.filename().wstring());
  }

  if (!state->capturedCrashDumpPath.empty()) {
    const auto removal = RemoveCrashArtifactsForDump(
      outBase,
      state->capturedCrashDumpPath,
      removableCrashEtwPath,
      preserveDump);
    if (!preserveDump && removal.dumpExistsAfter) {
      dumpDeletionFailed = true;
      preserveDump = true;
    }
    if (evidenceRequired && pendingDeleteRecorded) {
      const auto finalState = removal.dumpExistsAfter
        ? CleanExitDumpState::kDeleteFailed
        : CleanExitDumpState::kDiscarded;
      if (!TryWriteCleanExitEvidenceRecord(
            cfg,
            outBase,
            &state->crashCaptured,
            context,
            capturedDumpPath,
            finalState)) {
        AppendLogLine(
          outBase,
          L"Clean-exit evidence finalization failed; the durable pending_delete record "
          L"does not claim an unobserved dump state.");
      }
      preserveDump = removal.dumpExistsAfter;
    }
    if (preserveDump) {
      AppendLogLine(
        outBase,
        std::wstring(logPrefix)
          + (preserveRequested
               ? L" after filtered crash; dump file preserved, removed "
               : (dumpDeletionFailed
                    ? L" after filtered crash; dump deletion failed or was not verifiable, removed "
                    : L" clean-exit evidence write failed; dump preserved as a fail-safe, removed "))
          + std::to_wstring(removal.removedCount)
          + L" derived crash artifact(s), without keeping the crashCaptured latch "
          + (cfg.preserveFilteredCrashDumps ? L"(PreserveFilteredCrashDumps=1): "
                                            : (keepDump ? L"(kept for review): " : L": "))
          + std::filesystem::path(state->capturedCrashDumpPath).filename().wstring());
    } else {
      AppendLogLine(
        outBase,
        std::wstring(logPrefix) + L" after crash capture; removed "
          + std::to_wstring(removal.removedCount)
          + L" crash artifact(s): "
          + std::filesystem::path(state->capturedCrashDumpPath).filename().wstring());
    }
  }
  if (!state->crashRecapture.dump.empty()) {
    // Its own headless analysis is not tracked: a report it finishes after
    // this can still appear without its dump.
    const auto removal = RemoveCrashArtifactsForDump(
      outBase,
      state->crashRecapture.dump.wstring(),
      state->crashRecapture.manifest,
      preserveDump);
    AppendLogLine(
      outBase,
      std::wstring(logPrefix) + L" after filtered crash; "
        + (preserveDump ? L"recapture dump preserved, removed " : L"removed recapture dump and ")
        + std::to_wstring(removal.removedCount) + L" artifact(s): "
        + state->crashRecapture.dump.filename().wstring());
    state->crashRecapture = CrashRecaptureArtifacts{};
  }
  state->capturedCrashDumpPath.clear();
  state->pendingCrashViewerDumpPath.clear();
  state->crashCaptured.latched = false;
}

void CleanupCrashArtifactsAfterZeroExit(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase,
  HelperLoopState* state)
{
  // ADR-0032: a crash whose late recovery was held back (the plugin reported
  // it as not handled) would have kept its dump had the recovery run; an exit
  // with code 0 after that keeps it too. This runs for it only once the plugin
  // took the signal back: with the signal still set the exit tick keeps the
  // whole crash (IsNormalExitForCapturedCrash).
  const bool lateRecoveryWasWithheld = state && state->crashCaptured.latched &&
    state->crashCaptured.lateRecoveryWithheldCrashSeq != 0u &&
    state->crashCaptured.lateRecoveryWithheldCrashSeq == state->crashCaptured.capturedInfo.crashSeq;
  CleanupFilteredCrashArtifacts(
    cfg, proc, outBase, state, L"process_exit", L"exit_code=0", /*keepDump=*/lateRecoveryWasWithheld);
}

void ReleasePendingCrashAnalysisAtShutdown(const std::filesystem::path& outBase, HelperLoopState* state)
{
  if (!state || !state->pendingCrashAnalysis.active) {
    return;
  }
  // The game exited after a crash (a zero exit already stopped the
  // analyzer). Stopping it now would lose the report when no viewer runs
  // (AutoOpenViewerOnCrash=0): let it finish, unless the viewer opened at
  // exit is analyzing the same dump.
  if (state->crashViewerLaunchedAtExit) {
    AppendLogLine(outBase, L"Helper shutting down; the crash viewer analyzes the same dump, so the tracked analyzer is stopped.");
    ClearPendingCrashAnalysis(&state->pendingCrashAnalysis);
  } else {
    AppendLogLine(outBase, L"Helper shutting down while crash analysis is still running; leaving it to finish the report.");
    DetachPendingCrashAnalysis(&state->pendingCrashAnalysis);
  }
}

bool HandleLateRecoveryTick(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase,
  HelperLoopState* state)
{
  if (!state || !state->crashCaptured.latched || !proc.process || !proc.shm) {
    return false;
  }
  if (WaitForSingleObject(proc.process, 0) != WAIT_TIMEOUT) {
    return false;  // the exit tick classifies an exited process
  }
  auto& crash = state->crashCaptured;
  if (!AdvanceLateRecovery(&crash.lateRecovery, GetTickCount64(), proc.shm->header.last_heartbeat_qpc)) {
    return false;
  }

  const std::uint32_t crashSeq = crash.capturedInfo.crashSeq;
  if (IsCapturedCrashStillUnhandled(crash.latched, IsUnhandledExceptionSignaled(proc.unhandledEvent))) {
    // ADR-0032: the heartbeat is the main thread's; the faulting thread is
    // still in the filters that end the process.
    if (crash.lateRecoveryWithheldCrashSeq != crashSeq) {
      crash.lateRecoveryWithheldCrashSeq = crashSeq;
      AppendLogLine(
        outBase,
        L"The game is still running with the heartbeat advancing, but the plugin reported this exception as not "
        L"handled and no filter has resumed it (crash_seq=" + std::to_wstring(crashSeq)
          + L"): a crash handler is still at work on the faulting thread. Keeping it as a crash.");
    }
    return false;
  }

  const auto keptForSec = crash.lateRecovery.lastSampleTick64 >= crash.lateRecovery.latchedTick64
    ? (crash.lateRecovery.lastSampleTick64 - crash.lateRecovery.latchedTick64) / 1000u
    : 0u;
  AppendLogLine(
    outBase,
    L"Game kept running " + std::to_wstring(keptForSec)
      + L"s after a crash was captured, with the heartbeat advancing; the exception was handled "
        L"(crash_seq=" + std::to_wstring(crashSeq) + L"). Releasing the capture.");
  // The heartbeat proves the main thread runs, not that the faulting thread
  // recovered: a worker blocked for a minute in a crash handler would look
  // the same. So the dump is kept (marked late_recovery) instead of deleted;
  // only the crash actions and the freeze are released.
  CleanupFilteredCrashArtifacts(cfg, proc, outBase, state, L"late_recovery", L"game kept running", /*keepDump=*/true);
  // A later exit must not re-report this released exception.
  crash.releasedCrashSeq = crashSeq;
  crash.releasedCrashTick64 = GetTickCount64();
  crash.releasedOnBriefHeartbeat = false;
  crash.releasedInThreadDumpWindow = false;
  const bool thawed = ThawCrashCapture(proc, crashSeq);
  AppendLogLine(
    outBase,
    thawed
      ? L"Late-recovered exception thawed blackbox/resource capture for a later real CTD."
      : L"Late-recovered exception was not thawed because a newer crash record is already present.");
  crash.lateRecovery = LateRecoveryState{};
  return true;
}

void ExpireReleasedCrashDumpIfOutlived(
  const AttachedProcess& proc,
  const std::filesystem::path& outBase,
  HelperLoopState* state)
{
  if (!state || state->crashCaptured.releasedDumpPath.empty() || !proc.process || !proc.shm) {
    return;
  }
  if (WaitForSingleObject(proc.process, 0) != WAIT_TIMEOUT) {
    return;  // the exit tick settles it
  }
  if (AdvanceLateRecovery(
        &state->crashCaptured.releasedDumpRecovery, GetTickCount64(), proc.shm->header.last_heartbeat_qpc)) {
    DiscardReleasedCrashDump(
      outBase, &state->crashCaptured, L"the game kept running for over a minute after its exception");
  }
}

void AppendExitClassificationLog(const std::filesystem::path& outBase, bool hadCapturedCrash)
{
  // Without a captured exception there is nothing to classify; the next
  // line already says the process exited.
  if (!hadCapturedCrash) {
    return;
  }
  AppendLogLine(
    outBase,
    L"Process exited normally (exit_code=0); treating the captured first-chance exception as handled "
    L"and suppressing crash viewer/deferred behavior.");
}

namespace {

// A hang viewer queued for the game's exit (AutoOpenHangAfterProcessExit).
bool HangViewerQueuedForExit(const HelperConfig& cfg, const HelperLoopState& state)
{
  return !state.pendingHangViewerDumpPath.empty() && cfg.autoOpenViewerOnHang && cfg.autoOpenHangAfterProcessExit;
}

// Whether LaunchDeferredViewersAfterExit opens the queued hang viewer: not for
// a slow shutdown. The one place that decides it, so a held dump left to the
// viewer is never left to one that does not open.
bool HangViewerOpensAtExit(const HelperConfig& cfg, const HelperLoopState& state)
{
  return HangViewerQueuedForExit(cfg, state) && !state.slowShutdownAtExit;
}

// HeldHangDump at the game's exit. A slow shutdown (IsSlowShutdownAtExit,
// state->slowShutdownAtExit): its dump goes, with what was written
// for it, before any analysis puts it in crash_history.json. Any other held
// dump is analyzed, by the exit viewer when one opens for it.
void SettleHeldHangDumpAtExit(
  const HelperConfig& cfg,
  const std::filesystem::path& outBase,
  HelperLoopState* state)
{
  auto& hang = state->hangState;
  if (hang.heldDump.dumpPath.empty()) {
    return;
  }
  if (state->slowShutdownAtExit && !cfg.keepSlowShutdownHangDumps) {
    // Its ETW trace is written now, so it is removed with the dump.
    MaybeFinalizePendingHangEtwCapture(cfg, outBase, /*force=*/true, &hang.pendingEtw);
    const std::filesystem::path dump(hang.heldDump.dumpPath);
    hang.heldDump = HeldHangDump{};
    if (state->pendingHangViewerDumpPath == dump.wstring()) {
      state->pendingHangViewerDumpPath.clear();
    }
    const std::string name = PathPartUtf8(dump.filename());
    const auto ts = TryExtractTimestampToken(name);
    bool removed = false;
    if (ts) {
      std::unordered_map<std::string, std::uint32_t> tsRefs{ { *ts, 1u } };
      RemoveDumpAndArtifacts(
        dump.parent_path(), DatedFile{ dump, *ts }, kHangDumpFiles.prefix, tsRefs, kHangDumpFiles.deleteWctForTimestamp,
        kHangDumpFiles.deleteManualWctForTimestamp, kHangDumpFiles.deleteEtlForStem);
      std::error_code ec;
      removed = !ExistsForIo(dump, ec) && !ec;
    }
    AppendLogLine(
      outBase,
      removed
        ? L"Removed the hang dump of a slow shutdown: the game was still not responding when it exited normally "
          L"(exit_code=0), so it was closing, not frozen (KeepSlowShutdownHangDumps=1 keeps it): "
            + dump.filename().wstring()
        : L"Could not remove the hang dump of a slow shutdown (exit_code=0); it stays unanalyzed: "
            + dump.filename().wstring());
    return;
  }
  if (HangViewerOpensAtExit(cfg, *state) && state->pendingHangViewerDumpPath == hang.heldDump.dumpPath) {
    return;  // LaunchDeferredViewersAfterExit hands it to the viewer
  }
  ReleaseHeldHangDump(
    cfg, outBase,
    state->slowShutdownAtExit ? L"a slow shutdown, KeepSlowShutdownHangDumps=1" : L"the game exited", &hang);
}

}  // namespace

void SettleHeldHangDumpAfterExitViewer(
  const HelperConfig& cfg,
  const std::filesystem::path& outBase,
  bool viewerLaunched,
  HelperLoopState* state)
{
  if (!state || state->hangState.heldDump.dumpPath.empty() ||
      state->hangState.heldDump.dumpPath != state->pendingHangViewerDumpPath) {
    return;
  }
  if (viewerLaunched) {
    state->hangState.heldDump = HeldHangDump{};  // the viewer analyzes it
    return;
  }
  ReleaseHeldHangDump(cfg, outBase, L"the hang viewer did not open", &state->hangState);
}

void LaunchDeferredViewersAfterExit(
  const HelperConfig& cfg,
  const std::filesystem::path& outBase,
  DWORD exitCode,
  bool capturedCrashStillUnhandled,
  HelperLoopState* state)
{
  if (!state) {
    return;
  }

  // ADR-0032: a crash kept through an exit with code 0 gets its viewer too.
  const bool normalExit = IsNormalExitForCapturedCrash(exitCode, capturedCrashStillUnhandled);
  if (!state->pendingCrashViewerDumpPath.empty() &&
      cfg.autoOpenViewerOnCrash &&
      !normalExit) {
    const std::wstring deferredDumpPath = state->pendingCrashViewerDumpPath;
    const auto launch = StartDumpToolViewer(
      cfg,
      deferredDumpPath,
      outBase,
      L"crash_deferred_exit");
    state->crashViewerLaunchedAtExit = launch == DumpToolViewerLaunchResult::kLaunched;
    if (launch != DumpToolViewerLaunchResult::kLaunched && state->promotedDumpAwaitsViewer) {
      // A promoted dump was left to this viewer; without it there would be no report.
      AppendLogLine(outBase, L"The crash viewer did not open; starting the analysis of the released crash's dump.");
      StartDumpToolHeadlessIfConfigured(cfg, deferredDumpPath, outBase);
    }
    state->promotedDumpAwaitsViewer = false;
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
  } else if (!state->pendingCrashViewerDumpPath.empty() && cfg.autoOpenViewerOnCrash && normalExit) {
    AppendLogLine(
      outBase,
      L"Suppressed deferred crash viewer launch on normal process exit (exit_code=0, dump="
        + std::filesystem::path(state->pendingCrashViewerDumpPath).filename().wstring()
        + L").");
    state->pendingCrashViewerDumpPath.clear();
  }

  if (HangViewerQueuedForExit(cfg, *state) && !HangViewerOpensAtExit(cfg, *state)) {
    AppendLogLine(
      outBase,
      L"Suppressed hang viewer launch: the game was still not responding when it exited normally (exit_code=0), "
      L"a slow shutdown rather than a freeze; the hang dump is kept: "
        + std::filesystem::path(state->pendingHangViewerDumpPath).filename().wstring());
  } else if (HangViewerOpensAtExit(cfg, *state)) {
    const DWORD delayMs = static_cast<DWORD>(std::min<std::uint32_t>(cfg.autoOpenHangDelayMs, 10000u));
    if (delayMs > 0) {
      Sleep(delayMs);
    }
    const auto launch = StartDumpToolViewer(cfg, state->pendingHangViewerDumpPath, outBase, L"hang_exit");
    const bool launched = launch == DumpToolViewerLaunchResult::kLaunched;
    AppendLogLine(
      outBase,
      launched ? L"Auto-opened DumpTool viewer for latest hang dump after process exit."
               : L"Hang viewer auto-open attempt failed after process exit.");
    SettleHeldHangDumpAfterExitViewer(cfg, outBase, launched, state);
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
  DrainCrashEventBeforeExit(cfg, proc, outBase, STILL_ACTIVE, state);
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
    if (!GetExitCodeProcess(proc.process, &exitCode)) {
      AppendLogLine(
        outBase,
        L"GetExitCodeProcess failed after process signal (error="
          + std::to_wstring(GetLastError())
          + L"); treating exit code as unavailable/non-zero.");
      exitCode = STILL_ACTIVE;
    }
    // ADR-0032: read before the drain, which latches a record nobody captured.
    // A captured crash the plugin still reports as not handled stays a crash
    // whatever the exit code is.
    const bool capturedCrashStillUnhandled = IsCapturedCrashStillUnhandled(
      state->crashCaptured.latched, IsUnhandledExceptionSignaled(proc.unhandledEvent));
    // Always drain the committed crash generation after observing process exit.
    // This is metadata-only because MiniDumpWriteDump cannot read a dead process.
    const bool crashOnRecord = DrainCrashEventBeforeExit(cfg, proc, outBase, exitCode, state);
    // A dump held for a released crash that the drain did not report (or
    // whose record is gone) is not needed any more.
    DiscardReleasedCrashDump(outBase, &state->crashCaptured, L"the game exited without reporting its exception");
    if (exitCode != 0 && !state->crashCaptured.latched) {
      const auto& crash = state->crashCaptured;
      const auto& hang = state->hangState;
      const auto heartbeat = ReadHeartbeatAtExit(cfg, proc);
      if (crash.dumpWriteFailedCrashSeq != 0u) {
        // The hint written when that dump failed is the guidance that applies.
        if (cfg.enableWerDumpFallbackHint) {
          AppendLogLine(
            outBase,
            L"Abnormal process exit (exit_code=" + std::to_wstring(exitCode) + L") after crash_seq="
              + std::to_wstring(crash.dumpWriteFailedCrashSeq)
              + L", whose dump could not be written; keeping its WER LocalDumps hint.");
        }
      } else if (crashOnRecord) {
        // The drain just recorded a fault as post-exit metadata, so the hint
        // for an exit without a recorded exception does not apply.
        if (cfg.enableWerDumpFallbackHint) {
          AppendLogLine(
            outBase,
            L"Abnormal process exit (exit_code=" + std::to_wstring(exitCode)
              + L") with a crash on record as post-exit metadata; no abnormal-exit WER LocalDumps hint.");
        }
      } else if (hang.hangSkippedAsClosingThisEpisode && exitCode < 0xC0000000u) {
        // The hang check had seen the game in its exit (IsGameProcessClosing),
        // so this was no freeze during play. An NTSTATUS exit code (a crash
        // WER can capture) still gets the hint below.
        std::wstring evidence;
        if (heartbeat.known) {
          evidence += L", no heartbeat for " + std::to_wstring(static_cast<std::uint64_t>(heartbeat.secondsSince)) + L" s";
        }
        AppendLogLine(
          outBase,
          L"The game exited while it was still closing (exit_code=" + std::to_wstring(exitCode) + evidence
            + L"; the hang check had seen one thread left). Either it was ended during a stalled exit (Task "
              L"Manager or a watchdog), or a crash handler ended a crash in the exit: Crash Logger ends a crash "
              L"it reports with exit code 1. Check for a Crash Logger crash-*.log from this time. Not a freeze "
              L"during play; no WER LocalDumps hint, since WER captures neither case.");
      } else if (IsCloseOfUnresponsiveGame(
                   exitCode,
                   crashOnRecord,
                   hang.hangCapturedThisEpisode,
                   hang.modalDialogSeenThisEpisode,
                   heartbeat.stalled,
                   // Each suppression saw the window answering (the grace one
                   // only follows a not-foreground suppression).
                   hang.hangSuppressedNotForegroundThisEpisode || hang.hangSuppressedForegroundGraceThisEpisode ||
                     hang.hangSuppressedForegroundResponsiveThisEpisode)) {
        std::wstring evidence;
        if (heartbeat.known) {
          evidence += L", no heartbeat for "
            + std::to_wstring(static_cast<std::uint64_t>(heartbeat.secondsSince)) + L" s";
        }
        if (hang.hangCapturedThisEpisode) {
          evidence += L", hang captured";
        }
        if (exitCode == kExitCodeClosedAsNotResponding) {
          evidence += L", closed by Windows as not responding";
          AppendLogLine(
            outBase,
            L"The game was closed while not responding (exit_code=" + std::to_wstring(exitCode) + evidence
              + L") with no unhandled crash on record; no WER LocalDumps hint, since WER cannot capture a "
                L"frozen game that is ended.");
        } else {
          // Crash Logger ends a crash it reports with exit code 1, also one
          // SkyrimDiag does not record, so this may have been a crash.
          AppendLogLine(
            outBase,
            L"The game stopped responding and then exited (exit_code=" + std::to_wstring(exitCode) + evidence
              + L") with no crash SkyrimDiag recorded. Either it was closed (Task Manager or a watchdog), or a "
                L"crash handler ended it: Crash Logger ends a crash it reports with exit code 1, also an "
                L"exception SkyrimDiag does not record. Check for a Crash Logger crash-*.log from this time. "
                L"No WER LocalDumps hint, since WER captures neither case.");
        }
      } else if (cfg.enableWerDumpFallbackHint) {
        WriteWerFallbackHint(outBase, WerFallbackReason::kAbnormalExitWithoutCrash, exitCode);
        AppendLogLine(
          outBase,
          L"Abnormal process exit without a recorded crash (no dump was attempted); wrote WER LocalDumps fallback guidance: "
            + (outBase / L"SkyrimDiag_WER_LocalDumps_Hint.txt").wstring());
      }
    }
    if (IsNormalExitForCapturedCrash(exitCode, capturedCrashStillUnhandled)) {
      const bool hadCapturedCrash = state->crashCaptured.latched;
      CleanupCrashArtifactsAfterZeroExit(cfg, proc, outBase, state);
      AppendExitClassificationLog(outBase, hadCapturedCrash);
    } else if (exitCode == 0) {
      AppendLogLine(
        outBase,
        L"The game exited with exit_code=0, but the plugin had reported the captured exception as not handled and "
        L"no filter resumed it (crash_seq=" + std::to_wstring(state->crashCaptured.capturedInfo.crashSeq)
          + L"): a filter, or the game closing on another thread, ended the process with that code while the "
            L"faulting thread was in the exception filters. Keeping it as a crash.");
    }
    MaybeStopPendingCrashEtwCapture(cfg, proc, outBase, /*force=*/true, &pendingCrashEtw);
    std::wcerr << L"[SkyrimDiagHelper] Target process exited (exit_code=" << exitCode << L").\n";
    AppendLogLine(outBase, L"Target process exited (exit_code=" + std::to_wstring(exitCode) + L").");
    {
      // Stalled by the threshold the hang was captured at.
      const auto& hang = state->hangState;
      const auto heartbeat = ReadHeartbeatAtExit(cfg, proc);
      const bool stalledSinceCapture = heartbeat.known && hang.captureThresholdSecThisEpisode != 0u &&
        heartbeat.secondsSince >= static_cast<double>(hang.captureThresholdSecThisEpisode);
      state->slowShutdownAtExit = IsSlowShutdownAtExit(
        exitCode,
        hang.hangCapturedThisEpisode,
        hang.captureMayBeSlowShutdownThisEpisode || hang.closingSeenAfterCaptureThisEpisode,
        stalledSinceCapture,
        hang.modalDialogSeenThisEpisode);
    }
    SettleHeldHangDumpAtExit(cfg, outBase, state);
    LaunchDeferredViewersAfterExit(cfg, outBase, exitCode, capturedCrashStillUnhandled, state);
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
