#include <Windows.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>

#include "HelperLog.h"
#include "HelperMainInternal.h"
#include "HelperRuntimeTestUtils.h"

using skydiag::helper::HelperConfig;
using skydiag::helper::internal::ClearLog;
using skydiag::helper::internal::CleanupCrashArtifactsAfterZeroExit;
using skydiag::helper::internal::HandleProcessExitTick;
using skydiag::helper::internal::LaunchDeferredViewersAfterExit;
using skydiag::tests::runtime::AssertContains;
using skydiag::tests::runtime::FileExists;
using skydiag::tests::runtime::LaunchSleepingChildProcess;
using skydiag::tests::runtime::MakeAttachedProcessForChild;
using skydiag::tests::runtime::MakeSharedLayout;
using skydiag::tests::runtime::MakeTempDir;
using skydiag::tests::runtime::MakeTestConfig;
using skydiag::tests::runtime::ReadAllTextUtf8;
using skydiag::tests::runtime::Require;
using skydiag::tests::runtime::TerminateChildProcess;
using skydiag::tests::runtime::WriteAllTextUtf8;

namespace {

void TestCleanupCrashArtifactsAfterZeroExit_RemovesHandledAccessViolationArtifacts()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_false_positive_cleanup");
  ClearLog(outBase);

  const auto dumpPath = outBase / "SkyrimDiag_Crash_20260315_130000_001.dmp";
  const auto stem = dumpPath.stem().wstring();
  const auto etwPath = outBase / (stem + L".etl");
  const auto reportPath = outBase / (stem + L"_SkyrimDiagReport.txt");
  const auto summaryPath = outBase / (stem + L"_SkyrimDiagSummary.json");
  const auto blackboxPath = outBase / (stem + L"_SkyrimDiagBlackbox.jsonl");
  const auto pluginScanPath = outBase / (stem + L"_PluginScan.json");
  const auto manifestPath = outBase / "SkyrimDiag_Incident_Crash_20260315_130000_001.json";

  WriteAllTextUtf8(dumpPath, "dump");
  WriteAllTextUtf8(etwPath, "etl");
  WriteAllTextUtf8(reportPath, "report");
  WriteAllTextUtf8(summaryPath, "summary");
  WriteAllTextUtf8(blackboxPath, "blackbox");
  WriteAllTextUtf8(pluginScanPath, "plugin scan");
  WriteAllTextUtf8(manifestPath, "manifest");

  HelperConfig cfg = MakeTestConfig();
  skydiag::helper::AttachedProcess proc{};
  skydiag::helper::internal::HelperLoopState state{};
  state.crashCaptured = true;
  state.capturedCrashDumpPath = dumpPath.wstring();
  state.pendingCrashViewerDumpPath = dumpPath.wstring();
  state.pendingCrashEtw.etwPath = etwPath;
  state.pendingCrashAnalysis.active = true;

  CleanupCrashArtifactsAfterZeroExit(cfg, proc, outBase, &state);

  Require(!FileExists(dumpPath), "Handled AV with exit_code=0 must delete dump");
  Require(!FileExists(etwPath), "Handled AV with exit_code=0 must delete ETW sidecar");
  Require(!FileExists(reportPath), "Handled AV with exit_code=0 must delete report");
  Require(!FileExists(summaryPath), "Handled AV with exit_code=0 must delete summary");
  Require(!FileExists(blackboxPath), "Handled AV with exit_code=0 must delete blackbox");
  Require(!FileExists(pluginScanPath), "Handled AV with exit_code=0 must delete plugin scan");
  Require(!FileExists(manifestPath), "Handled AV with exit_code=0 must delete incident manifest");
  Require(!state.crashCaptured, "Handled AV with exit_code=0 must clear crashCaptured state");
  Require(!state.pendingCrashAnalysis.active, "Handled AV with exit_code=0 must cancel pending analysis");
  Require(state.capturedCrashDumpPath.empty(), "Handled AV with exit_code=0 must clear captured dump path");
  Require(state.pendingCrashViewerDumpPath.empty(), "Handled AV with exit_code=0 must clear deferred viewer path");

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "removed", "Handled AV zero-exit cleanup must log artifact removal");

  std::filesystem::remove_all(outBase);
}

void TestLaunchDeferredViewersAfterExit_SuppressesOnNormalExit()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_false_positive_viewer");
  ClearLog(outBase);

  HelperConfig cfg = MakeTestConfig();
  cfg.autoOpenViewerOnCrash = true;

  skydiag::helper::internal::HelperLoopState state{};
  state.pendingCrashViewerDumpPath = L"C:\\Temp\\SyntheticCrash.dmp";

  LaunchDeferredViewersAfterExit(
    cfg,
    outBase,
    /*exitCode=*/0,
    &state);

  Require(state.pendingCrashViewerDumpPath.empty(), "Normal exit must suppress deferred crash viewer");

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(
    log,
    "Suppressed deferred crash viewer launch on normal process exit",
    "Normal exit must log deferred crash viewer suppression");

  std::filesystem::remove_all(outBase);
}

void TestHandleProcessExitTick_TreatsStrongSharedMemoryExceptionAsHandledOnZeroExit()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_false_positive_exit_tick");
  ClearLog(outBase);

  const auto dumpPath = outBase / "SkyrimDiag_Crash_20260315_133000_001.dmp";
  const auto reportPath = outBase / "SkyrimDiag_Crash_20260315_133000_001_SkyrimDiagReport.txt";
  WriteAllTextUtf8(dumpPath, "dump");
  WriteAllTextUtf8(reportPath, "report");

  auto shared = MakeSharedLayout();
  shared->header.crash_seq = 2u;
  shared->header.crash.exception_code = 0xC0000005u;
  shared->header.state_flags = skydiag::kState_Frozen | skydiag::kState_InMenu;

  auto child = LaunchSleepingChildProcess();
  auto proc = MakeAttachedProcessForChild(child, shared.get());

  HelperConfig cfg = MakeTestConfig();
  cfg.autoOpenViewerOnCrash = true;
  cfg.enableWerDumpFallbackHint = true;

  skydiag::helper::internal::HelperLoopState state{};
  state.crashCaptured = true;
  state.capturedCrashDumpPath = dumpPath.wstring();
  state.pendingCrashViewerDumpPath = dumpPath.wstring();

  Require(TerminateProcess(child.pi.hProcess, 0) != FALSE, "TerminateProcess(exit_code=0) failed");
  Require(WaitForSingleObject(child.pi.hProcess, 5000) == WAIT_OBJECT_0, "Zero-exit child did not terminate");

  Require(
    HandleProcessExitTick(cfg, proc, outBase, &state),
    "Process exit tick must handle a signaled zero-exit process");
  Require(!FileExists(dumpPath), "Strong shared-memory exception with exit_code=0 must delete dump");
  Require(!FileExists(reportPath), "Strong shared-memory exception with exit_code=0 must delete report");
  Require(!state.crashCaptured, "Strong shared-memory exception with exit_code=0 must clear capture state");
  Require(state.pendingCrashViewerDumpPath.empty(), "Strong shared-memory exception with exit_code=0 must suppress viewer");
  Require(
    !FileExists(outBase / "SkyrimDiag_WER_LocalDumps_Hint.txt"),
    "Zero exit must not emit abnormal-exit WER guidance");

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(
    log,
    "Process exited normally (exit_code=0)",
    "Strong shared-memory exception with zero exit must be classified as normal termination");
  Require(
    log.find("Deferred crash viewer launched") == std::string::npos,
    "Strong shared-memory exception with zero exit must not launch a deferred viewer");

  TerminateChildProcess(&child);
  std::filesystem::remove_all(outBase);
}

void TestPreservedFilteredDump_DoesNotKeepCaptureLatch()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_false_positive_preserve_no_latch");
  ClearLog(outBase);

  const auto dumpPath = outBase / "SkyrimDiag_Crash_20260315_140000_001.dmp";
  const auto reportPath = outBase / "SkyrimDiag_Crash_20260315_140000_001_SkyrimDiagReport.txt";
  const auto pluginScanPath = outBase / "SkyrimDiag_Crash_20260315_140000_001_PluginScan.json";
  WriteAllTextUtf8(dumpPath, "filtered dump");
  WriteAllTextUtf8(reportPath, "filtered report");
  WriteAllTextUtf8(pluginScanPath, "filtered plugin scan");

  HelperConfig cfg = MakeTestConfig();
  cfg.preserveFilteredCrashDumps = true;
  skydiag::helper::AttachedProcess proc{};
  skydiag::helper::internal::HelperLoopState state{};
  state.crashCaptured = true;
  state.capturedCrashDumpPath = dumpPath.wstring();
  state.pendingCrashViewerDumpPath = dumpPath.wstring();

  CleanupCrashArtifactsAfterZeroExit(cfg, proc, outBase, &state);

  Require(FileExists(dumpPath), "PreserveFilteredCrashDumps must keep the filtered dump file");
  Require(!FileExists(reportPath), "PreserveFilteredCrashDumps must remove derived reports");
  Require(!FileExists(pluginScanPath), "PreserveFilteredCrashDumps must remove plugin-scan sidecars");
  Require(!state.crashCaptured, "Preserved filtered dump must not block a later real CTD capture");
  Require(state.capturedCrashDumpPath.empty(), "Preserved filtered dump must detach from active crash state");
  Require(state.pendingCrashViewerDumpPath.empty(), "Preserved filtered dump must not auto-open as a real CTD");

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "without keeping the crashCaptured latch", "Preserve behavior must be explicit in helper log");

  std::filesystem::remove_all(outBase);
}

}  // namespace

int main()
{
  try {
    TestCleanupCrashArtifactsAfterZeroExit_RemovesHandledAccessViolationArtifacts();
    TestLaunchDeferredViewersAfterExit_SuppressesOnNormalExit();
    TestHandleProcessExitTick_TreatsStrongSharedMemoryExceptionAsHandledOnZeroExit();
    TestPreservedFilteredDump_DoesNotKeepCaptureLatch();
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
