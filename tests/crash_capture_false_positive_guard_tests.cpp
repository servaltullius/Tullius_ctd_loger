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

  assert(std::filesystem::exists(processAttachPath) && "helper/src/ProcessAttach.cpp not found");
  assert(std::filesystem::exists(crashCapturePath) && "helper/src/CrashCapture.cpp not found");
  assert(std::filesystem::exists(helperMainPath) && "helper/src/main.cpp not found");

  const std::string processAttach = ReadAllText(processAttachPath);
  const std::string crashCapture = ReadAllText(crashCapturePath);
  const std::string helperMain = ReadAllText(helperMainPath);

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
    "suppressCrashAutomationForLikelyShutdownException",
    "Crash capture must keep a dedicated suppression flag for shutdown/menu boundary auto-actions.");

  AssertContains(
    crashCapture,
    "near menu/shutdown boundary during heartbeat check",
    "Crash capture must suppress delayed menu-exit false positives discovered during heartbeat checks.");

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
