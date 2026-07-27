#include <Windows.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "HelperLog.h"
#include "HelperMainInternal.h"
#include "HelperRuntimeTestUtils.h"

using skydiag::helper::HelperConfig;
using skydiag::helper::internal::ClearLog;
using skydiag::helper::internal::CleanupCrashArtifactsAfterZeroExit;
using skydiag::helper::internal::CrashCaptureState;
using skydiag::helper::internal::HandleProcessExitTick;
using skydiag::helper::internal::LaunchDeferredViewersAfterExit;
using skydiag::helper::internal::TryWriteCleanExitEvidenceRecord;
using skydiag::tests::runtime::AssertContains;
using skydiag::tests::runtime::FileExists;
using skydiag::tests::runtime::FindSingleFileByPrefix;
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

std::wstring QuoteArg(std::wstring_view value)
{
  std::wstring quoted = L"\"";
  quoted.append(value);
  quoted += L"\"";
  return quoted;
}

HANDLE LaunchDelayedArtifactWriter(const std::filesystem::path& artifactPath)
{
  std::vector<wchar_t> exePath(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, exePath.data(), static_cast<DWORD>(exePath.size()));
  Require(length > 0 && length < exePath.size(), "GetModuleFileNameW failed");

  std::wstring command = QuoteArg(std::wstring(exePath.data(), length));
  command += L" --delayed-artifact-writer ";
  command += QuoteArg(artifactPath.wstring());

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  Require(
    CreateProcessW(
      exePath.data(),
      command.data(),
      nullptr,
      nullptr,
      FALSE,
      CREATE_NO_WINDOW,
      nullptr,
      nullptr,
      &startup,
      &process) != FALSE,
    "CreateProcessW for delayed artifact writer failed");
  CloseHandle(process.hThread);
  return process.hProcess;
}

void TestCleanExitEvidenceWriteFailureDoesNotClaimSuccess()
{
  const auto tempRoot = MakeTempDir(L"skydiag_clean_exit_write_failure");
  const auto blockedOutputBase = tempRoot / L"not_a_directory";
  WriteAllTextUtf8(blockedOutputBase, "blocks child file creation");

  HelperConfig cfg = MakeTestConfig();
  cfg.enableCleanExitEvidenceQuarantine = true;

  CrashCaptureState crashState{};
  crashState.capturedInfo = skydiag::helper::internal::BuildCrashEventInfo(
    0xC0000005u,
    0x12345678u,
    42u,
    skydiag::kState_Frozen);
  crashState.capturedInfo.crashSeq = 2u;

  Require(
    !TryWriteCleanExitEvidenceRecord(
      cfg,
      blockedOutputBase,
      &crashState,
      L"write_failure_test",
      /*dumpPreserved=*/false),
    "A failed metadata write must be reported to the caller");
  Require(
    !crashState.cleanExitEvidenceWritten,
    "A failed metadata write must not mark clean-exit evidence as available");

  std::filesystem::remove_all(tempRoot);
}

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
  state.crashCaptured.latched = true;
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
  Require(!state.crashCaptured.latched, "Handled AV with exit_code=0 must clear crashCaptured state");
  Require(!state.pendingCrashAnalysis.active, "Handled AV with exit_code=0 must cancel pending analysis");
  Require(state.capturedCrashDumpPath.empty(), "Handled AV with exit_code=0 must clear captured dump path");
  Require(state.pendingCrashViewerDumpPath.empty(), "Handled AV with exit_code=0 must clear deferred viewer path");

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "removed", "Handled AV zero-exit cleanup must log artifact removal");

  std::filesystem::remove_all(outBase);
}

void TestCleanupCrashArtifactsAfterZeroExit_StopsDelayedAnalyzerWriter()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_false_positive_delayed_writer");
  ClearLog(outBase);

  const auto dumpPath = outBase / "SkyrimDiag_Crash_20260315_131500_001.dmp";
  const auto summaryPath = outBase / "SkyrimDiag_Crash_20260315_131500_001_SkyrimDiagSummary.json";
  WriteAllTextUtf8(dumpPath, "dump");

  HelperConfig cfg = MakeTestConfig();
  skydiag::helper::AttachedProcess proc{};
  skydiag::helper::internal::HelperLoopState state{};
  state.crashCaptured.latched = true;
  state.capturedCrashDumpPath = dumpPath.wstring();
  state.pendingCrashAnalysis.active = true;
  state.pendingCrashAnalysis.dumpPath = dumpPath.wstring();
  state.pendingCrashAnalysis.process = LaunchDelayedArtifactWriter(summaryPath);

  CleanupCrashArtifactsAfterZeroExit(cfg, proc, outBase, &state);
  Sleep(750);

  Require(!FileExists(dumpPath), "Zero-exit cleanup must remove the captured dump");
  Require(!FileExists(summaryPath), "Stopped analyzer must not recreate summary after zero-exit cleanup");
  Require(!state.pendingCrashAnalysis.active, "Zero-exit cleanup must clear tracked analyzer state");

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
  state.crashCaptured.latched = true;
  state.crashCaptured.capturedInfo =
    skydiag::helper::internal::BuildCrashEventInfo(
      0xC0000005u,
      0x12345678u,
      child.pi.dwThreadId,
      skydiag::kState_Frozen | skydiag::kState_InMenu);
  state.crashCaptured.capturedInfo.crashSeq = 2u;
  state.capturedCrashDumpPath = dumpPath.wstring();
  state.pendingCrashViewerDumpPath = dumpPath.wstring();

  Require(TerminateProcess(child.pi.hProcess, 0) != FALSE, "TerminateProcess(exit_code=0) failed");
  Require(WaitForSingleObject(child.pi.hProcess, 5000) == WAIT_OBJECT_0, "Zero-exit child did not terminate");

  Require(
    HandleProcessExitTick(cfg, proc, outBase, &state),
    "Process exit tick must handle a signaled zero-exit process");
  Require(!FileExists(dumpPath), "Strong shared-memory exception with exit_code=0 must delete dump");
  Require(!FileExists(reportPath), "Strong shared-memory exception with exit_code=0 must delete report");
  Require(!state.crashCaptured.latched, "Strong shared-memory exception with exit_code=0 must clear capture state");
  Require(state.pendingCrashViewerDumpPath.empty(), "Strong shared-memory exception with exit_code=0 must suppress viewer");
  Require(
    !FileExists(outBase / "SkyrimDiag_WER_LocalDumps_Hint.txt"),
    "Zero exit must not emit abnormal-exit WER guidance");

  const auto evidencePath = FindSingleFileByPrefix(
    outBase,
    L"SkyrimDiag_CleanExitEvidence_",
    L".json");
  const auto evidence = ReadAllTextUtf8(evidencePath);
  AssertContains(
    evidence,
    "strong_fault_published_but_process_exited_zero_dump_discarded",
    "Late zero-exit cleanup must preserve metadata for the captured strong fault");
  AssertContains(evidence, "\"dump_preserved\": false", "Discarded dump state must be explicit");
  AssertContains(evidence, "\"filter_context\": \"process_exit\"", "Late exit context must be explicit");
  AssertContains(evidence, "\"exception_addr\": 305419896", "Evidence must use the capture-time snapshot");

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
  state.crashCaptured.latched = true;
  state.crashCaptured.capturedInfo =
    skydiag::helper::internal::BuildCrashEventInfo(
      0xC0000005u,
      0x87654321u,
      77u,
      skydiag::kState_Frozen);
  state.crashCaptured.capturedInfo.crashSeq = 2u;
  state.capturedCrashDumpPath = dumpPath.wstring();
  state.pendingCrashViewerDumpPath = dumpPath.wstring();

  CleanupCrashArtifactsAfterZeroExit(cfg, proc, outBase, &state);

  Require(FileExists(dumpPath), "PreserveFilteredCrashDumps must keep the filtered dump file");
  Require(!FileExists(reportPath), "PreserveFilteredCrashDumps must remove derived reports");
  Require(!FileExists(pluginScanPath), "PreserveFilteredCrashDumps must remove plugin-scan sidecars");
  Require(!state.crashCaptured.latched, "Preserved filtered dump must not block a later real CTD capture");
  Require(state.capturedCrashDumpPath.empty(), "Preserved filtered dump must detach from active crash state");
  Require(state.pendingCrashViewerDumpPath.empty(), "Preserved filtered dump must not auto-open as a real CTD");

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "without keeping the crashCaptured latch", "Preserve behavior must be explicit in helper log");

  const auto evidencePath = FindSingleFileByPrefix(
    outBase,
    L"SkyrimDiag_CleanExitEvidence_",
    L".json");
  const auto evidence = ReadAllTextUtf8(evidencePath);
  AssertContains(
    evidence,
    "strong_fault_published_but_process_exited_zero_dump_preserved",
    "Preserved filtered dump must have a preservation-specific reason");
  AssertContains(evidence, "\"dump_preserved\": true", "Preserved dump state must be explicit");
  Require(
    evidence.find("Set PreserveFilteredCrashDumps=1") == std::string::npos,
    "Evidence must not tell the user to enable an option that is already active");

  // Cleanup is idempotent after the latch clears and must not create another
  // metadata record for the same captured generation.
  CleanupCrashArtifactsAfterZeroExit(cfg, proc, outBase, &state);
  Require(
    FindSingleFileByPrefix(outBase, L"SkyrimDiag_CleanExitEvidence_", L".json") == evidencePath,
    "Repeated cleanup must not duplicate clean-exit evidence");

  std::filesystem::remove_all(outBase);
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
  try {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--delayed-artifact-writer") {
      Sleep(500);
      WriteAllTextUtf8(std::filesystem::path(argv[2]), "late summary");
      return 0;
    }

    TestCleanExitEvidenceWriteFailureDoesNotClaimSuccess();
    TestCleanupCrashArtifactsAfterZeroExit_RemovesHandledAccessViolationArtifacts();
    TestCleanupCrashArtifactsAfterZeroExit_StopsDelayedAnalyzerWriter();
    TestLaunchDeferredViewersAfterExit_SuppressesOnNormalExit();
    TestHandleProcessExitTick_TreatsStrongSharedMemoryExceptionAsHandledOnZeroExit();
    TestPreservedFilteredDump_DoesNotKeepCaptureLatch();
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
