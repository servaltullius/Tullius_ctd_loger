#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
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
using skydiag::helper::internal::HandleCrashEventTick;
using skydiag::helper::internal::HandleProcessExitTick;
using skydiag::helper::internal::LaunchDeferredViewersAfterExit;
using skydiag::helper::internal::MaybeStopPendingCrashEtwCapture;
using skydiag::helper::internal::NoteCrashLoggerThreadDumpHotkeyPressed;
using skydiag::helper::internal::PendingCrashEtwCapture;
using skydiag::helper::internal::ShutdownRetentionWorker;
using skydiag::helper::internal::StopEtwCaptureToPath;
using skydiag::helper::internal::CrashSummaryInfo;
using skydiag::helper::internal::TryLoadCrashSummaryInfo;
using skydiag::helper::internal::TryWriteCleanExitEvidenceRecord;
using skydiag::helper::internal::TryUpdateIncidentManifestEtw;
using skydiag::helper::internal::UpdateCrashBucketStats;
using skydiag::helper::internal::WerFallbackReason;
using skydiag::helper::internal::WriteTextFileUtf8;
using skydiag::helper::internal::WriteWerFallbackHint;
using skydiag::tests::runtime::AssertContains;
using skydiag::tests::runtime::ChildProcess;
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

// Values of the wrong type (a hand edit, another version) read as missing:
// nlohmann's value() would throw, and nothing above the helper loop catches.
void TestWrongTypesInCrashBucketStatsAndSummaryAreDefaults()
{
  const auto outBase = MakeTempDir(L"skydiag_wrong_type_bucket_stats");
  const auto statsPath = outBase / "SkyrimDiag_CrashBucketStats.json";
  WriteAllTextUtf8(statsPath,
    "{\"version\":1,\"buckets\":{\"test-bucket\":{\"seen_total\":null,\"unknown_total\":\"3\",\"unknown_streak\":-1}}}");

  CrashSummaryInfo info{};
  info.bucketKey = "test-bucket";
  info.unknownFaultModule = true;
  std::uint32_t unknownStreak = 0;
  std::uint32_t seenCount = 0;
  std::wstring err;
  Require(UpdateCrashBucketStats(outBase, info, &unknownStreak, &seenCount, &err),
    "Wrong-typed bucket counters must not throw");
  Require(unknownStreak == 1u && seenCount == 1u, "Wrong-typed bucket counters restart at zero");

  const auto summaryPath = outBase / "SkyrimDiag_Crash_20260101_000000_000_SkyrimDiagSummary.json";
  WriteAllTextUtf8(summaryPath,
    "{\"schema\":{\"version\":\"2\"},\"crash_bucket_key\":7,\"exception\":{\"module_plus_offset\":null},"
    "\"actionable_candidates\":[{\"status_id\":3}],\"first_chance_context\":{\"repeated_signature_count\":\"x\"}}");
  CrashSummaryInfo loaded{};
  Require(TryLoadCrashSummaryInfo(summaryPath, &loaded, &err), "Wrong-typed summary fields must not throw");
  Require(loaded.schemaVersion == 1u, "A non-numeric schema version reads as the default");
  Require(loaded.bucketKey.empty(), "A non-string bucket key reads as none");
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

// EnableAutoRecaptureOnUnknownCrash wrote a full dump of the same crash; it
// is a handled exception too once the game exits with code 0.
void TestCleanupCrashArtifactsAfterZeroExit_RemovesItsRecapture()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_false_positive_recapture");
  ClearLog(outBase);

  const auto dumpPath = outBase / "SkyrimDiag_Crash_20260315_130000_001.dmp";
  const auto recapturePath = outBase / "SkyrimDiag_Crash_20260315_130030_002_Full.dmp";
  const auto recaptureReport = outBase / "SkyrimDiag_Crash_20260315_130030_002_Full_SkyrimDiagReport.txt";
  const auto recaptureManifest = outBase / "SkyrimDiag_Incident_CrashRecapture_20260315_130030_002.json";
  WriteAllTextUtf8(dumpPath, "dump");
  WriteAllTextUtf8(recapturePath, "full dump");
  WriteAllTextUtf8(recaptureReport, "report");
  WriteAllTextUtf8(recaptureManifest, "manifest");

  HelperConfig cfg = MakeTestConfig();
  skydiag::helper::AttachedProcess proc{};
  skydiag::helper::internal::HelperLoopState state{};
  state.crashCaptured.latched = true;
  state.capturedCrashDumpPath = dumpPath.wstring();
  state.crashRecapture.dump = recapturePath;
  state.crashRecapture.manifest = recaptureManifest;

  CleanupCrashArtifactsAfterZeroExit(cfg, proc, outBase, &state);

  Require(!FileExists(dumpPath), "the captured dump is removed");
  Require(!FileExists(recapturePath), "its recapture is removed");
  Require(!FileExists(recaptureReport), "the recapture's report is removed");
  Require(!FileExists(recaptureManifest), "the recapture manifest is removed");
  Require(state.crashRecapture.dump.empty(), "the recapture is forgotten");
  AssertContains(ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log"), "removed recapture dump",
    "the removal is logged");

  // A session without a captured exception has nothing to classify as handled.
  ClearLog(outBase);
  skydiag::helper::internal::AppendExitClassificationLog(outBase, /*hadCapturedCrash=*/false);
  std::error_code ec;
  Require(!std::filesystem::exists(outBase / "SkyrimDiagHelper.log", ec) ||
            ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log").find("first-chance") == std::string::npos,
    "no handled-exception line without a capture");

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

std::uint64_t QpcNow()
{
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  return static_cast<std::uint64_t>(now.QuadPart);
}

std::uint64_t QpcFrequency()
{
  LARGE_INTEGER freq{};
  QueryPerformanceFrequency(&freq);
  return static_cast<std::uint64_t>(freq.QuadPart);
}

bool HasFileWithPrefix(const std::filesystem::path& dir, std::wstring_view prefix)
{
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (entry.path().filename().wstring().rfind(prefix, 0) == 0) {
      return true;
    }
  }
  return false;
}

bool HasDumpFile(const std::filesystem::path& dir)
{
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (entry.path().extension() == L".dmp") {
      return true;
    }
  }
  return false;
}

void TerminateWithExitCode(const ChildProcess& child, DWORD exitCode)
{
  Require(TerminateProcess(child.pi.hProcess, exitCode) != FALSE, "TerminateProcess failed");
  Require(WaitForSingleObject(child.pi.hProcess, 5000) == WAIT_OBJECT_0, "Child did not terminate");
}

bool HandleCrashEventTickForState(
  const HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  skydiag::helper::internal::HelperLoopState* state)
{
  return HandleCrashEventTick(
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
}

// v0.2.68-rc3 field case (2026-10-18): Crash Logger's thread dump on the
// manual-capture hotkey published a caught C++ exception, and the early check
// released it without a dump because the game kept running. The game then
// froze (hang captured) and was closed with exit code 1. The exit path read
// the still committed record again and wrote PostExitCrashEvidence for it, and
// a WER hint saying the game was probably ended on purpose.
void TestHandleProcessExitTick_DoesNotReReportAReleasedThreadDumpFault()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_released_exit");
  ClearLog(outBase);

  auto shared = MakeSharedLayout();
  shared->header.qpc_freq = QpcFrequency();
  shared->header.last_heartbeat_qpc = QpcNow();
  shared->header.state_flags = skydiag::kState_Frozen;
  shared->header.crash.exception_code = 0xE06D7363u;
  shared->header.crash.faulting_tid = 53376u;
  shared->header.crash_seq = 2u;

  auto child = LaunchSleepingChildProcess();
  auto proc = MakeAttachedProcessForChild(child, shared.get());
  proc.crashEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
  Require(proc.crashEvent != nullptr, "CreateEventW failed");

  HelperConfig cfg = MakeTestConfig();
  cfg.enableWerDumpFallbackHint = true;
  skydiag::helper::internal::HelperLoopState state{};

  std::atomic_bool stop{ false };
  std::thread game([&] {
    while (!stop.load()) {
      shared->header.last_heartbeat_qpc = QpcNow();
      Sleep(100);
    }
  });
  NoteCrashLoggerThreadDumpHotkeyPressed(GetTickCount64());
  const bool handled = HandleCrashEventTickForState(cfg, proc, outBase, &state);
  stop.store(true);
  game.join();
  NoteCrashLoggerThreadDumpHotkeyPressed(0);
  Require(handled && !state.crashCaptured.latched, "The thread-dump fault is released without a capture");
  Require(state.crashCaptured.releasedCrashSeq == 2u, "The released generation is recorded");
  // The release rests on a few seconds of heartbeat, like one after a dump;
  // the exit below is not reported only because the game froze after it.
  Require(state.crashCaptured.releasedOnBriefHeartbeat, "The early thread-dump release is a brief-heartbeat release");
  Require(!HasDumpFile(outBase), "No dump is written for the released fault");

  // The game then froze, a hang was captured, and the user closed it.
  state.hangState.hangCapturedThisEpisode = true;
  TerminateWithExitCode(child, 1u);
  Require(HandleProcessExitTick(cfg, proc, outBase, &state), "Process exit tick must handle the exit");

  Require(
    !HasFileWithPrefix(outBase, L"SkyrimDiag_PostExitCrashEvidence_"),
    "A released exception must not be reported again at process exit");
  Require(
    !FileExists(outBase / "SkyrimDiag_WER_LocalDumps_Hint.txt"),
    "A frozen game that was closed gets no WER LocalDumps hint");
  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "already released as a handled exception", "The skipped re-report is logged");
  AssertContains(log, "stopped responding since", "The skipped re-report says the game froze after the release");
  // Exit code 1 is also how Crash Logger ends a crash, so the log does not
  // claim the game was closed.
  AssertContains(log, "stopped responding and then exited", "The exit after the freeze is explained");
  AssertContains(log, "crash-*.log", "The log points at Crash Logger's crash log");
  Require(
    log.find("closed while not responding") == std::string::npos,
    "Only Windows' not-responding exit code is called a close");
  Require(
    log.find("no dump was attempted") == std::string::npos,
    "A hang was captured, so the log must not claim that no dump was attempted");

  CloseHandle(proc.crashEvent);
  proc.crashEvent = nullptr;
  TerminateChildProcess(&child);
  std::filesystem::remove_all(outBase);
}

// A real unhandled C++ exception on a worker thread within a minute of Crash
// Logger's thread dump hotkey reaches the record the same way as a thread
// dump probe, and the main thread keeps ticking while Crash Logger writes its
// log, so the early check releases it without a dump. Crash Logger then ends
// the live game with exit code 1, after writing its crash-*.log: the released
// fault is recorded after all, and no hint claims that no exception was
// recorded.
void TestHandleProcessExitTick_ReportsAnEarlyReleasedFaultEndedByCrashLogger()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_early_release_crash");
  ClearLog(outBase);
  const auto crashLogDir = outBase / L"SKSE";
  std::filesystem::create_directories(crashLogDir);

  auto shared = MakeSharedLayout();
  shared->header.qpc_freq = QpcFrequency();
  shared->header.last_heartbeat_qpc = QpcNow();
  shared->header.state_flags = skydiag::kState_Frozen;
  shared->header.crash.exception_code = 0xE06D7363u;
  shared->header.crash.faulting_tid = 36508u;
  shared->header.crash_seq = 2u;

  auto child = LaunchSleepingChildProcess();
  auto proc = MakeAttachedProcessForChild(child, shared.get());
  proc.crashEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
  Require(proc.crashEvent != nullptr, "CreateEventW failed");

  HelperConfig cfg = MakeTestConfig();
  cfg.enableWerDumpFallbackHint = true;
  skydiag::helper::internal::HelperLoopState state{};
  state.crashLoggerLogDirs = { outBase / L"missing", crashLogDir };

  std::atomic_bool stop{ false };
  std::thread game([&] {
    while (!stop.load()) {
      shared->header.last_heartbeat_qpc = QpcNow();
      Sleep(100);
    }
  });
  NoteCrashLoggerThreadDumpHotkeyPressed(GetTickCount64());
  const bool handled = HandleCrashEventTickForState(cfg, proc, outBase, &state);
  stop.store(true);
  game.join();
  NoteCrashLoggerThreadDumpHotkeyPressed(0);
  Require(handled && !state.crashCaptured.latched, "The fault is released without a capture");
  Require(
    state.crashCaptured.releasedCrashSeq == 2u && state.crashCaptured.releasedOnBriefHeartbeat,
    "The early release rests on a few seconds of heartbeat");
  Require(
    state.crashCaptured.releasedInThreadDumpWindow && state.crashCaptured.releasedCrashEventFileTimeUtc != 0u,
    "The early release is marked as one in Crash Logger's thread dump window");
  Require(!HasDumpFile(outBase), "No dump is written for the released fault");

  // The heartbeat is still fresh: Crash Logger, not a freeze, ends the game,
  // once it has written its crash log.
  WriteAllTextUtf8(crashLogDir / L"crash-2026-10-18-12-00-00.log", "Skyrim SSE crash log\n");
  shared->header.last_heartbeat_qpc = QpcNow();
  TerminateWithExitCode(child, 1u);
  Require(HandleProcessExitTick(cfg, proc, outBase, &state), "Process exit tick must handle the exit");

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  if (!HasFileWithPrefix(outBase, L"SkyrimDiag_PostExitCrashEvidence_")) {
    std::fprintf(stderr, "%s\n", log.c_str());
  }
  const auto evidence = ReadAllTextUtf8(
    FindSingleFileByPrefix(outBase, L"SkyrimDiag_PostExitCrashEvidence_", L".json"));
  AssertContains(
    evidence,
    "\"reason\": \"released_fault_followed_by_abnormal_exit\"",
    "The released fault is recorded as followed by an abnormal exit");
  AssertContains(evidence, "\"crash_seq\": 2", "The released generation is the one recorded");
  AssertContains(log, "recording it after all", "Recording the released fault is logged");
  AssertContains(log, "Crash Logger wrote crash-2026-10-18-12-00-00.log", "The crash log it rests on is named");
  Require(
    !FileExists(outBase / "SkyrimDiag_WER_LocalDumps_Hint.txt"),
    "No hint claims the exit came without a recorded exception");
  AssertContains(log, "with a crash on record as post-exit metadata", "Skipping the hint is logged");

  CloseHandle(proc.crashEvent);
  proc.crashEvent = nullptr;
  TerminateChildProcess(&child);
  std::filesystem::remove_all(outBase);
}

// The twin without a crash: the user takes Crash Logger's thread dump of a
// soft-locked game (its main loop, and so the heartbeat, still running), one
// of its probes publishes a caught C++ exception, and the early check releases
// it. The user then ends the game in Task Manager, also with exit code 1, and
// Crash Logger wrote only its threaddump-*.log. That is no crash: no
// PostExitCrashEvidence is written for the probe's exception.
void TestHandleProcessExitTick_DoesNotReportAnEarlyReleasedFaultWithoutACrashLog()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_early_release_soft_lock");
  ClearLog(outBase);
  const auto crashLogDir = outBase / L"SKSE";
  std::filesystem::create_directories(crashLogDir);
  // A crash log from an earlier session does not count.
  const auto oldCrashLog = crashLogDir / L"crash-2026-10-17-09-00-00.log";
  WriteAllTextUtf8(oldCrashLog, "Skyrim SSE crash log\n");
  std::filesystem::last_write_time(
    oldCrashLog, std::filesystem::file_time_type::clock::now() - std::chrono::hours(20));

  auto shared = MakeSharedLayout();
  shared->header.qpc_freq = QpcFrequency();
  shared->header.last_heartbeat_qpc = QpcNow();
  shared->header.state_flags = skydiag::kState_Frozen;
  shared->header.crash.exception_code = 0xE06D7363u;
  shared->header.crash.faulting_tid = 53376u;
  shared->header.crash_seq = 2u;

  auto child = LaunchSleepingChildProcess();
  auto proc = MakeAttachedProcessForChild(child, shared.get());
  proc.crashEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
  Require(proc.crashEvent != nullptr, "CreateEventW failed");

  HelperConfig cfg = MakeTestConfig();
  cfg.enableWerDumpFallbackHint = true;
  skydiag::helper::internal::HelperLoopState state{};
  state.crashLoggerLogDirs = { crashLogDir };

  std::atomic_bool stop{ false };
  std::thread game([&] {
    while (!stop.load()) {
      shared->header.last_heartbeat_qpc = QpcNow();
      Sleep(100);
    }
  });
  NoteCrashLoggerThreadDumpHotkeyPressed(GetTickCount64());
  const bool handled = HandleCrashEventTickForState(cfg, proc, outBase, &state);
  stop.store(true);
  game.join();
  NoteCrashLoggerThreadDumpHotkeyPressed(0);
  Require(handled && !state.crashCaptured.latched, "The probe's exception is released without a capture");
  Require(
    state.crashCaptured.releasedOnBriefHeartbeat && state.crashCaptured.releasedInThreadDumpWindow,
    "The early release is a brief-heartbeat release in the thread dump window");

  // The game never froze: the heartbeat is fresh when it is ended.
  WriteAllTextUtf8(crashLogDir / L"threaddump-2026-10-18-12-00-00.log", "Skyrim SSE thread dump\n");
  shared->header.last_heartbeat_qpc = QpcNow();
  TerminateWithExitCode(child, 1u);
  Require(HandleProcessExitTick(cfg, proc, outBase, &state), "Process exit tick must handle the exit");

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  if (HasFileWithPrefix(outBase, L"SkyrimDiag_PostExitCrashEvidence_")) {
    std::fprintf(stderr, "%s\n", log.c_str());
  }
  Require(
    !HasFileWithPrefix(outBase, L"SkyrimDiag_PostExitCrashEvidence_"),
    "A thread dump probe's exception must not be recorded as a crash without Crash Logger's crash log");
  AssertContains(log, "no Crash Logger crash log (crash-*.log) written since was found", "The skipped record is logged");
  Require(log.find("recording it after all") == std::string::npos, "The released fault is not recorded");

  CloseHandle(proc.crashEvent);
  proc.crashEvent = nullptr;
  TerminateChildProcess(&child);
  std::filesystem::remove_all(outBase);
}

// The exit path's check for an early release in Crash Logger's thread dump
// window: a non-zero exit within a minute records it with Crash Logger's
// crash-*.log written since its crash event, or with the exception's own exit
// code, and not with a thread dump log or an older crash log alone.
void TestHandleProcessExitTick_ThreadDumpWindowReleaseNeedsACrashLog()
{
  enum class CrashLog
  {
    kNone,
    kThreadDumpOnly,
    kOlderThanTheEvent,
    kWrittenSince,
  };
  struct Case
  {
    const char* name;
    DWORD exitCode;
    CrashLog crashLog;
    bool expectEvidence;
    bool hangCaptured = false;
  };
  const Case cases[] = {
    { "exit 1, no Crash Logger log: not recorded", 1u, CrashLog::kNone, false },
    { "exit 1, thread dump log only: not recorded", 1u, CrashLog::kThreadDumpOnly, false },
    { "exit 1, crash log older than the event: not recorded", 1u, CrashLog::kOlderThanTheEvent, false },
    { "exit 1, crash log written since: recorded", 1u, CrashLog::kWrittenSince, true },
    { "its own exit code, no crash log: recorded", 0xE06D7363u, CrashLog::kNone, true },
    { "exit 1, crash log written since, hang captured: not recorded", 1u, CrashLog::kWrittenSince, false, true },
  };

  for (const auto& c : cases) {
    const auto outBase = MakeTempDir(L"skydiag_helper_thread_dump_window_exit");
    ClearLog(outBase);
    const auto crashLogDir = outBase / L"SKSE";
    std::filesystem::create_directories(crashLogDir);

    auto shared = MakeSharedLayout();
    shared->header.crash_seq = 2u;
    shared->header.crash.exception_code = 0xE06D7363u;
    shared->header.crash.faulting_tid = 4242u;
    shared->header.state_flags = 0u;  // thawed when it was released
    shared->header.qpc_freq = QpcFrequency();
    shared->header.last_heartbeat_qpc = QpcNow();

    auto child = LaunchSleepingChildProcess();
    auto proc = MakeAttachedProcessForChild(child, shared.get());
    HelperConfig cfg = MakeTestConfig();
    cfg.enableWerDumpFallbackHint = true;

    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const std::uint64_t nowFileTime = (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    skydiag::helper::internal::HelperLoopState state{};
    state.crashLoggerLogDirs = { crashLogDir };
    state.crashCaptured.releasedCrashSeq = 2u;
    state.crashCaptured.releasedOnBriefHeartbeat = true;
    state.crashCaptured.releasedInThreadDumpWindow = true;
    state.crashCaptured.releasedCrashTick64 = GetTickCount64() - 5'000u;
    state.crashCaptured.releasedCrashEventFileTimeUtc = nowFileTime - 10ull * 10'000'000ull;  // 10 s ago
    state.hangState.hangCapturedThisEpisode = c.hangCaptured;

    switch (c.crashLog) {
      case CrashLog::kNone:
        break;
      case CrashLog::kThreadDumpOnly:
        WriteAllTextUtf8(crashLogDir / L"threaddump-2026-10-18-12-00-00.log", "Skyrim SSE thread dump\n");
        break;
      case CrashLog::kOlderThanTheEvent: {
        const auto older = crashLogDir / L"crash-2026-10-18-11-00-00.log";
        WriteAllTextUtf8(older, "Skyrim SSE crash log\n");
        std::filesystem::last_write_time(
          older, std::filesystem::file_time_type::clock::now() - std::chrono::minutes(2));
        break;
      }
      case CrashLog::kWrittenSince:
        WriteAllTextUtf8(crashLogDir / L"crash-2026-10-18-12-00-00.log", "Skyrim SSE crash log\n");
        break;
    }

    TerminateWithExitCode(child, c.exitCode);
    Require(HandleProcessExitTick(cfg, proc, outBase, &state), c.name);

    const bool evidence = HasFileWithPrefix(outBase, L"SkyrimDiag_PostExitCrashEvidence_");
    const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
    if (evidence != c.expectEvidence) {
      std::fprintf(stderr, "case: %s\n%s\n", c.name, log.c_str());
    }
    Require(evidence == c.expectEvidence, c.name);
    if (c.expectEvidence) {
      const auto record = ReadAllTextUtf8(
        FindSingleFileByPrefix(outBase, L"SkyrimDiag_PostExitCrashEvidence_", L".json"));
      AssertContains(record, "\"reason\": \"released_fault_followed_by_abnormal_exit\"", c.name);
      AssertContains(log, "recording it after all", c.name);
    } else if (c.hangCaptured) {
      AssertContains(log, "already released as a handled exception", c.name);
    } else {
      AssertContains(log, "no Crash Logger crash log (crash-*.log) written since was found", c.name);
    }

    TerminateChildProcess(&child);
    std::filesystem::remove_all(outBase);
  }
}

// The zero-exit twin: a strong fault released as handled came back at a
// clean exit as not_captured clean-exit evidence.
void TestHandleProcessExitTick_ZeroExitDoesNotReportAReleasedStrongFault()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_released_zero_exit");
  ClearLog(outBase);

  auto shared = MakeSharedLayout();
  shared->header.crash_seq = 2u;
  shared->header.crash.exception_code = 0xC0000005u;
  shared->header.crash.exception_addr = 0x12345678u;
  shared->header.state_flags = 0u;  // thawed when it was released

  auto child = LaunchSleepingChildProcess();
  auto proc = MakeAttachedProcessForChild(child, shared.get());
  HelperConfig cfg = MakeTestConfig();
  cfg.enableCleanExitEvidenceQuarantine = true;
  cfg.enableWerDumpFallbackHint = true;

  skydiag::helper::internal::HelperLoopState state{};
  state.crashCaptured.capturedInfo =
    skydiag::helper::internal::BuildCrashEventInfo(0xC0000005u, 0x12345678u, 77u, skydiag::kState_Frozen);
  state.crashCaptured.capturedInfo.crashSeq = 2u;
  state.crashCaptured.releasedCrashSeq = 2u;

  TerminateWithExitCode(child, 0u);
  Require(HandleProcessExitTick(cfg, proc, outBase, &state), "Process exit tick must handle the exit");

  Require(!state.crashCaptured.latched, "A released fault is not latched at exit");
  Require(
    !HasFileWithPrefix(outBase, L"SkyrimDiag_CleanExitEvidence_"),
    "A released fault must not come back as not_captured clean-exit evidence");
  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "already released as a handled exception", "The skipped re-report is logged");
  Require(
    log.find("dump capture was not attempted") == std::string::npos,
    "A released fault is not drained as a late fault");

  TerminateChildProcess(&child);
  std::filesystem::remove_all(outBase);
}

// Only the released generation is skipped: a newer committed fault at a
// non-zero exit is still recorded as post-exit metadata.
void TestHandleProcessExitTick_ReportsAFaultNewerThanTheReleasedOne()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_newer_than_released");
  ClearLog(outBase);

  auto shared = MakeSharedLayout();
  shared->header.crash_seq = 4u;
  shared->header.crash.exception_code = 0xC0000005u;
  shared->header.crash.exception_addr = 0xDEADBEEFu;
  shared->header.state_flags = skydiag::kState_Frozen;

  auto child = LaunchSleepingChildProcess();
  auto proc = MakeAttachedProcessForChild(child, shared.get());
  HelperConfig cfg = MakeTestConfig();
  cfg.enableWerDumpFallbackHint = true;
  skydiag::helper::internal::HelperLoopState state{};
  state.crashCaptured.releasedCrashSeq = 2u;

  TerminateWithExitCode(child, 1u);
  Require(HandleProcessExitTick(cfg, proc, outBase, &state), "Process exit tick must handle the exit");

  const auto evidence = ReadAllTextUtf8(
    FindSingleFileByPrefix(outBase, L"SkyrimDiag_PostExitCrashEvidence_", L".json"));
  AssertContains(
    evidence,
    "\"reason\": \"committed_fault_published_after_final_live_process_poll\"",
    "A fault the helper never captured keeps its reason");
  AssertContains(evidence, "\"crash_seq\": 4", "The newer generation is the one reported");
  AssertContains(evidence, "\"exit_code\": 1", "The exit code is recorded");
  AssertContains(evidence, "\"exception_addr\": 3735928559", "The committed fault fields are recorded");
  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  Require(log.find("already released") == std::string::npos, "A newer fault is not treated as released");
  // The heartbeat is long stale here, but a crash is on record.
  Require(
    log.find("closed while not responding") == std::string::npos &&
      log.find("stopped responding and then exited") == std::string::npos,
    "An exit with a recorded crash is not explained as closing a frozen game");
  Require(
    !FileExists(outBase / "SkyrimDiag_WER_LocalDumps_Hint.txt"),
    "An exit with a recorded crash gets no hint claiming no exception was recorded");

  TerminateChildProcess(&child);
  std::filesystem::remove_all(outBase);
}

// The plugin's watchdog restarts a helper that died, and the new one starts
// with no released generation. A committed generation whose freeze an earlier
// helper cleared was already acknowledged and must not come back at exit; one
// still frozen was never handled and stays reportable.
void TestInitializeLoopState_SkipsAGenerationAnEarlierHelperAcknowledged()
{
  using skydiag::helper::internal::InitializeLoopState;

  {
    auto shared = MakeSharedLayout();
    skydiag::helper::AttachedProcess proc{};
    proc.shm = shared.get();
    skydiag::helper::internal::HelperLoopState state{};
    InitializeLoopState(proc, {}, &state);
    Require(state.crashCaptured.releasedCrashSeq == 0u, "No crash yet: nothing is acknowledged");
  }

  struct Case
  {
    const char* name;
    std::uint32_t exceptionCode;
    std::uint32_t stateFlags;
    DWORD exitCode;
    bool expectAcknowledged;
  };
  const Case cases[] = {
    { "acknowledged C++ exception, exit 1: not reported", 0xE06D7363u, 0u, 1u, true },
    { "acknowledged strong fault, exit 0: no clean-exit evidence", 0xC0000005u, 0u, 0u, true },
    { "still frozen, exit 1: reported", 0xC0000005u, skydiag::kState_Frozen, 1u, false },
  };
  for (const auto& c : cases) {
    const auto outBase = MakeTempDir(L"skydiag_helper_restarted");
    ClearLog(outBase);

    auto shared = MakeSharedLayout();
    shared->header.qpc_freq = QpcFrequency();
    shared->header.last_heartbeat_qpc = QpcNow();
    shared->header.crash_seq = 2u;
    shared->header.crash.exception_code = c.exceptionCode;
    shared->header.state_flags = c.stateFlags;

    auto child = LaunchSleepingChildProcess();
    auto proc = MakeAttachedProcessForChild(child, shared.get());
    HelperConfig cfg = MakeTestConfig();
    cfg.enableCleanExitEvidenceQuarantine = true;
    cfg.enableWerDumpFallbackHint = true;

    skydiag::helper::internal::HelperLoopState state{};
    InitializeLoopState(proc, outBase, &state);
    Require((state.crashCaptured.releasedCrashSeq == 2u) == c.expectAcknowledged, c.name);
    Require(!state.crashCaptured.releasedOnBriefHeartbeat, c.name);

    TerminateWithExitCode(child, c.exitCode);
    Require(HandleProcessExitTick(cfg, proc, outBase, &state), c.name);

    const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
    if (c.expectAcknowledged) {
      Require(!HasFileWithPrefix(outBase, L"SkyrimDiag_PostExitCrashEvidence_"), c.name);
      Require(!HasFileWithPrefix(outBase, L"SkyrimDiag_CleanExitEvidence_"), c.name);
      AssertContains(log, "already released as a handled exception", c.name);
    } else {
      const auto evidence = ReadAllTextUtf8(
        FindSingleFileByPrefix(outBase, L"SkyrimDiag_PostExitCrashEvidence_", L".json"));
      AssertContains(evidence, "\"crash_seq\": 2", c.name);
      Require(log.find("already released") == std::string::npos, c.name);
    }

    TerminateChildProcess(&child);
    std::filesystem::remove_all(outBase);
  }
}

// A strong fault released after its dump because the heartbeat advanced for a
// few seconds may still be the fatal one: a worker's crash handler can run
// that long while the main thread ticks, and the filter deleted the dump. A
// non-zero exit soon after the release, or with the fault's own exit code,
// records it after all; a later exit, a zero exit, or an exit after the game
// froze (a hang captured, a stalled heartbeat, Windows' not-responding close)
// does not, unless it carries the fault's own code.
void TestHandleProcessExitTick_ReportsAReleasedCrashFollowedByAnAbnormalExit()
{
  struct Case
  {
    const char* name;
    DWORD exitCode;
    bool releasedOnBriefHeartbeat;
    std::uint64_t releasedAgoMs;
    bool expectEvidence;
    bool hangCaptured = false;
    double heartbeatAgeSec = 0.5;
  };
  const Case cases[] = {
    { "post-dump release 5 s before exit 1: recorded", 1u, true, 5'000u, true },
    { "post-dump release 90 s before exit 1: not recorded", 1u, true, 90'000u, false },
    { "post-dump release 5 s before exit 0: not recorded", 0u, true, 5'000u, false },
    { "late recovery 5 s before exit 1: not recorded", 1u, false, 5'000u, false },
    { "late recovery 90 s before its own exit code: recorded", 0xC0000005u, false, 90'000u, true },
    { "post-dump release, hang captured, exit 1: not recorded", 1u, true, 5'000u, false, true },
    { "post-dump release, heartbeat stalled 20 s, exit 1: not recorded", 1u, true, 5'000u, false, false, 20.0 },
    { "post-dump release, closed as not responding: not recorded", 0xCFFFFFFFu, true, 5'000u, false },
    { "post-dump release, hang captured, its own exit code: recorded", 0xC0000005u, true, 5'000u, true, true },
  };

  for (const auto& c : cases) {
    const auto outBase = MakeTempDir(L"skydiag_helper_released_then_abnormal_exit");
    ClearLog(outBase);

    auto shared = MakeSharedLayout();
    shared->header.crash_seq = 2u;
    shared->header.crash.exception_code = 0xC0000005u;
    shared->header.crash.exception_addr = 0x12345678u;
    shared->header.crash.faulting_tid = 4242u;
    shared->header.state_flags = 0u;  // thawed when it was released
    shared->header.qpc_freq = QpcFrequency();

    auto child = LaunchSleepingChildProcess();
    auto proc = MakeAttachedProcessForChild(child, shared.get());
    HelperConfig cfg = MakeTestConfig();
    cfg.enableCleanExitEvidenceQuarantine = true;
    cfg.enableWerDumpFallbackHint = true;

    skydiag::helper::internal::HelperLoopState state{};
    state.crashCaptured.capturedInfo =
      skydiag::helper::internal::BuildCrashEventInfo(0xC0000005u, 0x12345678u, 4242u, skydiag::kState_Frozen);
    state.crashCaptured.capturedInfo.crashSeq = 2u;
    state.crashCaptured.releasedCrashSeq = 2u;
    state.crashCaptured.releasedOnBriefHeartbeat = c.releasedOnBriefHeartbeat;
    state.crashCaptured.releasedCrashTick64 = GetTickCount64() - c.releasedAgoMs;
    state.hangState.hangCapturedThisEpisode = c.hangCaptured;

    shared->header.last_heartbeat_qpc =
      QpcNow() - static_cast<std::uint64_t>(c.heartbeatAgeSec * static_cast<double>(shared->header.qpc_freq));
    TerminateWithExitCode(child, c.exitCode);
    Require(HandleProcessExitTick(cfg, proc, outBase, &state), c.name);

    const bool evidence = HasFileWithPrefix(outBase, L"SkyrimDiag_PostExitCrashEvidence_");
    const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
    if (evidence != c.expectEvidence) {
      std::fprintf(stderr, "case: %s\n%s\n", c.name, log.c_str());
    }
    Require(evidence == c.expectEvidence, c.name);
    Require(!HasFileWithPrefix(outBase, L"SkyrimDiag_CleanExitEvidence_"), c.name);
    if (c.expectEvidence) {
      const auto record = ReadAllTextUtf8(
        FindSingleFileByPrefix(outBase, L"SkyrimDiag_PostExitCrashEvidence_", L".json"));
      AssertContains(record, "\"reason\": \"released_fault_followed_by_abnormal_exit\"", c.name);
      AssertContains(record, "\"crash_seq\": 2", c.name);
      AssertContains(record, "\"faulting_tid\": 4242", c.name);
      AssertContains(log, "recording it after all", c.name);
      // A crash is on record, so no hint claims there was none.
      Require(!FileExists(outBase / "SkyrimDiag_WER_LocalDumps_Hint.txt"), c.name);
    } else {
      AssertContains(log, "already released as a handled exception", c.name);
    }

    TerminateChildProcess(&child);
    std::filesystem::remove_all(outBase);
  }
}

// After the game stopped responding, a non-zero exit with no crash on record
// is a frozen game being closed: the abnormal-exit WER hint (a plugin's error
// dialog, WER LocalDumps) does not apply. It still does after a dialog at the
// hang capture, for an NTSTATUS exit code, while the heartbeat is fresh, and
// when the hang capture was held back because the window still answered.
void TestHandleProcessExitTick_AbnormalExitHintOnlyForAResponsiveGame()
{
  enum class Suppressed
  {
    kNone,
    kNotForeground,
    kForegroundGrace,
    kForegroundResponsive,
  };
  struct Case
  {
    const char* name;
    DWORD exitCode;
    bool hangCaptured;
    bool dialogSeen;
    std::uint32_t stateFlags;
    double heartbeatAgeSec;
    bool expectHint;
    Suppressed suppressed = Suppressed::kNone;
  };
  const Case cases[] = {
    { "hang captured, exit 1: no hint", 1u, true, false, 0u, 0.5, false },
    { "responsive, exit 1: hint", 1u, false, false, 0u, 0.5, true },
    { "dialog at the hang capture, exit 3: hint", 3u, true, true, 0u, 30.0, true },
    { "fail-fast exit code after a hang: hint", 0xC0000409u, true, false, 0u, 30.0, true },
    { "heartbeat stalled 20 s, exit 1: no hint", 1u, false, false, 0u, 20.0, false },
    { "loading, heartbeat 20 s old, exit 1: hint", 1u, false, false, skydiag::kState_Loading, 20.0, true },
    // A plugin's error dialog keeps the window answering while the heartbeat
    // stops; with the game in the background the hang capture is held back,
    // so the dialog is never seen.
    { "capture held back in the background, heartbeat stalled 60 s, exit 1: hint",
      1u, false, false, 0u, 60.0, true, Suppressed::kNotForeground },
    { "capture held back in the foreground grace, heartbeat stalled 60 s, exit 1: hint",
      1u, false, false, 0u, 60.0, true, Suppressed::kForegroundGrace },
    { "capture held back for an answering window, heartbeat stalled 60 s, exit 1: hint",
      1u, false, false, 0u, 60.0, true, Suppressed::kForegroundResponsive },
    { "hang captured after a held-back capture, exit 1: no hint",
      1u, true, false, 0u, 60.0, false, Suppressed::kNotForeground },
    // Windows' "not responding" close ends the program with 0xCFFFFFFF.
    { "closed as not responding after a hang capture: no hint", 0xCFFFFFFFu, true, false, 0u, 30.0, false },
    { "closed as not responding before the hang threshold: no hint", 0xCFFFFFFFu, false, false, 0u, 6.0, false },
    { "closed as not responding with a dialog at the hang capture: hint", 0xCFFFFFFFu, true, true, 0u, 30.0, true },
  };

  for (const auto& c : cases) {
    const auto outBase = MakeTempDir(L"skydiag_helper_abnormal_exit_hint");
    ClearLog(outBase);

    auto shared = MakeSharedLayout();
    shared->header.qpc_freq = QpcFrequency();
    shared->header.state_flags = c.stateFlags;
    auto child = LaunchSleepingChildProcess();
    auto proc = MakeAttachedProcessForChild(child, shared.get());
    HelperConfig cfg = MakeTestConfig();
    cfg.enableWerDumpFallbackHint = true;
    skydiag::helper::internal::HelperLoopState state{};
    state.hangState.hangCapturedThisEpisode = c.hangCaptured;
    state.hangState.modalDialogSeenThisEpisode = c.dialogSeen;
    state.hangState.hangSuppressedNotForegroundThisEpisode = c.suppressed == Suppressed::kNotForeground;
    state.hangState.hangSuppressedForegroundGraceThisEpisode = c.suppressed == Suppressed::kForegroundGrace;
    state.hangState.hangSuppressedForegroundResponsiveThisEpisode = c.suppressed == Suppressed::kForegroundResponsive;

    shared->header.last_heartbeat_qpc =
      QpcNow() - static_cast<std::uint64_t>(c.heartbeatAgeSec * static_cast<double>(shared->header.qpc_freq));
    TerminateWithExitCode(child, c.exitCode);
    Require(HandleProcessExitTick(cfg, proc, outBase, &state), c.name);

    const auto hintPath = outBase / "SkyrimDiag_WER_LocalDumps_Hint.txt";
    const bool hint = FileExists(hintPath);
    const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
    if (hint != c.expectHint) {
      std::fprintf(stderr, "case: %s\n%s\n", c.name, log.c_str());
    }
    Require(hint == c.expectHint, c.name);
    if (c.expectHint) {
      AssertContains(ReadAllTextUtf8(hintPath), "This is not a failed capture.", c.name);
      Require(log.find("closed while not responding") == std::string::npos, c.name);
      Require(log.find("stopped responding and then exited") == std::string::npos, c.name);
    } else {
      Require(log.find("no dump was attempted") == std::string::npos, c.name);
      if (c.exitCode == 0xCFFFFFFFu) {
        AssertContains(log, "closed while not responding", c.name);
        AssertContains(log, "closed by Windows as not responding", c.name);
      } else {
        // Crash Logger also ends a crash with exit code 1: not called a close.
        Require(log.find("closed while not responding") == std::string::npos, c.name);
        AssertContains(log, "stopped responding and then exited", c.name);
        AssertContains(log, "crash-*.log", c.name);
      }
    }

    TerminateChildProcess(&child);
    std::filesystem::remove_all(outBase);
  }
}

// A crash whose dump could not be written leaves the dump-failure WER hint.
// The exit path overwrote it with the abnormal-exit hint ("This is not a
// failed capture") and reported the fault as published after the last poll.
void TestHandleProcessExitTick_KeepsTheDumpFailureHint()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_dump_failure_hint");
  ClearLog(outBase);

  auto shared = MakeSharedLayout();
  shared->header.crash_seq = 2u;
  shared->header.crash.exception_code = 0xC0000005u;
  shared->header.state_flags = 0u;

  auto child = LaunchSleepingChildProcess();
  auto proc = MakeAttachedProcessForChild(child, shared.get());
  HelperConfig cfg = MakeTestConfig();
  cfg.enableWerDumpFallbackHint = true;
  WriteWerFallbackHint(outBase, WerFallbackReason::kDumpWriteFailed, 0u);

  skydiag::helper::internal::HelperLoopState state{};
  state.crashCaptured.capturedInfo.crashSeq = 2u;
  state.crashCaptured.dumpWriteFailedCrashSeq = 2u;
  state.crashCaptured.dumpWriteFailedProcessExited = false;

  TerminateWithExitCode(child, 1u);
  Require(HandleProcessExitTick(cfg, proc, outBase, &state), "Process exit tick must handle the exit");

  const auto hint = ReadAllTextUtf8(outBase / "SkyrimDiag_WER_LocalDumps_Hint.txt");
  AssertContains(hint, "could not write its dump", "The dump-failure hint stays");
  Require(hint.find("This is not a failed capture.") == std::string::npos, "It is not replaced by the abnormal-exit hint");
  const auto evidence = ReadAllTextUtf8(
    FindSingleFileByPrefix(outBase, L"SkyrimDiag_PostExitCrashEvidence_", L".json"));
  AssertContains(
    evidence,
    "\"reason\": \"committed_fault_dump_write_failed\"",
    "The post-exit record says the dump write failed");
  AssertContains(
    ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log"),
    "keeping its WER LocalDumps hint",
    "Keeping the hint is logged");

  TerminateChildProcess(&child);
  std::filesystem::remove_all(outBase);
}

// End to end: the game exits (code 1) while the early thread-dump check runs.
// Its address space is gone, so no dump is attempted; the crash is recorded as
// a failed dump, and the exit keeps that hint and says why in its metadata.
void TestThreadDumpFaultOfAnExitedGame_IsRecordedAsADumpFailure()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_exited_before_dump");
  ClearLog(outBase);

  auto shared = MakeSharedLayout();
  shared->header.qpc_freq = QpcFrequency();
  shared->header.last_heartbeat_qpc = QpcNow();
  shared->header.state_flags = skydiag::kState_Frozen;
  shared->header.crash.exception_code = 0xE06D7363u;
  shared->header.crash_seq = 2u;

  auto child = LaunchSleepingChildProcess();
  auto proc = MakeAttachedProcessForChild(child, shared.get());
  proc.crashEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
  Require(proc.crashEvent != nullptr, "CreateEventW failed");

  HelperConfig cfg = MakeTestConfig();
  cfg.enableWerDumpFallbackHint = true;
  skydiag::helper::internal::HelperLoopState state{};

  TerminateWithExitCode(child, 1u);
  NoteCrashLoggerThreadDumpHotkeyPressed(GetTickCount64());
  const bool handled = HandleCrashEventTickForState(cfg, proc, outBase, &state);
  NoteCrashLoggerThreadDumpHotkeyPressed(0);
  Require(handled && !state.crashCaptured.latched, "The event is consumed without a capture");
  Require(
    state.crashCaptured.dumpWriteFailedCrashSeq == 2u && state.crashCaptured.dumpWriteFailedProcessExited,
    "The crash is recorded as a dump that could not be written after the game exited");
  Require(state.crashCaptured.releasedCrashSeq == 0u, "A crash of an exited game is not released as handled");

  Require(HandleProcessExitTick(cfg, proc, outBase, &state), "Process exit tick must handle the exit");

  Require(!HasDumpFile(outBase), "No dump file is left behind");
  AssertContains(
    ReadAllTextUtf8(outBase / "SkyrimDiag_WER_LocalDumps_Hint.txt"),
    "could not write its dump",
    "The exit keeps the dump-failure hint");
  const auto evidence = ReadAllTextUtf8(
    FindSingleFileByPrefix(outBase, L"SkyrimDiag_PostExitCrashEvidence_", L".json"));
  AssertContains(
    evidence,
    "\"reason\": \"committed_fault_dump_write_failed_process_exited\"",
    "The post-exit record says the game exited before its dump");
  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "before a crash dump could be written", "Skipping the dead-process dump is logged");
  // An attempted write on the dead process fails the same way (empty file,
  // deleted), so only the attempt's own log line tells the two paths apart.
  Require(log.find("Crash dump attempt") == std::string::npos, "No dump write is attempted");
  AssertContains(
    log, "Crash dump failed: the game process had already exited", "The skipped write gives the exit as its reason");

  CloseHandle(proc.crashEvent);
  proc.crashEvent = nullptr;
  TerminateChildProcess(&child);
  std::filesystem::remove_all(outBase);
}

// ADR-0022: one blackbox event of the plugin.
void PutEvent(skydiag::SharedLayout* shared, skydiag::EventType type, double secondsAfterStart, std::uint64_t a)
{
  const std::uint32_t index = shared->header.write_index++;
  auto& e = shared->events[index % shared->header.capacity];
  e.seq = index * 2u;
  e.type = static_cast<std::uint16_t>(type);
  e.qpc = shared->header.start_qpc + static_cast<std::uint64_t>(secondsAfterStart * shared->header.qpc_freq);
  e.payload.a = a;
}

// The close request the newest crash came after, from the snapshot the helper
// takes when the crash event comes.
void TestFindForcedCloseInSnapshot()
{
  using skydiag::helper::internal::FindForcedCloseInSnapshot;
  constexpr auto kAltF4 = static_cast<std::uint64_t>(skydiag::CloseRequestKind::kAltF4);
  {
    auto shared = MakeSharedLayout();
    PutEvent(shared.get(), skydiag::EventType::kHeartbeat, 1.0, 0u);
    PutEvent(shared.get(), skydiag::EventType::kCloseRequest, 120.0, kAltF4);
    PutEvent(shared.get(), skydiag::EventType::kCrash, 120.8, 0xC0000005u);
    const auto match = FindForcedCloseInSnapshot(shared.get(), sizeof(skydiag::SharedLayout));
    Require(match.kind == skydiag::CloseRequestKind::kAltF4, "Alt+F4 0.8 s before the crash");
    Require(match.msBeforeCrash > 799.0 && match.msBeforeCrash < 801.0, "0.8 s");
  }
  {
    auto shared = MakeSharedLayout();
    PutEvent(shared.get(), skydiag::EventType::kCloseRequest, 100.0, kAltF4);
    PutEvent(shared.get(), skydiag::EventType::kCrash, 111.0, 0xC0000005u);
    Require(!FindForcedCloseInSnapshot(shared.get(), sizeof(skydiag::SharedLayout)).matched(), "11 s is too long");
  }
  {
    // An entry the snapshot could not copy stably (odd seq) is skipped.
    auto shared = MakeSharedLayout();
    PutEvent(shared.get(), skydiag::EventType::kCloseRequest, 100.0, kAltF4);
    shared->events[0].seq = 1u;
    PutEvent(shared.get(), skydiag::EventType::kCrash, 100.5, 0xC0000005u);
    Require(!FindForcedCloseInSnapshot(shared.get(), sizeof(skydiag::SharedLayout)).matched(), "unstable entry");
  }
  {
    auto shared = MakeSharedLayout();
    PutEvent(shared.get(), skydiag::EventType::kCloseRequest, 100.0, kAltF4);
    PutEvent(shared.get(), skydiag::EventType::kCrash, 100.5, 0xC0000005u);
    shared->header.magic = 0u;
    Require(!FindForcedCloseInSnapshot(shared.get(), sizeof(skydiag::SharedLayout)).matched(), "not a blackbox");
    Require(!FindForcedCloseInSnapshot(nullptr, 0u).matched(), "no snapshot");
  }
}

// End to end, the 2026-10-26 case: Alt+F4 and 0.8 s later an access
// violation while the game still runs. The dump is written and kept as a
// crash, but no viewer is opened or queued for the game's exit.
void TestForcedCloseCrash_SkipsTheViewer()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_forced_close");
  ClearLog(outBase);

  auto shared = MakeSharedLayout();
  shared->header.state_flags = skydiag::kState_Frozen;
  shared->header.crash.exception_code = 0xC0000005u;
  shared->header.crash.exception_addr = 0x7FF7055F0000ull;
  shared->header.crash_seq = 2u;
  PutEvent(shared.get(), skydiag::EventType::kCloseRequest, 120.0,
    static_cast<std::uint64_t>(skydiag::CloseRequestKind::kAltF4));
  PutEvent(shared.get(), skydiag::EventType::kCrash, 120.8, 0xC0000005u);

  auto child = LaunchSleepingChildProcess();
  shared->header.crash.faulting_tid = child.pi.dwThreadId;
  // The faulting thread's real registers, as the plugin hands them over. An
  // all-zero context for a thread that exists makes Windows Server 2022's
  // dbghelp fail the dump with 0x800706F8 on AMX hosts (CI only; see the
  // v0.2.60-rc3 XSTATE fix), which no real crash produces.
  CONTEXT faultContext{};
  faultContext.ContextFlags = CONTEXT_FULL;
  const HANDLE faultThread =
    OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, child.pi.dwThreadId);
  Require(faultThread != nullptr, "OpenThread failed");
  Require(SuspendThread(faultThread) != static_cast<DWORD>(-1), "SuspendThread failed");
  const bool gotContext = GetThreadContext(faultThread, &faultContext) != FALSE;
  Require(ResumeThread(faultThread) != static_cast<DWORD>(-1), "ResumeThread failed");
  CloseHandle(faultThread);
  Require(gotContext, "GetThreadContext failed");
  shared->header.crash.context = faultContext;
  shared->header.crash.exception_record.ExceptionCode = 0xC0000005u;
  shared->header.crash.exception_record.ExceptionAddress = reinterpret_cast<PVOID>(0x7FF7055F0000ull);
  auto proc = MakeAttachedProcessForChild(child, shared.get());
  proc.crashEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
  Require(proc.crashEvent != nullptr, "CreateEventW failed");

  HelperConfig cfg = MakeTestConfig();
  cfg.autoOpenViewerOnCrash = true;
  cfg.autoOpenCrashOnlyIfProcessExited = true;
  skydiag::helper::internal::HelperLoopState state{};

  Require(HandleCrashEventTickForState(cfg, proc, outBase, &state), "The crash event is handled");
  Require(state.crashCaptured.latched, "The crash is kept");
  Require(state.crashCaptured.forcedClose.kind == skydiag::CloseRequestKind::kAltF4, "It followed Alt+F4");
  Require(HasDumpFile(outBase), "Its dump is written");
  Require(state.pendingCrashViewerDumpPath.empty(), "No viewer is queued for the exit");
  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "Skipped viewer auto-open: crash followed a close request (Alt+F4) 0.8 s earlier.", "The skip is logged");
  Require(log.find("deferring viewer") == std::string::npos, "No viewer deferred");
  Require(log.find("Auto-opened DumpTool viewer") == std::string::npos, "No viewer opened");

  CloseHandle(proc.crashEvent);
  proc.crashEvent = nullptr;
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

// The decision alone: alive for a minute after the crash was kept and the
// heartbeat advancing in six consecutive five-second windows.
void TestLateRecoveryNeedsAMinuteOfAdvancingHeartbeat()
{
  using skydiag::helper::internal::AdvanceLateRecovery;
  using skydiag::helper::internal::LateRecoveryState;
  using skydiag::helper::internal::StartLateRecovery;

  LateRecoveryState s{};
  Require(!AdvanceLateRecovery(&s, 100'000, 1), "Not started: never releases");

  StartLateRecovery(&s, 1'000, 100);
  std::uint64_t now = 1'000;
  std::uint64_t hb = 100;
  for (int i = 0; i < 6; ++i) {
    now += 5'000;
    hb += 50;
    Require(!AdvanceLateRecovery(&s, now, hb), "30 s of heartbeat is not enough before a minute alive");
  }
  {
    LateRecoveryState ready{};
    StartLateRecovery(&ready, 1'000, 100);
    ready.lastSampleTick64 = 70'000;
    ready.lastHeartbeatQpc = 500;
    ready.advancingWindows = 5;
    Require(!AdvanceLateRecovery(&ready, 71'000, 600), "Samples come no faster than every five seconds");
    Require(ready.advancingWindows == 5u, "A too-early call does not count a window");
    Require(AdvanceLateRecovery(&ready, 75'000, 600), "The next window completes the count");
  }
  for (int i = 0; i < 5; ++i) {
    now += 5'000;
    hb += 50;
    (void)AdvanceLateRecovery(&s, now, hb);
  }
  now += 5'000;
  hb += 50;
  Require(now - 1'000 >= 60'000, "test clock reached a minute");
  Require(AdvanceLateRecovery(&s, now, hb), "A minute alive and 30 s of advancing heartbeat releases");

  // A window without heartbeat (paused, minimized, stalled) restarts the count.
  StartLateRecovery(&s, 1'000, 100);
  now = 1'000;
  hb = 100;
  bool releasedBeforeStall = false;
  for (int i = 0; i < 20; ++i) {
    now += 5'000;
    if (i != 15) {
      hb += 50;
    }
    const bool released = AdvanceLateRecovery(&s, now, hb);
    releasedBeforeStall = releasedBeforeStall || (i < 15 && released);
    Require(!(i >= 15 && released), "A stalled window restarts the 30 s count");
  }
  Require(releasedBeforeStall, "Before the stall the release condition was met");
}

// A first-chance exception kept as a crash because the main thread was busy
// during the filter window, after which the game kept running: the capture is
// released like a zero exit, and the freeze is thawed for a later real CTD.
void TestHandleLateRecoveryTick_ReleasesACrashTheGameOutlived()
{
  using skydiag::helper::internal::HandleLateRecoveryTick;

  const auto outBase = MakeTempDir(L"skydiag_helper_late_recovery");
  ClearLog(outBase);
  const auto dumpPath = outBase / "SkyrimDiag_Crash_20261015_120000_001.dmp";
  const auto reportPath = outBase / "SkyrimDiag_Crash_20261015_120000_001_SkyrimDiagReport.txt";
  WriteAllTextUtf8(dumpPath, "dump");
  WriteAllTextUtf8(reportPath, "report");

  auto shared = MakeSharedLayout();
  shared->header.crash_seq = 2u;
  shared->header.crash.exception_code = 0xC0000005u;
  shared->header.state_flags = skydiag::kState_Frozen;
  shared->header.last_heartbeat_qpc = 100u;

  auto child = LaunchSleepingChildProcess();
  auto proc = MakeAttachedProcessForChild(child, shared.get());
  HelperConfig cfg = MakeTestConfig();

  skydiag::helper::internal::HelperLoopState state{};
  state.crashCaptured.latched = true;
  state.crashCaptured.capturedInfo =
    skydiag::helper::internal::BuildCrashEventInfo(0xC0000005u, 0x12345678u, child.pi.dwThreadId, skydiag::kState_Frozen);
  state.crashCaptured.capturedInfo.crashSeq = 2u;
  state.capturedCrashDumpPath = dumpPath.wstring();
  state.pendingCrashViewerDumpPath = dumpPath.wstring();

  // Six advancing windows but only 30 s since the crash was kept: too early.
  const auto now = GetTickCount64();
  auto& late = state.crashCaptured.lateRecovery;
  skydiag::helper::internal::StartLateRecovery(&late, now - 30'000, 100u);
  late.lastSampleTick64 = now - 6'000;
  late.advancingWindows = 5u;
  shared->header.last_heartbeat_qpc = 200u;
  Require(!HandleLateRecoveryTick(cfg, proc, outBase, &state), "Too early: the capture is kept");
  Require(state.crashCaptured.latched && FileExists(dumpPath), "Too early: dump and latch stay");

  // Two minutes after the crash was kept, five advancing windows already seen.
  late.latchedTick64 = now - 120'000;
  late.lastSampleTick64 = now - 6'000;
  late.lastHeartbeatQpc = 200u;
  late.advancingWindows = 5u;
  shared->header.last_heartbeat_qpc = 300u;
  Require(HandleLateRecoveryTick(cfg, proc, outBase, &state), "The game outlived the crash: released");
  Require(!state.crashCaptured.latched, "Latch cleared");
  // The heartbeat does not prove the faulting thread recovered, so the dump
  // is kept for review; its derived crash artifacts go.
  Require(FileExists(dumpPath), "The dump is kept");
  Require(!FileExists(reportPath), "Derived crash artifacts are removed");
  Require(state.crashCaptured.releasedCrashSeq == 2u, "A later exit does not re-report this exception");
  Require(
    !state.crashCaptured.releasedOnBriefHeartbeat && state.crashCaptured.releasedCrashTick64 != 0u,
    "Late recovery is not a brief-heartbeat release");
  Require(state.pendingCrashViewerDumpPath.empty(), "Deferred viewer cancelled");
  Require((shared->header.state_flags & skydiag::kState_Frozen) == 0u, "Freeze thawed for a later CTD");

  const auto evidence = ReadAllTextUtf8(FindSingleFileByPrefix(outBase, L"SkyrimDiag_CleanExitEvidence_", L".json"));
  AssertContains(evidence, "strong_fault_published_but_game_kept_running_dump_preserved", "Evidence says the game kept running");
  AssertContains(evidence, "kept running for over a minute", "The note does not claim an exit code");
  AssertContains(evidence, "\"filter_context\": \"late_recovery\"", "Late-recovery context is explicit");
  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "Game kept running", "Log explains the release");
  AssertContains(log, "thawed", "Log confirms the thaw");

  TerminateChildProcess(&child);
  std::filesystem::remove_all(outBase);
}

// Without heartbeat progress (the game is stuck, or a real crash is still
// being reported) the capture is never released, however long it waits.
void TestHandleLateRecoveryTick_KeepsACrashWithoutHeartbeat()
{
  using skydiag::helper::internal::HandleLateRecoveryTick;

  const auto outBase = MakeTempDir(L"skydiag_helper_late_recovery_stuck");
  ClearLog(outBase);
  const auto dumpPath = outBase / "SkyrimDiag_Crash_20261015_120500_001.dmp";
  WriteAllTextUtf8(dumpPath, "dump");

  auto shared = MakeSharedLayout();
  shared->header.crash_seq = 2u;
  shared->header.state_flags = skydiag::kState_Frozen;
  shared->header.last_heartbeat_qpc = 500u;
  auto child = LaunchSleepingChildProcess();
  auto proc = MakeAttachedProcessForChild(child, shared.get());
  HelperConfig cfg = MakeTestConfig();

  skydiag::helper::internal::HelperLoopState state{};
  state.crashCaptured.latched = true;
  state.crashCaptured.capturedInfo.crashSeq = 2u;
  state.capturedCrashDumpPath = dumpPath.wstring();
  auto& late = state.crashCaptured.lateRecovery;
  const auto now = GetTickCount64();
  late.latchedTick64 = now - 600'000;
  late.lastSampleTick64 = now - 6'000;
  late.lastHeartbeatQpc = 500u;
  late.advancingWindows = 5u;
  Require(!HandleLateRecoveryTick(cfg, proc, outBase, &state), "No heartbeat progress: kept");
  Require(state.crashCaptured.latched && FileExists(dumpPath), "Dump and latch stay");
  Require((shared->header.state_flags & skydiag::kState_Frozen) != 0u, "Still frozen");

  TerminateChildProcess(&child);
  std::filesystem::remove_all(outBase);
}

// OutputDir is relative to the helper exe folder (SkyrimDiagHelper.ini). It
// must reach the dump tool absolute: the tool runs with the output folder as
// its working directory, so "MyLogs" used to become MyLogs\MyLogs\x.dmp.
void TestOutputDirResolvesAgainstTheHelperFolder()
{
  using skydiag::helper::ResolveOutputDirAgainstExeDir;
  const std::filesystem::path exeDir = L"C:\\Game\\Data\\SKSE\\Plugins";
  Require(ResolveOutputDirAgainstExeDir(L"", exeDir, L"Tullius Ctd Logs") ==
            std::filesystem::path(L"C:\\Game\\Data\\SKSE\\Plugins\\Tullius Ctd Logs"),
    "Blank OutputDir is the default subfolder");
  Require(ResolveOutputDirAgainstExeDir(L"MyLogs", exeDir, L"Tullius Ctd Logs") ==
            std::filesystem::path(L"C:\\Game\\Data\\SKSE\\Plugins\\MyLogs"),
    "A relative OutputDir is under the helper folder");
  Require(ResolveOutputDirAgainstExeDir(L"..\\..\\..\\Logs", exeDir, L"Tullius Ctd Logs") ==
            std::filesystem::path(L"C:\\Game\\Logs"),
    "Parent references are normalized");
  Require(ResolveOutputDirAgainstExeDir(L"D:\\SkyrimDiag", exeDir, L"Tullius Ctd Logs") ==
            std::filesystem::path(L"D:\\SkyrimDiag"),
    "An absolute OutputDir is kept");
  Require(ResolveOutputDirAgainstExeDir(L"D:\\Logs\\", exeDir, L"Tullius Ctd Logs") ==
            std::filesystem::path(L"D:\\Logs"),
    "A trailing separator is dropped (it would escape the closing quote on the command line)");
  Require(ResolveOutputDirAgainstExeDir(L"D:\\", exeDir, L"Tullius Ctd Logs") ==
            std::filesystem::path(L"D:\\"),
    "A drive root keeps its separator");
}

HANDLE DuplicateProcessHandle(HANDLE process)
{
  HANDLE copy = nullptr;
  Require(DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS) != FALSE,
    "DuplicateHandle failed");
  return copy;
}

// After a crash the game exits non-zero and the helper shuts down. With no
// viewer opened (AutoOpenViewerOnCrash=0) the headless analyzer is the only
// thing writing the report, so it must be left to finish. When the viewer
// opened at exit analyzes the same dump, the tracked analyzer is stopped.
void TestShutdownLeavesTheAnalyzerRunningUnlessTheViewerOpened()
{
  using skydiag::helper::internal::ReleasePendingCrashAnalysisAtShutdown;
  const auto outBase = MakeTempDir(L"skydiag_helper_shutdown_analysis");
  ClearLog(outBase);

  {
    auto analyzer = LaunchSleepingChildProcess();
    skydiag::helper::internal::HelperLoopState state{};
    state.pendingCrashAnalysis.active = true;
    state.pendingCrashAnalysis.process = DuplicateProcessHandle(analyzer.pi.hProcess);
    state.pendingCrashAnalysis.startedAtTick64 = GetTickCount64();
    ReleasePendingCrashAnalysisAtShutdown(outBase, &state);
    Require(!state.pendingCrashAnalysis.active, "No longer tracked");
    Require(WaitForSingleObject(analyzer.pi.hProcess, 0) == WAIT_TIMEOUT, "The analyzer keeps running to write the report");
    AssertContains(ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log"), "leaving it to finish", "Log says it was left running");
    TerminateChildProcess(&analyzer);
  }
  {
    auto analyzer = LaunchSleepingChildProcess();
    skydiag::helper::internal::HelperLoopState state{};
    state.pendingCrashAnalysis.active = true;
    state.pendingCrashAnalysis.process = DuplicateProcessHandle(analyzer.pi.hProcess);
    state.pendingCrashAnalysis.startedAtTick64 = GetTickCount64();
    state.crashViewerLaunchedAtExit = true;
    ReleasePendingCrashAnalysisAtShutdown(outBase, &state);
    Require(WaitForSingleObject(analyzer.pi.hProcess, 5000) == WAIT_OBJECT_0, "The viewer analyzes the dump: tracked analyzer stopped");
    TerminateChildProcess(&analyzer);
  }
  std::filesystem::remove_all(outBase);
}

// The analysis timeout exists for the recapture decision, but the analysis
// also writes the crash report: a slow one (a large dump) is left to finish
// instead of being killed, and with recapture on its late result decides no
// recapture.
void TestAnalysisTimeoutLeavesTheReportToFinish()
{
  using skydiag::helper::internal::FinalizePendingCrashAnalysisIfReady;
  const auto outBase = MakeTempDir(L"skydiag_helper_analysis_timeout");
  ClearLog(outBase);
  skydiag::helper::AttachedProcess proc{};

  for (const bool recapture : { false, true }) {
    HelperConfig cfg = MakeTestConfig();
    cfg.enableAutoRecaptureOnUnknownCrash = recapture;
    auto analyzer = LaunchSleepingChildProcess();
    skydiag::helper::internal::PendingCrashAnalysis task{};
    task.active = true;
    task.process = DuplicateProcessHandle(analyzer.pi.hProcess);
    task.startedAtTick64 = GetTickCount64() - 60'000;
    task.timeoutMs = 20'000;
    FinalizePendingCrashAnalysisIfReady(cfg, proc, outBase, &task);
    Require(WaitForSingleObject(analyzer.pi.hProcess, 0) == WAIT_TIMEOUT, "The analysis is left to finish the report");
    Require(task.active && task.process && task.timeoutMs == 0u,
      "Still tracked without a timeout, so a filtered-crash cleanup can stop it");
    Require(task.recaptureExpired == recapture, "With recapture on, its window is marked as passed");
    if (recapture) {
      // It finishes late: no recapture decision, the handle is released.
      Require(TerminateProcess(analyzer.pi.hProcess, 0u) != FALSE, "TerminateProcess failed");
      Require(WaitForSingleObject(analyzer.pi.hProcess, 5000) == WAIT_OBJECT_0, "the analyzer did not end");
      FinalizePendingCrashAnalysisIfReady(cfg, proc, outBase, &task);
      Require(!task.active, "No longer tracked once it finished");
      AssertContains(ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log"), "recapture skipped", "The skipped recapture is logged");
    } else {
      skydiag::helper::internal::ClearPendingCrashAnalysis(&task);
      Require(WaitForSingleObject(analyzer.pi.hProcess, 5000) == WAIT_OBJECT_0, "The cleanup can still stop it");
    }
    TerminateChildProcess(&analyzer);
  }
  std::filesystem::remove_all(outBase);
}

// A helper the plugin's watchdog started after the one handling a crash died
// finds that generation still Frozen while the game runs. Nothing else would
// signal it again, and while it stays Frozen the plugin records no later
// crash, so the new helper signals its event and handles it.
void TestInitializeLoopState_RearmsAFrozenCrashNoHelperHandles()
{
  using skydiag::helper::internal::InitializeLoopState;
  struct Case
  {
    const char* name;
    std::uint32_t stateFlags;
    bool gameRunning;
    bool expectRearmed;
  };
  const Case cases[] = {
    { "still frozen, game running: signaled again", skydiag::kState_Frozen, true, true },
    { "acknowledged (thawed): not signaled", 0u, true, false },
    { "still frozen, game exited: left to the exit drain", skydiag::kState_Frozen, false, false },
  };
  for (const auto& c : cases) {
    const auto outBase = MakeTempDir(L"skydiag_helper_rearm");
    ClearLog(outBase);
    auto shared = MakeSharedLayout();
    shared->header.crash_seq = 2u;
    shared->header.crash.exception_code = 0xC0000005u;
    shared->header.state_flags = c.stateFlags;
    auto child = LaunchSleepingChildProcess();
    auto proc = MakeAttachedProcessForChild(child, shared.get());
    proc.crashEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Require(proc.crashEvent != nullptr, "CreateEventW failed");
    if (!c.gameRunning) {
      TerminateWithExitCode(child, 1u);
    }

    skydiag::helper::internal::HelperLoopState state{};
    InitializeLoopState(proc, outBase, &state);
    const bool signaled = WaitForSingleObject(proc.crashEvent, 0) == WAIT_OBJECT_0;
    Require(signaled == c.expectRearmed, c.name);
    Require(state.rearmedFrozenCrashSeq == (c.expectRearmed ? 2u : 0u), c.name);
    Require(HasFileWithPrefix(outBase, L"SkyrimDiag_CrashRearm_") == c.expectRearmed, c.name);

    CloseHandle(proc.crashEvent);
    TerminateChildProcess(&child);
    std::filesystem::remove_all(outBase);
  }
}

// A helper that dies on a crash every time (an antivirus killing it while it
// writes the dump) is restarted by the plugin's watchdog. Each one finding
// the generation still Frozen must not signal it again forever, the game
// stuttering at every dump attempt: it is signaled again once, then thawed
// so a later crash is recorded again. The marker goes when the game exits.
void TestRearmFrozenCrash_SignalsAGenerationAgainOnlyOnce()
{
  using skydiag::helper::internal::InitializeLoopState;
  using skydiag::helper::internal::RearmFrozenCrashFromEarlierHelper;
  const auto outBase = MakeTempDir(L"skydiag_helper_rearm_once");
  ClearLog(outBase);
  auto shared = MakeSharedLayout();
  shared->header.crash_seq = 2u;
  shared->header.crash.exception_code = 0xC0000005u;
  shared->header.state_flags = skydiag::kState_Frozen;
  auto child = LaunchSleepingChildProcess();
  auto proc = MakeAttachedProcessForChild(child, shared.get());
  proc.crashEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Require(proc.crashEvent != nullptr, "CreateEventW failed");

  // The first restarted helper signals it again; it dies before handling it.
  Require(RearmFrozenCrashFromEarlierHelper(proc, outBase) == 2u, "signaled again the first time");
  Require(WaitForSingleObject(proc.crashEvent, 0) == WAIT_OBJECT_0, "its crash event is set");
  Require(ResetEvent(proc.crashEvent) != FALSE, "ResetEvent failed");

  // The next one finds it still Frozen.
  skydiag::helper::internal::HelperLoopState state{};
  InitializeLoopState(proc, outBase, &state);
  Require(state.rearmedFrozenCrashSeq == 0u, "not signaled a second time");
  Require(WaitForSingleObject(proc.crashEvent, 0) == WAIT_TIMEOUT, "its crash event stays clear");
  Require((shared->header.state_flags & skydiag::kState_Frozen) == 0u, "thawed so a later crash is recorded");
  AssertContains(
    ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log"), "not signaling it again. Thawed capture", "the give-up is logged");

  // Another game process with the same crash_seq is not affected by it.
  auto other = LaunchSleepingChildProcess();
  auto otherProc = MakeAttachedProcessForChild(other, shared.get());
  otherProc.crashEvent = proc.crashEvent;
  shared->header.state_flags = skydiag::kState_Frozen;
  Require(RearmFrozenCrashFromEarlierHelper(otherProc, outBase) == 2u, "another game's generation is its own");
  Require(ResetEvent(proc.crashEvent) != FALSE, "ResetEvent failed");

  // The game exits: the helper's shutdown deletes its markers, not another game's.
  TerminateWithExitCode(child, 1u);
  skydiag::helper::internal::ShutdownLoopState(MakeTestConfig(), proc, outBase, &state);
  Require(HasFileWithPrefix(outBase, skydiag::helper::internal::FrozenCrashRearmMarkerPrefix(otherProc)),
    "another game's marker stays");
  Require(!HasFileWithPrefix(outBase, skydiag::helper::internal::FrozenCrashRearmMarkerPrefix(proc)),
    "the exited game's marker is deleted");

  CloseHandle(proc.crashEvent);
  TerminateChildProcess(&other);
  TerminateChildProcess(&child);
  std::filesystem::remove_all(outBase);
}

// A crash released on a few seconds of heartbeat keeps its dump until the
// release is settled. When the exit then reports the fault after all, that
// dump becomes its crash dump instead of a metadata-only record, under its
// crash name again; when the exit does not report it, the dump is deleted.
void TestHandleProcessExitTick_SettlesTheHeldDumpOfAReleasedCrash()
{
  struct Case
  {
    const char* name;
    DWORD exitCode;
    std::uint64_t releasedAgoMs;
    bool expectReported;
  };
  const Case cases[] = {
    { "exit 1 five seconds after the release: reported with its dump", 1u, 5'000u, true },
    { "a normal exit: dump deleted", 0u, 5'000u, false },
    { "exit 1 long after the release: dump deleted", 1u, 90'000u, false },
  };
  for (const auto& c : cases) {
    const auto outBase = MakeTempDir(L"skydiag_helper_released_held_dump");
    ClearLog(outBase);
    const auto heldDump = outBase / L"SkyrimDiag_Released_20261019_120000_001.dmp";
    const auto crashDump = outBase / L"SkyrimDiag_Crash_20261019_120000_001.dmp";
    WriteAllTextUtf8(heldDump, "dump");

    auto shared = MakeSharedLayout();
    shared->header.crash_seq = 2u;
    shared->header.crash.exception_code = 0xC0000005u;
    shared->header.crash.exception_addr = 0x12345678u;
    shared->header.crash.faulting_tid = 4242u;
    shared->header.qpc_freq = QpcFrequency();
    shared->header.last_heartbeat_qpc = QpcNow();
    auto child = LaunchSleepingChildProcess();
    auto proc = MakeAttachedProcessForChild(child, shared.get());
    HelperConfig cfg = MakeTestConfig();

    skydiag::helper::internal::HelperLoopState state{};
    state.crashCaptured.releasedCrashSeq = 2u;
    state.crashCaptured.releasedOnBriefHeartbeat = true;
    state.crashCaptured.releasedCrashTick64 = GetTickCount64() - c.releasedAgoMs;
    state.crashCaptured.releasedDumpPath = heldDump.wstring();

    TerminateWithExitCode(child, c.exitCode);
    Require(HandleProcessExitTick(cfg, proc, outBase, &state), c.name);

    const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
    Require(state.crashCaptured.releasedDumpPath.empty(), c.name);
    if (c.expectReported) {
      if (!FileExists(crashDump)) {
        std::fprintf(stderr, "%s\n", log.c_str());
      }
      Require(FileExists(crashDump) && !FileExists(heldDump), c.name);
      Require(state.crashCaptured.latched && state.capturedCrashDumpPath == crashDump.wstring(), c.name);
      Require(!HasFileWithPrefix(outBase, L"SkyrimDiag_PostExitCrashEvidence_"), c.name);
      AssertContains(log, "reporting it as the crash dump", c.name);
    } else {
      Require(!FileExists(heldDump), c.name);
      Require(!state.crashCaptured.latched, c.name);
      AssertContains(log, "Released crash dump deleted", c.name);
    }
    TerminateChildProcess(&child);
    std::filesystem::remove_all(outBase);
  }
}

// The held dump goes once the game has outlived its exception (a minute
// alive, 30 s of advancing heartbeat), not before.
void TestExpireReleasedCrashDumpIfOutlived()
{
  using skydiag::helper::internal::ExpireReleasedCrashDumpIfOutlived;
  const auto outBase = MakeTempDir(L"skydiag_helper_released_dump_expiry");
  ClearLog(outBase);
  const auto heldDump = outBase / L"SkyrimDiag_Crash_20261019_120500_001.dmp";
  WriteAllTextUtf8(heldDump, "dump");
  auto shared = MakeSharedLayout();
  auto child = LaunchSleepingChildProcess();
  auto proc = MakeAttachedProcessForChild(child, shared.get());

  skydiag::helper::internal::HelperLoopState state{};
  state.crashCaptured.releasedDumpPath = heldDump.wstring();
  auto& recovery = state.crashCaptured.releasedDumpRecovery;
  const auto now = GetTickCount64();
  skydiag::helper::internal::StartLateRecovery(&recovery, now - 30'000, 100u);
  recovery.lastSampleTick64 = now - 6'000;
  recovery.advancingWindows = 5u;
  shared->header.last_heartbeat_qpc = 200u;
  ExpireReleasedCrashDumpIfOutlived(proc, outBase, &state);
  Require(FileExists(heldDump) && !state.crashCaptured.releasedDumpPath.empty(), "Too early: the dump is held");

  recovery.latchedTick64 = now - 120'000;
  recovery.lastSampleTick64 = now - 6'000;
  recovery.lastHeartbeatQpc = 200u;
  recovery.advancingWindows = 5u;
  shared->header.last_heartbeat_qpc = 300u;
  ExpireReleasedCrashDumpIfOutlived(proc, outBase, &state);
  Require(!FileExists(heldDump) && state.crashCaptured.releasedDumpPath.empty(), "Outlived: the dump is deleted");
  AssertContains(ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log"), "kept running for over a minute", "Logged");

  TerminateChildProcess(&child);
  std::filesystem::remove_all(outBase);
}

// A hang still open when the game exits with code 0 is a slow shutdown (a
// large load order quitting): the dump stays, but no freeze viewer opens
// after the game closed normally.
void TestLaunchDeferredViewersAfterExit_SkipsTheHangViewerForASlowShutdown()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_slow_shutdown_hang");
  ClearLog(outBase);
  HelperConfig cfg = MakeTestConfig();
  cfg.autoOpenViewerOnHang = true;
  cfg.autoOpenHangAfterProcessExit = true;
  cfg.autoOpenHangDelayMs = 0u;

  skydiag::helper::internal::HelperLoopState state{};
  state.pendingHangViewerDumpPath = L"C:\\Temp\\SyntheticHang.dmp";
  state.hangState.hangCapturedThisEpisode = true;
  LaunchDeferredViewersAfterExit(cfg, outBase, /*exitCode=*/0, &state);

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "Suppressed hang viewer launch", "the skipped viewer is logged");
  Require(log.find("Hang viewer auto-open attempt failed") == std::string::npos, "no viewer launch was tried");
  Require(log.find("Auto-opened DumpTool viewer") == std::string::npos, "no viewer launch was tried");
  std::filesystem::remove_all(outBase);
}

// A hang capture's ETW trace records for its window after the hang dump
// (it starts after the dump) and is then stopped from the helper loop, one
// wpr call per tick, so a crash is still noticed in between; the old
// synchronous finalize could block for minutes. The fake wpr here (this test
// exe) fails every -stop of this file and confirms -cancel.
void TestPendingHangEtwStopRunsOneWprCallPerTick()
{
  using skydiag::helper::internal::HangEtwCaptureSeconds;
  using skydiag::helper::internal::MaybeFinalizePendingHangEtwCapture;
  using skydiag::helper::internal::PendingHangEtwCapture;
  static_assert(HangEtwCaptureSeconds(20u) == 10u, "EtwMaxDurationSec=20 (default): a 10 s window");
  static_assert(HangEtwCaptureSeconds(4u) == 4u, "a shorter EtwMaxDurationSec is kept");
  const auto outBase = MakeTempDir(L"skydiag_hang_etw_pending");
  ClearLog(outBase);
  HelperConfig cfg = MakeTestConfig();
  cfg.etwWprExe = CurrentExecutablePath().wstring();
  const auto manifestPath = outBase / L"SkyrimDiag_Incident_Hang_20261019_120000_001.json";
  WriteAllTextUtf8(manifestPath, "{}");

  PendingHangEtwCapture pending{};
  pending.active = true;
  pending.etwPath = outBase / L"synthetic-hang.etl";
  pending.manifestPath = manifestPath;
  pending.startedAtTick64 = GetTickCount64();
  pending.captureSeconds = HangEtwCaptureSeconds(cfg.etwMaxDurationSec);
  MaybeFinalizePendingHangEtwCapture(cfg, outBase, /*force=*/false, &pending);
  Require(pending.active && pending.stopAttempts == 0u, "just started: the trace keeps recording");
  Require(!FileExists(outBase / "SkyrimDiagHelper.log") ||
            ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log").find("ETW hang capture") == std::string::npos,
    "no wpr call yet");

  pending.startedAtTick64 = GetTickCount64() - (static_cast<std::uint64_t>(pending.captureSeconds) * 1000u + 1u);
  MaybeFinalizePendingHangEtwCapture(cfg, outBase, /*force=*/false, &pending);
  Require(pending.active && pending.stopAttempts == 1u, "one wpr -stop on the first tick after the window");
  MaybeFinalizePendingHangEtwCapture(cfg, outBase, /*force=*/false, &pending);
  Require(pending.active && pending.stopAttempts == 2u, "one more on the next");
  MaybeFinalizePendingHangEtwCapture(cfg, outBase, /*force=*/false, &pending);
  Require(!pending.active, "after the last stop attempt, wpr -cancel settles it");

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "attempt 3 of 3", "each stop attempt is logged");
  AssertContains(log, "WPR cancellation was confirmed", "the cancel is logged");
  AssertContains(
    ReadAllTextUtf8(manifestPath), "\"etw_status\": \"cancelled_after_stop_failure\"", "the manifest gets the ETW outcome");

  // At helper shutdown (after the game exited) every remaining step runs at
  // once, within the window too.
  PendingHangEtwCapture atShutdown{};
  atShutdown.active = true;
  atShutdown.etwPath = outBase / L"synthetic-hang-2.etl";
  atShutdown.startedAtTick64 = GetTickCount64();
  atShutdown.captureSeconds = 10u;
  MaybeFinalizePendingHangEtwCapture(cfg, outBase, /*force=*/true, &atShutdown);
  Require(!atShutdown.active, "forced: settled in one call");
  std::filesystem::remove_all(outBase);
}

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
    TestWrongTypesInCrashBucketStatsAndSummaryAreDefaults();
    TestIncidentManifestUpdatePropagatesAtomicReplaceFailure();
    TestCleanupCrashArtifactsAfterZeroExit_RemovesHandledAccessViolationArtifacts();
    TestCleanupCrashArtifactsAfterZeroExit_RemovesItsRecapture();
    TestCleanupCrashArtifactsAfterZeroExit_StopsDelayedAnalyzerWriter();
    TestLaunchDeferredViewersAfterExit_SuppressesOnNormalExit();
    TestHandleProcessExitTick_TreatsStrongSharedMemoryExceptionAsHandledOnZeroExit();
    TestHandleProcessExitTick_DrainsLateZeroExitFaultMetadataWithoutDumpAttempt();
    TestHandleProcessExitTick_PreservesPendingEvidenceAfterFinalizationFailure();
    TestHandleProcessExitTick_DoesNotReReportAReleasedThreadDumpFault();
    TestHandleProcessExitTick_ReportsAnEarlyReleasedFaultEndedByCrashLogger();
    TestHandleProcessExitTick_DoesNotReportAnEarlyReleasedFaultWithoutACrashLog();
    TestHandleProcessExitTick_ThreadDumpWindowReleaseNeedsACrashLog();
    TestHandleProcessExitTick_ZeroExitDoesNotReportAReleasedStrongFault();
    TestHandleProcessExitTick_ReportsAFaultNewerThanTheReleasedOne();
    TestInitializeLoopState_SkipsAGenerationAnEarlierHelperAcknowledged();
    TestHandleProcessExitTick_ReportsAReleasedCrashFollowedByAnAbnormalExit();
    TestHandleProcessExitTick_AbnormalExitHintOnlyForAResponsiveGame();
    TestHandleProcessExitTick_KeepsTheDumpFailureHint();
    TestThreadDumpFaultOfAnExitedGame_IsRecordedAsADumpFailure();
    TestFindForcedCloseInSnapshot();
    TestForcedCloseCrash_SkipsTheViewer();
    TestPreservedFilteredDump_DoesNotKeepCaptureLatch();
    TestCleanExitDumpDeletionFailureIsRecordedAsPreserved();
    TestEtwStopFailureRetriesThenConfirmsCancellation();
    TestEtwStopExitZeroRequiresNonEmptyArtifact();
    TestLateRecoveryNeedsAMinuteOfAdvancingHeartbeat();
    TestHandleLateRecoveryTick_ReleasesACrashTheGameOutlived();
    TestHandleLateRecoveryTick_KeepsACrashWithoutHeartbeat();
    TestOutputDirResolvesAgainstTheHelperFolder();
    TestShutdownLeavesTheAnalyzerRunningUnlessTheViewerOpened();
    TestAnalysisTimeoutLeavesTheReportToFinish();
    TestInitializeLoopState_RearmsAFrozenCrashNoHelperHandles();
    TestRearmFrozenCrash_SignalsAGenerationAgainOnlyOnce();
    TestHandleProcessExitTick_SettlesTheHeldDumpOfAReleasedCrash();
    TestExpireReleasedCrashDumpIfOutlived();
    TestLaunchDeferredViewersAfterExit_SkipsTheHangViewerForASlowShutdown();
    TestPendingHangEtwStopRunsOneWprCallPerTick();
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
