#include "HelperMainInternal.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "CrashCapture.h"
#include "DumpToolLaunch.h"
#include "HelperLog.h"
#include "SkyrimDiagHelper/HangDetect.h"
#include "SkyrimDiagHelper/HeadlessAnalysisPolicy.h"
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
  const auto decision = skydiag::helper::EvaluateHang(
    static_cast<std::uint64_t>(now.QuadPart),
    proc.shm->header.last_heartbeat_qpc,
    proc.shm->header.qpc_freq,
    stateFlags,
    (stateFlags & skydiag::kState_InMenu) != 0u
      ? std::max(cfg.hangThresholdInGameSec, cfg.hangThresholdInMenuSec)
      : cfg.hangThresholdInGameSec,
    cfg.hangThresholdLoadingSec);
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
  const std::wstring dumpPath = crash.releasedDumpPath;
  crash.releasedDumpPath.clear();
  crash.releasedDumpPreserve = false;
  crash.releasedDumpRecovery = LateRecoveryState{};
  std::error_code ec;
  if (!std::filesystem::exists(dumpPath, ec) || ec) {
    AppendLogLine(outBase, L"The dump held since the release is gone: " + std::filesystem::path(dumpPath).filename().wstring());
    return false;
  }

  crash.latched = true;
  crash.capturedInfo = info;
  state->capturedCrashDumpPath = dumpPath;
  AppendLogLine(
    outBase,
    L"Its dump was kept since the release; reporting it as the crash dump: "
      + std::filesystem::path(dumpPath).filename().wstring());
  // The game has exited, so a queued viewer opens right after this
  // (LaunchDeferredViewersAfterExit) and analyzes the dump itself.
  const bool viewerQueued = cfg.autoOpenViewerOnCrash && QueueDeferredCrashViewer(dumpPath, &state->pendingCrashViewerDumpPath);
  if (ShouldRunHeadlessDumpAnalysis(cfg, viewerQueued, /*analysisRequired=*/false)) {
    std::wstring analyzeErr;
    if (StartPendingCrashAnalysisTask(cfg, dumpPath, outBase, &state->pendingCrashAnalysis, &analyzeErr)) {
      AppendLogLine(outBase, L"Crash headless analysis queued for the released crash's dump.");
    } else {
      AppendLogLine(outBase, L"Crash headless analysis queue failed: " + analyzeErr);
    }
  } else if (viewerQueued) {
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
    if (!pendingDeleteRecorded) {
      preserveDump = true;
    }
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
  CleanupFilteredCrashArtifacts(cfg, proc, outBase, state, L"process_exit", L"exit_code=0", /*keepDump=*/false);
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
  const bool thawed = TryClearRecoveredCrashFreeze(proc.shmWritable, crashSeq);
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

void LaunchDeferredViewersAfterExit(
  const HelperConfig& cfg,
  const std::filesystem::path& outBase,
  DWORD exitCode,
  HelperLoopState* state)
{
  if (!state) {
    return;
  }

  if (!state->pendingCrashViewerDumpPath.empty() &&
      cfg.autoOpenViewerOnCrash &&
      exitCode != 0) {
    const std::wstring deferredDumpPath = state->pendingCrashViewerDumpPath;
    const auto launch = StartDumpToolViewer(
      cfg,
      deferredDumpPath,
      outBase,
      L"crash_deferred_exit");
    state->crashViewerLaunchedAtExit = launch == DumpToolViewerLaunchResult::kLaunched;
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
  } else if (!state->pendingCrashViewerDumpPath.empty() && cfg.autoOpenViewerOnCrash && exitCode == 0) {
    AppendLogLine(
      outBase,
      L"Suppressed deferred crash viewer launch on normal process exit (exit_code=0, dump="
        + std::filesystem::path(state->pendingCrashViewerDumpPath).filename().wstring()
        + L").");
    state->pendingCrashViewerDumpPath.clear();
  }

  if (!state->pendingHangViewerDumpPath.empty() && cfg.autoOpenViewerOnHang && cfg.autoOpenHangAfterProcessExit) {
    const DWORD delayMs = static_cast<DWORD>(std::min<std::uint32_t>(cfg.autoOpenHangDelayMs, 10000u));
    if (delayMs > 0) {
      Sleep(delayMs);
    }
    const auto launch = StartDumpToolViewer(cfg, state->pendingHangViewerDumpPath, outBase, L"hang_exit");
    if (launch == DumpToolViewerLaunchResult::kLaunched) {
      AppendLogLine(outBase, L"Auto-opened DumpTool viewer for latest hang dump after process exit.");
    } else {
      AppendLogLine(outBase, L"Hang viewer auto-open attempt failed after process exit.");
    }
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
    if (exitCode == 0) {
      const bool hadCapturedCrash = state->crashCaptured.latched;
      CleanupCrashArtifactsAfterZeroExit(cfg, proc, outBase, state);
      AppendExitClassificationLog(outBase, hadCapturedCrash);
    }
    MaybeStopPendingCrashEtwCapture(cfg, proc, outBase, /*force=*/true, &pendingCrashEtw);
    std::wcerr << L"[SkyrimDiagHelper] Target process exited (exit_code=" << exitCode << L").\n";
    AppendLogLine(outBase, L"Target process exited (exit_code=" + std::to_wstring(exitCode) + L").");
    LaunchDeferredViewersAfterExit(cfg, outBase, exitCode, state);
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
