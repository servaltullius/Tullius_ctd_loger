#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "SourceGuardTestUtils.h"

using skydiag::tests::source_guard::AssertContains;
using skydiag::tests::source_guard::AssertOrdered;
using skydiag::tests::source_guard::ExtractFunctionBody;
using skydiag::tests::source_guard::ExtractFunctionFlow;
using skydiag::tests::source_guard::ReadAllText;
using skydiag::tests::source_guard::ReadSplitAwareText;

int main()
{
  const std::filesystem::path repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  const std::filesystem::path processAttachPath = repoRoot / "helper" / "src" / "ProcessAttach.cpp";
  const std::filesystem::path crashCapturePath = repoRoot / "helper" / "src" / "CrashCapture.cpp";
  const std::filesystem::path helperMainPath = repoRoot / "helper" / "src" / "main.cpp";
  const std::filesystem::path dumpToolLaunchPath = repoRoot / "helper" / "src" / "DumpToolLaunch.cpp";

  assert(std::filesystem::exists(processAttachPath) && "helper/src/ProcessAttach.cpp not found");
  assert(std::filesystem::exists(crashCapturePath) && "helper/src/CrashCapture.cpp not found");
  assert(std::filesystem::exists(helperMainPath) && "helper/src/main.cpp not found");
  assert(std::filesystem::exists(dumpToolLaunchPath) && "helper/src/DumpToolLaunch.cpp not found");

  const std::string processAttach = ReadAllText(processAttachPath);
  const std::string crashCapture = ReadAllText(crashCapturePath);
  const std::string helperMain = ReadSplitAwareText(helperMainPath);
  const std::string dumpToolLaunch = ReadAllText(dumpToolLaunchPath);

  AssertContains(
    processAttach,
    "EVENT_MODIFY_STATE | SYNCHRONIZE",
    "Crash event handle must include EVENT_MODIFY_STATE to allow ResetEvent.");

  AssertContains(
    processAttach,
    "FILE_MAP_READ | FILE_MAP_WRITE",
    "Helper shared-memory mapping must be writable so a recovered exception can thaw telemetry safely.");

  // HandleCrashEventTick with its steps (CheckCrashLoggerThreadDumpWindow, ...).
  const std::string crashTickBody = ExtractFunctionFlow(
    crashCapture,
    "bool HandleCrashEventTick(",
    { "ThreadDumpWindowOutcome CheckCrashLoggerThreadDumpWindow(",
      "RepeatedRecoveredFaultOutcome CheckRepeatedRecoveredFault(",
      "bool WriteCrashDumpWithRetries(",
      "void HandleCrashDumpWriteFailure(",
      "void ReleaseFilteredCrashDump(" });
  AssertContains(
    crashTickBody,
    "WaitForSingleObject(proc.crashEvent, waitMs)",
    "Crash capture flow must wait on crash event with the configured timeout.");

  AssertContains(
    crashTickBody,
    "if (w == WAIT_FAILED)",
    "Crash capture flow must branch on crash event wait failure.");

  AssertContains(
    crashTickBody,
    "ResetEvent(proc.crashEvent)",
    "Crash capture flow must consume manual-reset crash events after handling.");

  AssertContains(
    crashTickBody,
    "WriteDumpWithStreams(",
    "Crash capture flow must write dump in crash tick path.");

  AssertContains(
    crashTickBody,
    "ExtractCrashInfo(",
    "Crash capture flow must extract crash event information through helper struct conversion.");

  AssertContains(
    crashTickBody,
    "FilterShutdownException(",
    "Crash capture flow must apply shutdown-exception filter after dump capture.");

  AssertContains(
    crashTickBody,
    "ProcessValidCrashDump(",
    "Crash capture flow must route valid dump post-processing through extracted helper.");

  AssertContains(
    crashTickBody,
    "FilterVerdict::kRetryNewerCrash",
    "A newer crash arriving during recovery filtering must supersede the first-chance dump without latching it.");

  AssertContains(
    crashTickBody,
    "if (!IsCommittedCrashSequence(info.crashSeq))",
    "Crash event must fail closed when its snapshot has a zero or odd crash sequence.");

  AssertOrdered(
    crashTickBody,
    "if (!IsCommittedCrashSequence(info.crashSeq))",
    "WriteDumpWithStreams(",
    "Crash sequence validation must run before any dump file is written.");

  AssertOrdered(
    crashTickBody,
    "WriteDumpWithStreams(",
    "FilterShutdownException(",
    "Crash capture must write dump before shutdown filtering (dump-first policy).");

  AssertOrdered(
    crashTickBody,
    "FilterShutdownException(",
    "ProcessValidCrashDump(",
    "Crash capture must run post-processing only after filtering keeps the dump.");

  const std::string firstChanceFilterBody = ExtractFunctionBody(crashCapture, "FilterVerdict FilterFirstChanceException(");
  AssertContains(
    firstChanceFilterBody,
    "WaitForCrashUnhandledOrProcess(crashEvent, unhandledEvent, process, kHeartbeatCheckIntervalMs)",
    "First-chance filter must wake for a newer crash, and for the plugin's unhandled signal, instead of sleeping "
    "through the real CTD.");
  // ADR-0032: the plugin's signal settles what the heartbeat and the exit
  // code can only suggest, so it is looked at before both.
  AssertOrdered(
    firstChanceFilterBody,
    "IsUnhandledExceptionSignaled(unhandledEvent)",
    "ClassifyExitCodeVerdictWithContext(",
    "An exception the plugin reported as unhandled must be kept whatever the exit code is.");
  AssertOrdered(
    firstChanceFilterBody,
    "HasNewerCrashRecord(shm, info.crashSeq)",
    "IsUnhandledExceptionSignaled(unhandledEvent)",
    "A newer crash record still comes first.");
  const std::string shutdownFilterBody = ExtractFunctionBody(crashCapture, "FilterVerdict FilterShutdownException(");
  AssertOrdered(
    shutdownFilterBody,
    "IsUnhandledExceptionSignaled(unhandledEvent)",
    "ClassifyExitCodeVerdictWithContext(",
    "The shutdown filter must not release an exception the plugin reported as unhandled.");
  AssertOrdered(
    crashTickBody,
    "WriteCrashDumpWithRetries(",
    "SetEvent(proc.dumpSettledEvent)",
    "The plugin's hold must be ended once the dump write is over.");
  AssertOrdered(
    crashTickBody,
    "SetEvent(proc.dumpSettledEvent)",
    "FilterShutdownException(",
    "The plugin's hold must be ended before the filters wait for the game.");
  const std::string thawCaptureBody = ExtractFunctionBody(crashCapture, "bool ThawCrashCapture(");
  AssertOrdered(
    thawCaptureBody,
    "ResetEvent(proc.dumpSettledEvent)",
    "TryClearRecoveredCrashFreeze(",
    "An incident's signals must be cleared before the freeze, or the next crash finds them.");
  AssertOrdered(
    thawCaptureBody,
    "ResetEvent(proc.unhandledEvent)",
    "TryClearRecoveredCrashFreeze(",
    "An incident's signals must be cleared before the freeze, or the next crash finds them.");
  assert(
    crashCapture.find("TryClearRecoveredCrashFreeze(proc.shmWritable") ==
      crashCapture.rfind("TryClearRecoveredCrashFreeze(proc.shmWritable") &&
    "Every release of an incident must go through ThawCrashCapture.");
  AssertContains(
    firstChanceFilterBody,
    "HasNewerCrashRecord(shm, info.crashSeq)",
    "First-chance filter must compare committed crash sequences before declaring recovery.");
  // A fault on the game's main thread is never one of Crash Logger's thread
  // dump probes, so its dump does not wait for the heartbeat check.
  AssertOrdered(
    crashTickBody,
    "InferMainThreadIdFromSnapshot(dumpSnapshot, dumpSnapshotBytes)",
    "WriteDumpWithStreams(",
    "The Crash Logger thread dump check must know the main thread before the dump is written.");

  const std::string processValidBody = ExtractFunctionBody(crashCapture, "void ProcessValidCrashDump(");
  AssertContains(
    processValidBody,
    "CollectPluginScanJson(",
    "Post-processing helper must collect plugin scan sidecar.");

  AssertContains(
    processValidBody,
    "QueueDeferredCrashViewer(",
    "Post-processing helper must queue deferred viewer launch when process-exit auto-open is delayed.");

  AssertContains(
    processValidBody,
    "ApplyRetentionFromConfig(cfg, outBase)",
    "Post-processing helper must apply retention after successful capture.");

  AssertContains(
    processValidBody,
    "Process exited with exit_code=0 during wait window",
    "Post-processing helper must handle exit_code=0 during the viewer wait window.");
  AssertContains(
    processValidBody,
    "suppressing deferred crash viewer",
    "Post-processing helper must suppress crash viewer launch once the process reports a zero exit code.");
  // ADR-0032: not for a dump the plugin still reports as an unhandled crash.
  AssertOrdered(
    processValidBody,
    "DoesCapturedCrashOutliveZeroExit(",
    "if (IsNormalExitForCapturedCrash(processExitCode, crashOutlivesZeroExit)) {",
    "A zero exit code in the viewer wait window must not undo a crash the plugin reported as unhandled.");
  // Every reader of an exit code that gives a verdict goes through one
  // function, which looks at the plugin's signal first.
  const std::string classifyExitBody =
    ExtractFunctionBody(crashCapture, "FilterVerdict ClassifyExitCodeVerdictWithContext(");
  AssertOrdered(
    classifyExitBody,
    "if (IsUnhandledExceptionSignaled(unhandledEvent)) {",
    "ClassifyExitCodeVerdict(exitCode, info, outBase)",
    "The exit code's verdict must look at the plugin's unhandled signal first.");
  AssertContains(
    crashTickBody,
    "cfg, exitCode, UnhandledEventFor(proc, info, crashState), info, outBase, L\"post_dump_exit_check\", -1,",
    "The exit check after a dump written late must judge the exit code with the plugin's signal.");
  assert(
    crashCapture.find("ClassifyExitCodeVerdict(exitCode") == crashCapture.rfind("ClassifyExitCodeVerdict(exitCode") &&
    "Only ClassifyExitCodeVerdictWithContext may turn a bare exit code into a verdict.");

  AssertOrdered(
    processValidBody,
    "StartEtwCaptureWithProfile(",
    "MakeIncidentManifestV1(",
    "Post-processing helper must start ETW capture before writing incident manifest.");

  const std::string zeroExitCleanupBody = ExtractFunctionBody(helperMain, "void CleanupFilteredCrashArtifacts(");
  assert(
    zeroExitCleanupBody.find("exitCode0StrongCrash") == std::string::npos &&
    "Zero-exit cleanup must not preserve artifacts based on strong first-chance exception evidence.");
  AssertContains(
    zeroExitCleanupBody,
    "if (!state->crashCaptured.latched)",
    "Zero-exit cleanup must short-circuit when crash capture state is not active.");
  AssertOrdered(
    zeroExitCleanupBody,
    "TryWriteCleanExitEvidenceRecord(",
    "RemoveCrashArtifactsForDump(",
    "Late zero-exit evidence must be written before filtered artifacts are removed.");
  AssertContains(
    zeroExitCleanupBody,
    "CleanExitDumpState::kPendingDelete",
    "Late zero-exit cleanup must commit a pending deletion record before touching the dump.");
  AssertOrdered(
    zeroExitCleanupBody,
    "RemoveCrashArtifactsForDump(",
    "CleanExitDumpState::kDeleteFailed",
    "Late zero-exit cleanup must derive the final evidence state from observed removal.");

  AssertOrdered(
    crashTickBody,
    "TryWriteCleanExitEvidenceRecord(",
    "if (preserveFilteredDump)",
    "Fast zero-exit evidence must be written before the dump is preserved or removed.");
  AssertContains(
    crashTickBody,
    "CleanExitDumpState::kPendingDelete",
    "Fast zero-exit filtering must commit a pending deletion record before touching the dump.");
  AssertContains(
    crashTickBody,
    "RemoveFileAndObserve(dumpPath)",
    "Fast zero-exit filtering must observe the final dump state after deletion.");

  AssertContains(
    zeroExitCleanupBody,
    "MaybeStopPendingCrashEtwCapture(cfg, proc, outBase, /*force=*/true, &state->pendingCrashEtw);",
    "Zero-exit cleanup must force-stop crash ETW before artifact deletion.");

  AssertContains(
    zeroExitCleanupBody,
    "RemoveCrashArtifactsForDump(\n      outBase,\n      state->capturedCrashDumpPath,\n      removableCrashEtwPath,\n      preserveDump)",
    "Zero-exit cleanup must remove the captured artifact set without deleting an ETW path whose cleanup is unconfirmed.");

  AssertOrdered(
    zeroExitCleanupBody,
    "MaybeStopPendingCrashEtwCapture(cfg, proc, outBase, /*force=*/true, &state->pendingCrashEtw);",
    "RemoveCrashArtifactsForDump(",
    "Zero-exit cleanup must stop ETW before deleting crash artifacts.");

  const std::string processExitTickBody = ExtractFunctionBody(helperMain, "bool HandleProcessExitTick(");
  AssertContains(
    processExitTickBody,
    "WaitForSingleObject(proc.process, 0)",
    "Process exit tick must poll target process handle.");

  AssertContains(
    processExitTickBody,
    "DrainCrashEventBeforeExit(cfg, proc, outBase, exitCode, state, &releasedCrashPromotedOnUnhandledSignal);",
    "Process exit tick must drain pending crash metadata for every observed exit code.");
  assert(
    processExitTickBody.find("sharedMemoryStrongCrash") == std::string::npos &&
    processExitTickBody.find("exitCode0StrongCrash") == std::string::npos &&
    "Process exit tick must not override exit_code=0 on the strength of the exception.");

  AssertContains(
    processExitTickBody,
    "CleanupCrashArtifactsAfterZeroExit(",
    "Process exit tick must invoke zero-exit crash artifact cleanup path.");
  // ADR-0032: exit_code=0 is normal termination unless the plugin still
  // reports the captured crash as not handled. Read before the drain, which
  // latches a record nobody captured.
  AssertOrdered(
    processExitTickBody,
    "DoesCapturedCrashOutliveZeroExit(",
    "DrainCrashEventBeforeExit(cfg, proc, outBase, exitCode, state, &releasedCrashPromotedOnUnhandledSignal);",
    "Whether the captured crash is still unhandled must be read before the exit drain latches anything.");
  AssertOrdered(
    processExitTickBody,
    "DoesCapturedCrashOutliveZeroExit(",
    "IsLateRecoveryWithheld(",
    "A capture whose late recovery was held back must not be kept whole through a zero exit.");
  AssertOrdered(
    processExitTickBody,
    "if (IsNormalExitForCapturedCrash(exitCode, crashOutlivesZeroExit)) {",
    "CleanupCrashArtifactsAfterZeroExit(",
    "Zero-exit cleanup must not run for a crash the plugin still reports as unhandled.");
  AssertContains(
    processExitTickBody,
    "capturedCrashOutlivesZeroExit || releasedCrashPromotedOnUnhandledSignal;",
    "A released crash the drain reported on the plugin's signal must outlive a zero exit too.");
  AssertContains(
    processExitTickBody,
    "LaunchDeferredViewersAfterExit(cfg, outBase, exitCode, crashOutlivesZeroExit, state);",
    "The exit's viewer must be told when a crash is kept through a zero exit.");

  AssertContains(
    processExitTickBody,
    "LaunchDeferredViewersAfterExit(",
    "Process exit tick must invoke deferred viewer launch helper.");

  // ADR-0032: the plugin's signal holds a late recovery back, but only up to
  // its cap, and only once the heartbeat says the game is running on.
  const std::string lateRecoveryTickBody = ExtractFunctionBody(helperMain, "bool HandleLateRecoveryTick(");
  AssertOrdered(
    lateRecoveryTickBody,
    "AdvanceLateRecovery(",
    "ShouldWithholdLateRecovery(unhandledSignalSet, crash.lateRecovery.latchedTick64, nowTick64)",
    "The late recovery must be held back through the capped rule, after the heartbeat check.");
  assert(
    lateRecoveryTickBody.find("if (IsCapturedCrashStillUnhandled(") == std::string::npos &&
    "The late recovery must not be held back for the signal without the cap.");
  AssertOrdered(
    lateRecoveryTickBody,
    "ShouldWithholdLateRecovery(",
    "ThawCrashCapture(proc, crashSeq)",
    "A capture released at the cap must let go of the plugin's signal with the freeze.");

  // An exception released at that cap left the signal set while the game ran
  // on: the signal is not used for it again. Everything that reads or waits
  // on the signal for an exception takes the event from UnhandledEventFor;
  // a bare proc.unhandledEvent would bring back a dump and a freeze each time.
  AssertContains(
    lateRecoveryTickBody,
    "NoteRecoveredFault(&crash.signalLeftSetFaults, fault.exceptionCode, fault.exceptionAddr);",
    "A release at the cap must note the exception whose signal was left set.");
  const std::string exitDrainBody = ExtractFunctionBody(helperMain, "bool DrainCrashEventBeforeExit(");
  assert(
    lateRecoveryTickBody.find("proc.unhandledEvent") == std::string::npos &&
    processExitTickBody.find("proc.unhandledEvent") == std::string::npos &&
    exitDrainBody.find("proc.unhandledEvent") == std::string::npos &&
    "The helper loop must read the plugin's signal through UnhandledEventFor.");
  {
    // In CrashCapture.cpp only UnhandledEventFor itself and ThawCrashCapture
    // (a check and a reset) name the event.
    std::size_t bareUses = 0;
    for (auto at = crashCapture.find("proc.unhandledEvent"); at != std::string::npos;
         at = crashCapture.find("proc.unhandledEvent", at + 1)) {
      ++bareUses;
    }
    assert(bareUses == 3u && "Crash capture must read the plugin's signal through UnhandledEventFor.");
  }
  AssertOrdered(
    crashTickBody,
    "EndHoldOfSignalLeftSetFault(proc, outBase, info, crashState);",
    "CheckCrashLoggerThreadDumpWindow(",
    "The plugin's hold for an exception whose signal is not used must end before any recovery check.");

  const std::string deferredViewerBody = ExtractFunctionBody(helperMain, "void LaunchDeferredViewersAfterExit(");
  AssertContains(
    deferredViewerBody,
    "const bool normalExit = IsNormalExitForCapturedCrash(exitCode, crashOutlivesZeroExit);",
    "Deferred crash viewer launch must judge the exit as the exit tick does.");
  AssertContains(
    deferredViewerBody,
    "cfg.autoOpenViewerOnCrash &&\n      !normalExit",
    "Deferred crash viewer launch must require a non-zero process exit code, or a crash kept through a zero exit.");
  AssertContains(
    deferredViewerBody,
    "cfg.autoOpenViewerOnCrash && normalExit) {",
    "Only a normal exit suppresses the deferred crash viewer.");
  assert(
    deferredViewerBody.find("exitCode0StrongCrash") == std::string::npos &&
    "Deferred crash viewer launch must not override a zero exit code with exception strength.");
  AssertContains(
    deferredViewerBody,
    "Suppressed deferred crash viewer launch on normal process exit",
    "Normal exit path must explicitly suppress deferred crash viewer popup.");

  AssertContains(
    processExitTickBody,
    "HandleProcessWaitFailed(",
    "Process exit tick must handle WAIT_FAILED via dedicated helper.");

  AssertContains(
    processExitTickBody,
    "WriteWerFallbackHint(outBase, WerFallbackReason::kAbnormalExitWithoutCrash, exitCode)",
    "Abnormal exit without an internal dump must emit WER LocalDumps fallback guidance.");
  AssertOrdered(
    processExitTickBody,
    "IsCloseOfUnresponsiveGame(",
    "WriteWerFallbackHint(outBase, WerFallbackReason::kAbnormalExitWithoutCrash, exitCode)",
    "A frozen game that was closed must not get the abnormal-exit WER hint.");
  AssertOrdered(
    processExitTickBody,
    "} else if (crashOnRecord) {",
    "WriteWerFallbackHint(outBase, WerFallbackReason::kAbnormalExitWithoutCrash, exitCode)",
    "A fault the exit drain just recorded must not get the hint saying no exception was recorded.");

  const std::string drainBody = ExtractFunctionBody(helperMain, "bool DrainCrashEventBeforeExit(");
  // ADR-0032: a released crash the plugin then reported as not handled.
  AssertOrdered(
    drainBody,
    "ShouldReportReleasedCrashOnUnhandledSignal(",
    "!ShouldReportReleasedCrashAtExit(",
    "The exit drain must look at the plugin's signal before the exit code skips a released crash.");
  AssertOrdered(
    drainBody,
    "state->crashCaptured.releasedCrashSeq == info.crashSeq",
    "if (exitCode == 0u)",
    "The exit drain must skip a released crash generation before either exit-code path reports it.");
  AssertContains(
    drainBody,
    "state->hangState.hangCapturedThisEpisode",
    "A released fault must not be reported again when the game froze after its release.");
  // A thread dump of a soft-locked game that was then ended looks like a
  // worker's crash Crash Logger ended; only its crash log tells them apart.
  AssertOrdered(
    drainBody,
    "if (!reportedUnhandled && crash.releasedInThreadDumpWindow && exitCode != info.exceptionCode) {",
    "TryWritePostExitCrashEvidenceRecord(",
    "An early thread dump window release must need Crash Logger's crash log before it is recorded at exit.");
  AssertOrdered(
    crashTickBody,
    "FilterFirstChanceException(",
    "crashState->releasedInThreadDumpWindow = true;",
    "The early thread dump window release must be marked for the exit path.");
  const std::string repeatedFaultCheckBody = ExtractFunctionBody(
    crashCapture,
    "RepeatedRecoveredFaultOutcome CheckRepeatedRecoveredFault(");
  AssertOrdered(
    repeatedFaultCheckBody,
    "IsRepeatedRecoveredFault(",
    "FilterFirstChanceException(",
    "A repeated handled fault must check recovery before writing a dump.");
  AssertContains(
    repeatedFaultCheckBody,
    "crashState->releasedInThreadDumpWindow = false;",
    "A repeated handled fault release must not require Crash Logger's crash log at exit.");
  assert(
    repeatedFaultCheckBody.find("releasedInThreadDumpWindow = true") == std::string::npos &&
    "CheckRepeatedRecoveredFault must never set releasedInThreadDumpWindow.");
  AssertContains(
    repeatedFaultCheckBody,
    "NoteRecoveredFault(&crashState->recoveredFaults, info.exceptionCode, info.exceptionAddr);",
    "Releasing a repeated fault without a dump must still increment the recovered-fault count.");

  // A helper the plugin's watchdog restarted must not report a generation an
  // earlier helper already acknowledged by clearing its freeze.
  const std::string initLoopBody = ExtractFunctionBody(helperMain, "void InitializeLoopState(");
  AssertOrdered(
    initLoopBody,
    "TryCaptureCommittedCrashInfo(",
    "state->crashCaptured.releasedCrashSeq = committed.crashSeq;",
    "Loop state must seed the released generation from shared memory.");
  AssertContains(
    initLoopBody,
    "(committed.stateFlags & skydiag::kState_Frozen) == 0u",
    "Only a generation whose freeze was cleared counts as acknowledged.");

  AssertOrdered(
    processExitTickBody,
    "MaybeStopPendingCrashEtwCapture(cfg, proc, outBase, /*force=*/true, &pendingCrashEtw);",
    "LaunchDeferredViewersAfterExit(",
    "Process exit tick must finalize crash ETW before deferred viewer launch.");

  const std::string helperLoopBody = ExtractFunctionBody(helperMain, "void RunHelperLoop(");
  AssertOrdered(
    helperLoopBody,
    "/*waitMs=*/0",
    "HandleProcessExitTick(cfg, proc, outBase, state)",
    "Helper loop must drain the crash event before polling process exit.");

  const std::string manualCaptureBody = ExtractFunctionBody(helperMain, "void PumpManualCaptureInputs(");
  AssertContains(
    manualCaptureBody,
    "triggeredFromHotkeyMessage",
    "Manual capture input pump must suppress fallback polling when WM_HOTKEY already fired.");
  AssertContains(
    manualCaptureBody,
    "TryTriggerManualCapture(",
    "Manual capture input pump must route both hotkey paths through a shared debounce helper.");
  AssertContains(
    manualCaptureBody,
    "ManualCaptureKeys().ConsumePress()",
    "Manual capture must also take presses from the key-state watcher, which WM_HOTKEY can miss in gameplay.");
  if (manualCaptureBody.find("GetAsyncKeyState(") != std::string::npos) {
    std::fprintf(stderr, "Manual capture must take polled presses from the key-state watcher, not read keys itself.\n");
    std::abort();
  }
  const std::string keyWatcher = ReadAllText(repoRoot / "helper" / "src" / "ManualCaptureKeyWatcher.cpp");
  AssertContains(
    keyWatcher,
    "& 0x8000) != 0",
    "The key-state watcher must read the keys' current down state.");
  if (keyWatcher.find(") & 1)") != std::string::npos) {
    std::fprintf(stderr, "Manual capture must not read GetAsyncKeyState's shared pressed-since-last-call bit.\n");
    std::abort();
  }
  // Both paths watch the configured ManualCaptureHotkey, not a fixed chord.
  AssertContains(
    keyWatcher,
    "edge.emplace(*manualCapture);",
    "The key-state watcher must poll the configured manual capture hotkey.");
  AssertContains(
    helperMain,
    "RegisterHotKey(nullptr, kHotkeyId, ManualCaptureHotkeyRegistrationModifiers(hotkey), hotkey.virtualKey)",
    "RegisterHotKey must register the configured manual capture hotkey.");
  AssertContains(
    helperMain,
    "ManualCaptureKeys().Start(hotkey, crashLoggerThreadDump, legacyManualCapture);",
    "The key-state watcher must start with the configured manual capture hotkey.");
  // A press of the old Ctrl+Shift+F12 is logged as taking no snapshot, with or
  // without Crash Logger: the watcher polls the exact old chord on its own
  // edge and only sets a flag, and the helper loop writes the log line.
  AssertContains(
    keyWatcher,
    "legacyEdge.emplace(*legacyManualCapture);",
    "The key-state watcher must poll the old manual capture chord on its own edge.");
  AssertContains(
    keyWatcher,
    "legacyPressed_.store(true, std::memory_order_release);",
    "The key-state watcher must flag presses of the old manual capture chord.");
  if (keyWatcher.find("AppendLogLine") != std::string::npos) {
    std::fprintf(stderr, "The key-state watcher thread must not write the helper log itself.\n");
    std::abort();
  }
  AssertOrdered(
    ExtractFunctionBody(helperMain, "void RegisterManualCaptureHotkeyIfEnabled("),
    "LegacyManualCaptureChordToWatch(cfg.enableManualCaptureHotkey, cfg.manualCaptureHotkey)",
    "ManualCaptureKeys().Start(std::nullopt, crashLoggerThreadDump, legacyManualCapture);",
    "The old manual capture chord must be chosen before the key-state watcher starts, Crash Logger or not.");
  AssertContains(
    manualCaptureBody,
    "if (ManualCaptureKeys().ConsumeLegacyPress()) {\n"
    "    AppendLogLine(outBase, LegacyManualCaptureHotkeyPressedMessage(cfg.manualCaptureHotkey));",
    "The helper loop must log each press of the old manual capture chord.");
  // The Crash Logger thread dump window (IsLikelyCrashLoggerThreadDumpFault)
  // follows Crash Logger's own hotkey, which no longer matches the manual
  // capture hotkey by default: the watcher notes it, also with the manual
  // capture hotkey off, and a manual capture alone does not open the window.
  AssertContains(
    keyWatcher,
    "IsCrashLoggerThreadDumpHotkeyDown(*crashLoggerThreadDump, IsKeyDown)",
    "The key-state watcher must poll Crash Logger's thread dump hotkey.");
  AssertContains(
    keyWatcher,
    "NoteCrashLoggerThreadDumpHotkeyPressed(GetTickCount64());",
    "The key-state watcher must note when Crash Logger's thread dump hotkey is held.");
  const std::string registerHotkeyBody =
    ExtractFunctionBody(helperMain, "void RegisterManualCaptureHotkeyIfEnabled(");
  AssertOrdered(
    registerHotkeyBody,
    "CheckCrashLoggerThreadDumpHotkey(",
    "if (!cfg.enableManualCaptureHotkey) {\n"
    "    ManualCaptureKeys().Start(std::nullopt, crashLoggerThreadDump, legacyManualCapture);",
    "Crash Logger's thread dump hotkey must be watched even with the manual capture hotkey off.");
  AssertContains(
    ExtractFunctionBody(helperMain, "void UnregisterManualCaptureHotkeyIfEnabled("),
    "}\n  ManualCaptureKeys().Stop();",
    "The key-state watcher must stop whether or not the manual capture hotkey was on.");
  if (crashCapture.find("NoteManualCaptureStarted") != std::string::npos ||
      ReadAllText(repoRoot / "helper" / "src" / "ManualCapture.cpp").find("NoteCrashLoggerThreadDumpHotkeyPressed") !=
        std::string::npos) {
    std::fprintf(stderr, "A manual capture must not open the Crash Logger thread dump window by itself.\n");
    std::abort();
  }
  if (helperMain.find("VK_F12") != std::string::npos || keyWatcher.find("VK_F12") != std::string::npos) {
    std::fprintf(stderr, "The manual capture hotkey must not be hard-coded to F12.\n");
    std::abort();
  }
  AssertContains(
    helperMain,
    "GetTickCount64()",
    "Manual capture debounce must use wall-clock gating to avoid duplicate capture bursts.");

  // wmain only guards RunHelper: an exception reaches the session log and the
  // process ends at once (a joinable retention thread would std::terminate).
  const std::string helperWmainBody = ExtractFunctionBody(helperMain, "int wmain(int argc, wchar_t** argv)");
  AssertOrdered(
    helperWmainBody,
    "return RunHelper(argc, argv);",
    "catch (...)",
    "wmain must run the helper inside a catch-all.");
  AssertOrdered(
    helperWmainBody,
    "Helper stopped by an unexpected error: ",
    "ExitProcess(",
    "An unexpected exception must be logged before the helper exits.");
  const std::string helperEntryBody = ExtractFunctionBody(helperMain, "static int RunHelper(int argc, wchar_t** argv)");
  // Before the session log starts, wmain logs an exception to the configured
  // OutputDir: it is known as soon as the config is read, before attaching.
  AssertOrdered(
    helperEntryBody,
    "LoadConfig(",
    "SetHelperPreSessionOutBase(",
    "The pre-session log folder comes from the config.");
  AssertOrdered(
    helperEntryBody,
    "SetHelperPreSessionOutBase(",
    "AttachByPid(",
    "An exception while attaching must have a log folder.");
  // Attaching reuses err and clears it on success, so the config warning
  // (an invalid ManualCaptureHotkey, say) must be kept before attaching to
  // reach the helper log.
  AssertOrdered(
    helperEntryBody,
    "const std::wstring configWarning = err;",
    "AttachByPid(",
    "The config warning must be kept before attaching clears err.");
  AssertContains(
    helperEntryBody,
    "AppendLogLine(outBase, L\"Config warning: \" + configWarning);",
    "The config warning must be written to the helper log.");
  AssertContains(
    helperEntryBody,
    "if (!helperSingletonMutex)",
    "Helper entry must fail closed when its singleton mutex cannot be created.");
  AssertContains(
    helperEntryBody,
    "return 4;",
    "A singleton mutex creation failure must return a non-zero process exit code.");
  AssertOrdered(
    helperEntryBody,
    "HANDLE helperSingletonMutex = skydiag::helper::internal::AcquireHelperSingletonMutex(proc.pid, &err);",
    "skydiag::helper::internal::StartHelperLogSession(outBase);",
    "Helper entry must acquire singleton mutex before restarting the helper log so duplicate helpers do not erase active diagnostics.");

  const std::string thawBody = ExtractFunctionBody(
    crashCapture,
    "bool TryClearRecoveredCrashFreeze(");
  AssertOrdered(
    thawBody,
    "ReadCrashSequence(&shm->header) != expectedCrashSeq",
    "InterlockedAnd(",
    "Helper ACK/reset must verify the committed incident generation before clearing ownership.");
  AssertContains(
    thawBody,
    "~static_cast<LONG>(skydiag::kState_Frozen)",
    "Helper ACK/reset must atomically clear the protocol-v4 incident ownership bit.");
  AssertContains(
    thawBody,
    "~static_cast<LONG>(skydiag::kState_Frozen));\n"
    "  MemoryBarrier();\n"
    "\n"
    "  if (ReadCrashSequence(&shm->header) != expectedCrashSeq)",
    "Helper ACK/reset must recheck the generation after releasing ownership.");
  AssertContains(
    thawBody,
    "InterlockedOr(",
    "Helper ACK/reset must restore ownership if a newer generation raced with its clear.");

  const std::string viewerLaunchBody = ExtractFunctionBody(dumpToolLaunch, "DumpToolViewerLaunchResult StartDumpToolViewer(");
  AssertContains(
    viewerLaunchBody,
    "DumpTool viewer launch failed (reason=",
    "DumpTool viewer launch failures must be persisted to helper log for diagnosis.");

  AssertContains(
    viewerLaunchBody,
    "win32_error=",
    "DumpTool viewer launch failure logs must include Win32 error code.");

  AssertContains(
    viewerLaunchBody,
    ", exe=",
    "DumpTool viewer launch diagnostics must include resolved executable path.");

  AssertContains(
    dumpToolLaunch,
    "GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()))",
    "DumpTool launcher must resolve its executable directory with a dynamic path buffer.");

  assert(
    dumpToolLaunch.find("GetModuleFileNameW(nullptr, buf, MAX_PATH)") == std::string::npos &&
    "DumpTool launcher must not use MAX_PATH-sized fixed buffers.");

  return 0;
}
