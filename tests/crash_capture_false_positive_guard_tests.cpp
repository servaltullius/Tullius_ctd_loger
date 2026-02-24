#include <cassert>
#include <filesystem>
#include <string>

#include "SourceGuardTestUtils.h"

using skydiag::tests::source_guard::AssertContains;
using skydiag::tests::source_guard::AssertOrdered;
using skydiag::tests::source_guard::ExtractFunctionBody;
using skydiag::tests::source_guard::ReadAllText;

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
  const std::string helperMain = ReadAllText(helperMainPath);
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

  AssertOrdered(
    processValidBody,
    "StartEtwCaptureWithProfile(",
    "MakeIncidentManifestV1(",
    "Post-processing helper must start ETW capture before writing incident manifest.");

  const std::string zeroExitCleanupBody = ExtractFunctionBody(helperMain, "void CleanupCrashArtifactsAfterZeroExit(");
  AssertContains(
    zeroExitCleanupBody,
    "if (!state->crashCaptured)",
    "Zero-exit cleanup must short-circuit when crash capture state is not active.");

  AssertContains(
    zeroExitCleanupBody,
    "MaybeStopPendingCrashEtwCapture(cfg, proc, outBase, /*force=*/true, &state->pendingCrashEtw);",
    "Zero-exit cleanup must force-stop crash ETW before artifact deletion.");

  AssertContains(
    zeroExitCleanupBody,
    "RemoveCrashArtifactsForDump(outBase, state->capturedCrashDumpPath, crashEtwPath)",
    "Zero-exit cleanup must remove crash artifact set tied to captured dump.");

  AssertOrdered(
    zeroExitCleanupBody,
    "MaybeStopPendingCrashEtwCapture(cfg, proc, outBase, /*force=*/true, &state->pendingCrashEtw);",
    "RemoveCrashArtifactsForDump(outBase, state->capturedCrashDumpPath, crashEtwPath)",
    "Zero-exit cleanup must stop ETW before deleting crash artifacts.");

  const std::string processExitTickBody = ExtractFunctionBody(helperMain, "bool HandleProcessExitTick(");
  AssertContains(
    processExitTickBody,
    "WaitForSingleObject(proc.process, 0)",
    "Process exit tick must poll target process handle.");

  AssertContains(
    processExitTickBody,
    "DrainCrashEventBeforeExit(",
    "Process exit tick must drain pending crash signal before exit handling.");

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
    "(exitCode != 0)",
    "Deferred crash viewer launch must not trigger on normal exit_code=0.");
  AssertContains(
    deferredViewerBody,
    "Suppressed deferred crash viewer launch on normal process exit",
    "Normal exit path must explicitly suppress deferred crash viewer popup.");

  AssertContains(
    processExitTickBody,
    "HandleProcessWaitFailed(",
    "Process exit tick must handle WAIT_FAILED via dedicated helper.");

  AssertOrdered(
    processExitTickBody,
    "MaybeStopPendingCrashEtwCapture(cfg, proc, outBase, /*force=*/true, &pendingCrashEtw);",
    "LaunchDeferredViewersAfterExit(",
    "Process exit tick must finalize crash ETW before deferred viewer launch.");

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

  return 0;
}
