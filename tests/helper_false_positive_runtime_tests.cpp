#include <Windows.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "HelperLog.h"
#include "HelperCommon.h"
#include "HelperMainInternal.h"
#include "HelperRuntimeTestUtils.h"
#include "EtwCapture.h"
#include "IncidentManifest.h"
#include "RetentionWorker.h"

using skydiag::helper::HelperConfig;
using skydiag::helper::internal::ClearLog;
using skydiag::helper::internal::CleanupCrashArtifactsAfterZeroExit;
using skydiag::helper::internal::CleanExitDumpState;
using skydiag::helper::internal::EtwFinalizeStatus;
using skydiag::helper::internal::FinalizeEtwCaptureWithCleanup;
using skydiag::helper::internal::CrashCaptureState;
using skydiag::helper::internal::HandleProcessExitTick;
using skydiag::helper::internal::LaunchDeferredViewersAfterExit;
using skydiag::helper::internal::MaybeStopPendingCrashEtwCapture;
using skydiag::helper::internal::PendingCrashEtwCapture;
using skydiag::helper::internal::ShutdownRetentionWorker;
using skydiag::helper::internal::StopEtwCaptureToPath;
using skydiag::helper::internal::CrashSummaryInfo;
using skydiag::helper::internal::TryWriteCleanExitEvidenceRecord;
using skydiag::helper::internal::TryUpdateIncidentManifestEtw;
using skydiag::helper::internal::UpdateCrashBucketStats;
using skydiag::helper::internal::WriteTextFileUtf8;
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

std::filesystem::path CurrentExecutablePath()
{
  std::vector<wchar_t> exePath(32768, L'\0');
  const DWORD length = GetModuleFileNameW(
    nullptr,
    exePath.data(),
    static_cast<DWORD>(exePath.size()));
  Require(length > 0 && length < exePath.size(), "GetModuleFileNameW failed");
  return std::filesystem::path(std::wstring(exePath.data(), length));
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
      {},
      CleanExitDumpState::kNotCaptured),
    "A failed metadata write must be reported to the caller");
  Require(
    !crashState.cleanExitEvidenceWritten,
    "A failed metadata write must not mark clean-exit evidence as available");

  std::filesystem::remove_all(tempRoot);
}

void TestAtomicTextWriteFailurePreservesExistingFile()
{
  const auto outBase = MakeTempDir(L"skydiag_atomic_text_write");
  const auto target = outBase / "state.json";
  WriteAllTextUtf8(target, "{\"state\":\"old\"}");

  HANDLE locked = CreateFileW(
    target.c_str(),
    GENERIC_READ,
    FILE_SHARE_READ,
    nullptr,
    OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  Require(locked != INVALID_HANDLE_VALUE, "Failed to lock state file against replacement");
  Require(
    !WriteTextFileUtf8(target, "{\"state\":\"new\"}"),
    "Atomic state write must report replace failure");
  CloseHandle(locked);

  Require(
    ReadAllTextUtf8(target) == "{\"state\":\"old\"}",
    "Failed atomic replacement must preserve the complete prior file");
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(outBase, ec)) {
    Require(!ec, "Failed to enumerate atomic write output");
    Require(
      entry.path().filename().wstring().find(L".tmp.") == std::wstring::npos,
      "Failed atomic replacement must clean its temporary file");
  }
  std::filesystem::remove_all(outBase);
}

void TestCorruptCrashBucketStatsAreQuarantinedBeforeRecovery()
{
  const auto outBase = MakeTempDir(L"skydiag_corrupt_bucket_stats");
  const auto statsPath = outBase / "SkyrimDiag_CrashBucketStats.json";
  WriteAllTextUtf8(statsPath, "{ definitely not valid json");

  CrashSummaryInfo info{};
  info.bucketKey = "test-bucket";
  info.unknownFaultModule = true;
  std::uint32_t unknownStreak = 0;
  std::uint32_t seenCount = 0;
  std::wstring err;
  Require(
    UpdateCrashBucketStats(outBase, info, &unknownStreak, &seenCount, &err),
    "Corrupt crash bucket stats should be quarantined and rebuilt");
  Require(unknownStreak == 1u && seenCount == 1u, "Rebuilt crash bucket counters must start at one");
  AssertContains(
    ReadAllTextUtf8(statsPath),
    "\"test-bucket\"",
    "Recovered crash bucket stats must contain the new bucket");

  bool foundQuarantine = false;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(outBase, ec)) {
    Require(!ec, "Failed to enumerate crash bucket quarantine output");
    const auto name = entry.path().filename().wstring();
    if (name.rfind(L"SkyrimDiag_CrashBucketStats.json.corrupt.", 0) == 0) {
      foundQuarantine = true;
      Require(
        ReadAllTextUtf8(entry.path()) == "{ definitely not valid json",
        "Quarantine must preserve the exact corrupt bytes");
    }
  }
  Require(foundQuarantine, "Corrupt crash bucket stats must be retained under a quarantine name");
  std::filesystem::remove_all(outBase);
}

void TestIncidentManifestUpdatePropagatesAtomicReplaceFailure()
{
  const auto outBase = MakeTempDir(L"skydiag_manifest_replace_failure");
  const auto manifestPath = outBase / "SkyrimDiag_Incident_Crash_test.json";
  const std::string original = "{\"artifacts\":{\"etw_status\":\"recording\"}}";
  WriteAllTextUtf8(manifestPath, original);

  HANDLE locked = CreateFileW(
    manifestPath.c_str(),
    GENERIC_READ,
    FILE_SHARE_READ,
    nullptr,
    OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  Require(locked != INVALID_HANDLE_VALUE, "Failed to lock manifest against replacement");
  std::wstring err;
  Require(
    !TryUpdateIncidentManifestEtw(
      manifestPath,
      outBase / "capture.etl",
      "written",
      &err),
    "Manifest updater must return failure when atomic replacement fails");
  Require(
    err.find(L"atomic write failed") != std::wstring::npos,
    "Manifest updater must explain its atomic write failure");
  CloseHandle(locked);

  Require(
    ReadAllTextUtf8(manifestPath) == original,
    "Failed manifest update must preserve the complete prior document");
  std::filesystem::remove_all(outBase);
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

void TestHandleProcessExitTick_DrainsLateZeroExitFaultMetadataWithoutDumpAttempt()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_late_zero_exit_drain");
  ClearLog(outBase);

  auto shared = MakeSharedLayout();
  shared->header.crash_seq = 2u;
  shared->header.crash.exception_code = 0xC0000005u;
  shared->header.crash.exception_addr = 0xDEADBEEFu;
  shared->header.state_flags = skydiag::kState_Frozen;

  auto child = LaunchSleepingChildProcess();
  shared->header.crash.faulting_tid = child.pi.dwProcessId;
  auto proc = MakeAttachedProcessForChild(child, shared.get());
  proc.crashEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
  Require(proc.crashEvent != nullptr, "CreateEventW for late zero-exit drain failed");

  HelperConfig cfg = MakeTestConfig();
  cfg.enableCleanExitEvidenceQuarantine = true;
  cfg.enableWerDumpFallbackHint = true;
  skydiag::helper::internal::HelperLoopState state{};

  Require(TerminateProcess(child.pi.hProcess, 0) != FALSE, "TerminateProcess(exit_code=0) failed");
  Require(
    WaitForSingleObject(child.pi.hProcess, 5000) == WAIT_OBJECT_0,
    "Late zero-exit child did not terminate");

  Require(
    HandleProcessExitTick(cfg, proc, outBase, &state),
    "Process exit tick must drain a late committed crash generation");
  Require(!state.crashCaptured.latched, "Metadata-only zero-exit drain must finish cleanup");

  const auto evidencePath = FindSingleFileByPrefix(
    outBase,
    L"SkyrimDiag_CleanExitEvidence_",
    L".json");
  const auto evidence = ReadAllTextUtf8(evidencePath);
  AssertContains(
    evidence,
    "\"dump_state\": \"not_captured\"",
    "Late zero-exit race must explicitly record that no dump could be captured");
  AssertContains(
    evidence,
    "strong_fault_published_after_final_event_poll_dump_not_captured",
    "Late zero-exit race must retain its causal reason");
  AssertContains(
    evidence,
    "\"exception_addr\": 3735928559",
    "Metadata-only drain must preserve immutable committed crash fields");
  Require(
    !FileExists(outBase / "SkyrimDiag_WER_LocalDumps_Hint.txt"),
    "Zero exit must not emit abnormal-exit WER guidance");

  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(outBase, ec)) {
    Require(!ec, "Failed to enumerate late zero-exit output");
    Require(entry.path().extension() != L".dmp", "Metadata-only drain must not attempt a dead-process dump");
  }
  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(
    log,
    "dump capture was not attempted",
    "Late zero-exit drain must explain the dead-process capture boundary");

  CloseHandle(proc.crashEvent);
  proc.crashEvent = nullptr;
  TerminateChildProcess(&child);
  std::filesystem::remove_all(outBase);
}

void TestHandleProcessExitTick_PreservesPendingEvidenceAfterFinalizationFailure()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_pending_evidence_drain");
  ClearLog(outBase);

  const auto pendingEvidencePath =
    outBase / L"SkyrimDiag_CleanExitEvidence_existing_pending.json";
  constexpr const char* kPendingEvidence =
    "{\n"
    "  \"schema\": \"skydiag.clean_exit_evidence.v1\",\n"
    "  \"dump_state\": \"pending_delete\",\n"
    "  \"crash_seq\": 2\n"
    "}\n";
  WriteAllTextUtf8(pendingEvidencePath, kPendingEvidence);
  HANDLE lockedEvidence = CreateFileW(
    pendingEvidencePath.c_str(),
    GENERIC_READ,
    FILE_SHARE_READ,
    nullptr,
    OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  Require(
    lockedEvidence != INVALID_HANDLE_VALUE,
    "Failed to lock pending evidence against final atomic replacement");

  auto shared = MakeSharedLayout();
  shared->header.crash_seq = 2u;
  shared->header.crash.exception_code = 0xC0000005u;
  shared->header.crash.exception_addr = 0xAABBCCDDu;
  shared->header.state_flags = skydiag::kState_Frozen;

  auto child = LaunchSleepingChildProcess();
  shared->header.crash.faulting_tid = child.pi.dwThreadId;
  auto proc = MakeAttachedProcessForChild(child, shared.get());

  HelperConfig cfg = MakeTestConfig();
  cfg.enableCleanExitEvidenceQuarantine = true;
  skydiag::helper::internal::HelperLoopState state{};
  state.crashCaptured.capturedInfo =
    skydiag::helper::internal::BuildCrashEventInfo(
      0xC0000005u,
      0xAABBCCDDu,
      child.pi.dwThreadId,
      skydiag::kState_Frozen);
  state.crashCaptured.capturedInfo.crashSeq = 2u;
  // This is the durable state left when pending_delete was committed but the
  // final atomic replacement failed after the dump cleanup attempt.
  state.crashCaptured.cleanExitEvidenceWritten = true;
  state.crashCaptured.cleanExitEvidenceFinalized = false;
  state.crashCaptured.cleanExitEvidencePath = pendingEvidencePath;

  Require(
    TerminateProcess(child.pi.hProcess, 0) != FALSE,
    "TerminateProcess(exit_code=0) failed");
  Require(
    WaitForSingleObject(child.pi.hProcess, 5000) == WAIT_OBJECT_0,
    "Pending-evidence child did not terminate");
  Require(
    HandleProcessExitTick(cfg, proc, outBase, &state),
    "Process exit tick must handle the pending-evidence state");

  CloseHandle(lockedEvidence);
  Require(
    state.crashCaptured.cleanExitEvidencePath == pendingEvidencePath,
    "Post-exit drain must retain the authoritative pending evidence path");
  Require(
    ReadAllTextUtf8(pendingEvidencePath) == kPendingEvidence,
    "Post-exit drain must not overwrite pending_delete with not_captured");

  std::size_t evidenceCount = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(outBase, ec)) {
    Require(!ec, "Failed to enumerate pending evidence output");
    const auto filename = entry.path().filename().wstring();
    if (filename.rfind(L"SkyrimDiag_CleanExitEvidence_", 0) == 0 &&
        entry.path().extension() == L".json") {
      ++evidenceCount;
    }
  }
  Require(
    evidenceCount == 1u,
    "Post-exit drain must not create a second not_captured sidecar for the same crash sequence");
  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(
    log,
    "post-exit drain will not relabel it as not_captured",
    "The helper must explain why pending evidence remains authoritative");

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

void TestCleanExitDumpDeletionFailureIsRecordedAsPreserved()
{
  const auto outBase = MakeTempDir(L"skydiag_clean_exit_delete_failure");
  ClearLog(outBase);
  const auto dumpPath = outBase / "SkyrimDiag_Crash_20260315_150000_001.dmp";
  WriteAllTextUtf8(dumpPath, "locked filtered dump");

  HANDLE lockedDump = CreateFileW(
    dumpPath.c_str(),
    GENERIC_READ,
    FILE_SHARE_READ,
    nullptr,
    OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  Require(lockedDump != INVALID_HANDLE_VALUE, "Failed to lock filtered dump against deletion");

  HelperConfig cfg = MakeTestConfig();
  cfg.enableCleanExitEvidenceQuarantine = true;
  skydiag::helper::AttachedProcess proc{};
  skydiag::helper::internal::HelperLoopState state{};
  state.crashCaptured.latched = true;
  state.crashCaptured.capturedInfo =
    skydiag::helper::internal::BuildCrashEventInfo(
      0xC0000005u,
      0xABCDEF01u,
      88u,
      skydiag::kState_Frozen);
  state.crashCaptured.capturedInfo.crashSeq = 2u;
  state.capturedCrashDumpPath = dumpPath.wstring();

  CleanupCrashArtifactsAfterZeroExit(cfg, proc, outBase, &state);
  Require(FileExists(dumpPath), "A dump whose deletion failed must remain on disk");
  Require(!state.crashCaptured.latched, "Deletion failure must not keep the active capture latch");

  const auto evidencePath = FindSingleFileByPrefix(
    outBase,
    L"SkyrimDiag_CleanExitEvidence_",
    L".json");
  const auto evidence = ReadAllTextUtf8(evidencePath);
  AssertContains(
    evidence,
    "\"dump_state\": \"delete_failed\"",
    "Evidence must report the observed deletion failure");
  AssertContains(
    evidence,
    "\"dump_preserved\": true",
    "Deletion failure must never claim that the dump was discarded");
  AssertContains(
    evidence,
    "\"dump_filename\": \"SkyrimDiag_Crash_20260315_150000_001.dmp\"",
    "Evidence must identify the retained dump without exposing an absolute path");

  CloseHandle(lockedDump);
  std::filesystem::remove_all(outBase);
}

void TestEtwStopFailureRetriesThenConfirmsCancellation()
{
  const auto outBase = MakeTempDir(L"skydiag_etw_cleanup_retry");
  ClearLog(outBase);

  HelperConfig cfg = MakeTestConfig();
  cfg.etwWprExe = CurrentExecutablePath().wstring();
  cfg.enableIncidentManifest = false;

  PendingCrashEtwCapture pending{};
  pending.active = true;
  pending.etwPath = outBase / "synthetic.etl";
  pending.startedAtTick64 = 0;
  pending.captureSeconds = 1u;
  skydiag::helper::AttachedProcess proc{};

  MaybeStopPendingCrashEtwCapture(
    cfg,
    proc,
    outBase,
    /*force=*/true,
    &pending);
  Require(!pending.active, "Confirmed wpr -cancel must clear the active crash ETW state");
  Require(!pending.cleanupPending, "Confirmed wpr -cancel must clear cleanup-pending state");
  Require(pending.stopAttempts == 3u, "Crash ETW cleanup must exhaust its bounded stop retries");

  std::wstring finalizeErr;
  const auto hangFinalize = FinalizeEtwCaptureWithCleanup(
    cfg,
    outBase,
    outBase / "synthetic-hang.etl",
    /*maxStopAttempts=*/3u,
    &finalizeErr);
  Require(
    hangFinalize == EtwFinalizeStatus::kCancelledAfterStopFailure,
    "Synchronous hang ETW cleanup must confirm cancellation after bounded stop failures");

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(
    log,
    "WPR cancellation was confirmed",
    "Crash ETW cleanup must log confirmed cancellation instead of forgetting the session");
  ShutdownRetentionWorker();
  std::filesystem::remove_all(outBase);
}

void TestEtwStopExitZeroRequiresNonEmptyArtifact()
{
  const auto outBase = MakeTempDir(L"skydiag_etw_artifact_readback");
  HelperConfig cfg = MakeTestConfig();
  cfg.etwWprExe = CurrentExecutablePath().wstring();

  std::wstring err;
  const auto missingPath = outBase / L"exit0-no-file.etl";
  Require(
    !StopEtwCaptureToPath(cfg, outBase, missingPath, &err),
    "wpr exit 0 without an ETL file must not be reported as written");
  Require(
    err.find(L"did not create the ETL file") != std::wstring::npos,
    "Missing ETL readback failure must explain that no artifact was created");

  const auto emptyPath = outBase / L"exit0-empty.etl";
  Require(
    !StopEtwCaptureToPath(cfg, outBase, emptyPath, &err),
    "wpr exit 0 with an empty ETL file must not be reported as written");
  Require(
    err.find(L"empty or unreadable") != std::wstring::npos,
    "Empty ETL readback failure must identify the unusable artifact");

  const auto validPath = outBase / L"exit0-valid.etl";
  Require(
    StopEtwCaptureToPath(cfg, outBase, validPath, &err),
    "wpr exit 0 with a non-empty ETL file must be accepted");
  Require(err.empty(), "Successful ETL readback must clear the prior failure");
  Require(
    std::filesystem::file_size(validPath) > 0u,
    "Accepted ETL artifact must be non-empty");

  const auto stalePath = outBase / L"exit0-stale.etl";
  constexpr const char* kStalePayload = "stale etl from a prior capture";
  WriteAllTextUtf8(stalePath, kStalePayload);
  Require(
    !StopEtwCaptureToPath(cfg, outBase, stalePath, &err),
    "wpr exit 0 without changing a pre-existing ETL must not report a fresh artifact");
  Require(
    err.find(L"pre-existing ETL file was unchanged") != std::wstring::npos,
    "Stale ETL rejection must explain that freshness could not be confirmed");
  Require(
    ReadAllTextUtf8(stalePath) == kStalePayload,
    "Stale ETL freshness validation must not destroy the prior artifact");

  std::filesystem::remove_all(outBase);
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
  try {
    if (argc >= 2 && std::wstring_view(argv[1]) == L"-stop") {
      if (argc >= 3) {
        const std::filesystem::path etlPath(argv[2]);
        const auto filename = etlPath.filename().wstring();
        if (filename == L"exit0-no-file.etl") {
          return 0;
        }
        if (filename == L"exit0-empty.etl") {
          WriteAllTextUtf8(etlPath, "");
          return 0;
        }
        if (filename == L"exit0-valid.etl") {
          WriteAllTextUtf8(etlPath, "synthetic etl payload");
          return 0;
        }
        if (filename == L"exit0-stale.etl") {
          return 0;
        }
      }
      return 7;
    }
    if (argc >= 2 && std::wstring_view(argv[1]) == L"-cancel") {
      return 0;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--delayed-artifact-writer") {
      Sleep(500);
      WriteAllTextUtf8(std::filesystem::path(argv[2]), "late summary");
      return 0;
    }

    TestCleanExitEvidenceWriteFailureDoesNotClaimSuccess();
    TestAtomicTextWriteFailurePreservesExistingFile();
    TestCorruptCrashBucketStatsAreQuarantinedBeforeRecovery();
    TestIncidentManifestUpdatePropagatesAtomicReplaceFailure();
    TestCleanupCrashArtifactsAfterZeroExit_RemovesHandledAccessViolationArtifacts();
    TestCleanupCrashArtifactsAfterZeroExit_StopsDelayedAnalyzerWriter();
    TestLaunchDeferredViewersAfterExit_SuppressesOnNormalExit();
    TestHandleProcessExitTick_TreatsStrongSharedMemoryExceptionAsHandledOnZeroExit();
    TestHandleProcessExitTick_DrainsLateZeroExitFaultMetadataWithoutDumpAttempt();
    TestHandleProcessExitTick_PreservesPendingEvidenceAfterFinalizationFailure();
    TestPreservedFilteredDump_DoesNotKeepCaptureLatch();
    TestCleanExitDumpDeletionFailureIsRecordedAsPreserved();
    TestEtwStopFailureRetriesThenConfirmsCancellation();
    TestEtwStopExitZeroRequiresNonEmptyArtifact();
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
