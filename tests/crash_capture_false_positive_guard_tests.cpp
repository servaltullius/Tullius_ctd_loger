#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "SourceGuardTestUtils.h"

using skydiag::tests::source_guard::AssertContains;
using skydiag::tests::source_guard::AssertOrdered;
using skydiag::tests::source_guard::ExtractFunctionBody;
using skydiag::tests::source_guard::ReadAllText;
using skydiag::tests::source_guard::ReadSplitAwareText;

int main()
{
  const std::filesystem::path repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  const std::filesystem::path processAttachPath = repoRoot / "helper" / "src" / "ProcessAttach.cpp";
  const std::filesystem::path crashCapturePath = repoRoot / "helper" / "src" / "CrashCapture.cpp";
  const std::filesystem::path helperMainPath = repoRoot / "helper" / "src" / "main.cpp";
  const std::filesystem::path dumpToolLaunchPath = repoRoot / "helper" / "src" / "DumpToolLaunch.cpp";
  const std::filesystem::path crashHeuristicsPath = repoRoot / "helper" / "include" / "SkyrimDiagHelper" / "CrashHeuristics.h";

  assert(std::filesystem::exists(processAttachPath) && "helper/src/ProcessAttach.cpp not found");
  assert(std::filesystem::exists(crashCapturePath) && "helper/src/CrashCapture.cpp not found");
  assert(std::filesystem::exists(helperMainPath) && "helper/src/main.cpp not found");
  assert(std::filesystem::exists(dumpToolLaunchPath) && "helper/src/DumpToolLaunch.cpp not found");
  assert(std::filesystem::exists(crashHeuristicsPath) && "helper/include/SkyrimDiagHelper/CrashHeuristics.h not found");

  const std::string processAttach = ReadAllText(processAttachPath);
  const std::string crashCapture = ReadAllText(crashCapturePath);
  const std::string helperMain = ReadSplitAwareText(helperMainPath);
  const std::string dumpToolLaunch = ReadAllText(dumpToolLaunchPath);
  const std::string crashHeuristics = ReadAllText(crashHeuristicsPath);

  AssertContains(
    crashHeuristics,
    "kStatusControlCExit",
    "Strong crash heuristic must treat STATUS_CONTROL_C_EXIT as weak for normal-exit filtering.");
  AssertContains(
    crashHeuristics,
    "kStatusCppException",
    "Strong crash heuristic must include handled C++ exception code in weak set.");

  AssertContains(
    processAttach,
    "EVENT_MODIFY_STATE | SYNCHRONIZE",
    "Crash event handle must include EVENT_MODIFY_STATE to allow ResetEvent.");

  AssertContains(
    processAttach,
    "FILE_MAP_READ | FILE_MAP_WRITE",
    "Helper shared-memory mapping must be writable so a recovered exception can thaw telemetry safely.");

  const std::string crashTickBody = ExtractFunctionBody(crashCapture, "bool HandleCrashEventTick(");
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
    "WaitForCrashOrProcess(crashEvent, process, kHeartbeatCheckIntervalMs)",
    "First-chance filter must wake for a newer crash instead of sleeping through the real CTD.");
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
    "DrainCrashEventBeforeExit(cfg, proc, outBase, exitCode, state);",
    "Process exit tick must drain pending crash metadata for every observed exit code.");
  assert(
    processExitTickBody.find("sharedMemoryStrongCrash") == std::string::npos &&
    processExitTickBody.find("exitCode0StrongCrash") == std::string::npos &&
    "Process exit tick must treat exit_code=0 as authoritative normal termination.");

  AssertContains(
    processExitTickBody,
    "CleanupCrashArtifactsAfterZeroExit(",
    "Process exit tick must invoke zero-exit crash artifact cleanup path.");

  AssertContains(
    processExitTickBody,
    "LaunchDeferredViewersAfterExit(",
    "Process exit tick must invoke deferred viewer launch helper.");

  const std::string deferredViewerBody = ExtractFunctionBody(helperMain, "void LaunchDeferredViewersAfterExit(");
  AssertContains(
    deferredViewerBody,
    "cfg.autoOpenViewerOnCrash &&\n      exitCode != 0",
    "Deferred crash viewer launch must require a non-zero process exit code.");
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
    "if (crash.releasedInThreadDumpWindow && exitCode != info.exceptionCode) {",
    "TryWritePostExitCrashEvidenceRecord(",
    "An early thread dump window release must need Crash Logger's crash log before it is recorded at exit.");
  AssertOrdered(
    crashTickBody,
    "FilterFirstChanceException(",
    "crashState->releasedInThreadDumpWindow = true;",
    "The early thread dump window release must be marked for the exit path.");

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
    "RegisterHotKey(nullptr, kHotkeyId, hotkey.modifiers, hotkey.virtualKey)",
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

  const std::string helperEntryBody = ExtractFunctionBody(helperMain, "int wmain(int argc, wchar_t** argv)");
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
