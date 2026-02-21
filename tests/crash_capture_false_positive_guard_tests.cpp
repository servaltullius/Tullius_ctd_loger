#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

static std::string ReadAllText(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::in | std::ios::binary);
  assert(in && "Failed to open file");
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static void AssertContains(const std::string& haystack, const char* needle, const char* message)
{
  assert(haystack.find(needle) != std::string::npos && message);
}

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
  const std::string helperMain = ReadAllText(helperMainPath);
  const std::string dumpToolLaunch = ReadAllText(dumpToolLaunchPath);

  AssertContains(
    processAttach,
    "EVENT_MODIFY_STATE | SYNCHRONIZE",
    "Crash event handle must include EVENT_MODIFY_STATE to allow ResetEvent.");

  AssertContains(
    crashCapture,
    "ResetEvent(proc.crashEvent)",
    "Crash capture flow must consume manual-reset crash events after handling.");

  AssertContains(
    crashCapture,
    "kRequiredHeartbeatAdvances = 2",
    "Handled-exception filter must require multiple heartbeat advances before deleting a dump.");

  AssertContains(
    crashCapture,
    "kState_InMenu",
    "Crash capture must reference in-menu state to suppress shutdown-boundary false positives.");

  AssertContains(
    crashCapture,
    "keeping dump and preserving crash auto-actions",
    "Crash capture must keep crash auto-actions enabled for non-zero exits near menu/shutdown boundary.");

  AssertContains(
    crashCapture,
    "menu/shutdown boundary during heartbeat check with non-zero exit",
    "Crash capture must preserve crash auto-actions even when non-zero exits happen during heartbeat checks.");

  AssertContains(
    crashCapture,
    "after auto-open wait (wait_ms=",
    "Crash capture must log deferred crash viewer decisions with explicit wait timeout details.");

  AssertContains(
    helperMain,
    "RemoveCrashArtifactsForDump",
    "Helper main loop must remove crash artifacts if a prior crash capture is followed by exit_code=0.");

  AssertContains(
    helperMain,
    "exit_code=0 after crash capture; removed",
    "Helper main loop must log normal-exit crash artifact cleanup.");

  AssertContains(
    helperMain,
    "Failed to remove crash artifact",
    "Crash artifact cleanup must log per-file deletion failures for diagnostics.");

  AssertContains(
    helperMain,
    "Deferred crash viewer launch attempted after process exit (exit_code=",
    "Helper main loop must log deferred crash viewer launch attempts with exit code context.");

  AssertContains(
    crashCapture,
    "IsStrongCrashException",
    "Crash capture must consult IsStrongCrashException to avoid suppressing real CTDs when exit_code=0 is misleading.");

  AssertContains(
    helperMain,
    "exit_code=0 after crash capture but exception_code=",
    "Helper must preserve crash artifacts when a strong crash exception is paired with exit_code=0.");

  AssertContains(
    helperMain,
    "crash_deferred_exit_code0_strong",
    "Helper must attempt deferred crash viewer launch for strong crash exceptions even when exit_code=0.");

  AssertContains(
    dumpToolLaunch,
    "DumpTool viewer launch failed (reason=",
    "DumpTool viewer launch failures must be persisted to helper log for diagnosis.");

  AssertContains(
    dumpToolLaunch,
    "win32_error=",
    "DumpTool viewer launch failure logs must include Win32 error code.");

  AssertContains(
    dumpToolLaunch,
    ", exe=",
    "DumpTool viewer launch diagnostics must include resolved executable path.");

  const auto crashCapturedBranchPos = helperMain.find("if (crashCaptured) {");
  assert(crashCapturedBranchPos != std::string::npos && "Missing crashCaptured branch in helper main loop");
  const auto forceEtwStopPos =
    helperMain.find("MaybeStopPendingCrashEtwCapture(cfg, proc, outBase, /*force=*/true, &pendingCrashEtw);", crashCapturedBranchPos);
  const auto cleanupPos = helperMain.find("RemoveCrashArtifactsForDump(outBase, capturedCrashDumpPath", crashCapturedBranchPos);
  assert(forceEtwStopPos != std::string::npos && "Helper must force-stop crash ETW in the normal-exit cleanup branch.");
  assert(cleanupPos != std::string::npos && "Helper must clean up crash artifacts in the normal-exit cleanup branch.");
  assert(
    forceEtwStopPos < cleanupPos &&
    "Helper must stop crash ETW before removing crash artifacts to avoid orphan ETL files/manifest update races.");

  return 0;
}
