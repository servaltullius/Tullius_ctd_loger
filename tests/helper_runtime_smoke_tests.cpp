#include <Windows.h>

#include <cstdio>
#include <filesystem>
#include <exception>
#include <string>

#include "CrashCapture.h"
#include "CrashEtwCapture.h"
#include "HelperLog.h"
#include "HelperMainInternal.h"
#include "HelperRuntimeTestUtils.h"
#include "PendingCrashAnalysis.h"
#include "RetentionWorker.h"

using skydiag::helper::HelperConfig;
using skydiag::helper::internal::ClearLog;
using skydiag::helper::internal::CleanupCrashArtifactsAfterZeroExit;
using skydiag::helper::internal::CaptureStableSharedSnapshot;
using skydiag::helper::internal::CrashCaptureState;
using skydiag::helper::internal::ExtractCrashInfo;
using skydiag::helper::internal::HandleCrashEventTick;
using skydiag::helper::internal::PendingCrashAnalysis;
using skydiag::helper::internal::PendingCrashEtwCapture;
using skydiag::helper::internal::ShutdownRetentionWorker;
using skydiag::helper::internal::StableSharedSnapshot;
using skydiag::helper::internal::TryClearRecoveredCrashFreeze;
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

void TestHandleCrashEventTick_WritesCrashArtifacts()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_runtime_smoke");
  ClearLog(outBase);

  auto shared = MakeSharedLayout();
  shared->header.crash_seq = 2;
  shared->header.crash.exception_code = 0xC0000005u;
  auto child = LaunchSleepingChildProcess();
  shared->header.crash.faulting_tid = child.pi.dwThreadId;
  shared->header.crash.exception_addr = reinterpret_cast<std::uint64_t>(shared.get());
  shared->header.crash.exception_record.ExceptionCode = 0xC0000005u;
  shared->header.crash.exception_record.ExceptionAddress = reinterpret_cast<PVOID>(shared.get());
  RtlCaptureContext(&shared->header.crash.context);

  auto proc = MakeAttachedProcessForChild(child, shared.get());
  proc.crashEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
  Require(proc.crashEvent != nullptr, "CreateEventW failed");

  HelperConfig cfg = MakeTestConfig();
  cfg.autoOpenViewerOnCrash = false;
  cfg.autoAnalyzeDump = false;
  cfg.enableIncidentManifest = true;

  CrashCaptureState crashState{};
  PendingCrashEtwCapture pendingCrashEtw{};
  PendingCrashAnalysis pendingCrashAnalysis{};
  std::wstring lastCrashDumpPath;
  std::wstring pendingHangViewerDumpPath;
  std::wstring pendingCrashViewerDumpPath;

  const bool handled = HandleCrashEventTick(
    cfg,
    proc,
    outBase,
    /*waitMs=*/0,
    &crashState,
    &pendingCrashEtw,
    &pendingCrashAnalysis,
    &lastCrashDumpPath,
    &pendingHangViewerDumpPath,
    &pendingCrashViewerDumpPath);

  Require(handled, "Crash event should be consumed");
  Require(crashState.latched, "Crash capture state should flip true");
  Require(
    crashState.capturedInfo.crashSeq == 2u &&
      crashState.capturedInfo.exceptionCode == 0xC0000005u,
    "Crash capture state must retain the immutable crash metadata for exit-time classification");
  Require(!lastCrashDumpPath.empty(), "Crash dump path should be recorded");
  Require(FileExists(lastCrashDumpPath), "Crash dump file must exist");
  Require(
    FileExists(FindSingleFileByPrefix(outBase, L"SkyrimDiag_Incident_Crash_", L".json")),
    "Crash incident manifest must be written");

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "Crash event signaled", "Crash capture must log crash-event intake");
  AssertContains(log, "Incident manifest written", "Crash capture must log incident manifest creation");
  AssertContains(log, "Crash captured; waiting for process exit.", "Crash capture must log post-capture state");

  ShutdownRetentionWorker();
  CloseHandle(proc.crashEvent);
  proc.crashEvent = nullptr;
  TerminateChildProcess(&child);
  std::filesystem::remove_all(outBase);
}

void TestRecoveredCrashThaw_AllowsStableFollowupSnapshot()
{
  auto shared = MakeSharedLayout();
  shared->header.crash_seq = 2u;
  shared->header.crash.exception_code = 0xC0000005u;
  shared->header.crash.exception_addr = 0x1111u;
  shared->header.crash.faulting_tid = 101u;
  shared->header.state_flags = skydiag::kState_Frozen;

  StableSharedSnapshot first{};
  Require(
    CaptureStableSharedSnapshot(shared.get(), sizeof(*shared), &first),
    "Committed crash record must produce a stable helper snapshot");
  Require(
    (reinterpret_cast<std::uintptr_t>(first.layout()) % alignof(skydiag::SharedLayout)) == 0u,
    "Stable helper snapshot storage must satisfy SharedLayout alignment");
  const auto firstInfo = ExtractCrashInfo(&first.layout()->header);
  Require(firstInfo.crashSeq == 2u, "Stable snapshot must retain its committed crash sequence");
  Require(firstInfo.exceptionAddr == 0x1111u, "Stable snapshot must retain the first crash record");

  Require(
    TryClearRecoveredCrashFreeze(shared.get(), firstInfo.crashSeq),
    "Recovered first-chance exception must thaw the matching frozen record");
  Require(
    (shared->header.state_flags & skydiag::kState_Frozen) == 0u,
    "Recovered exception must resume blackbox/resource telemetry");

  // Model the next committed crash. The old immutable snapshot must stay
  // unchanged while a new stable snapshot observes only the follow-up record.
  shared->header.crash_seq = 3u;
  shared->header.crash.exception_code = 0xC000001Du;
  shared->header.crash.exception_addr = 0x2222u;
  shared->header.crash.faulting_tid = 202u;
  shared->header.state_flags |= skydiag::kState_Frozen;
  MemoryBarrier();
  shared->header.crash_seq = 4u;

  Require(
    ExtractCrashInfo(&first.layout()->header).exceptionAddr == 0x1111u,
    "Previously captured snapshot must remain immutable after a follow-up crash");

  StableSharedSnapshot second{};
  Require(
    CaptureStableSharedSnapshot(shared.get(), sizeof(*shared), &second),
    "Follow-up crash must produce a new stable snapshot after recovery");
  const auto secondInfo = ExtractCrashInfo(&second.layout()->header);
  Require(secondInfo.crashSeq == 4u, "Follow-up snapshot must expose the new committed sequence");
  Require(secondInfo.exceptionAddr == 0x2222u, "Follow-up snapshot must not mix the prior crash record");
  Require(secondInfo.faultingTid == 202u, "Follow-up snapshot must retain one coherent crash record");
}

void TestHandleCrashEventTick_RejectsUncommittedCrashSequenceBeforeDump()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_runtime_zero_seq");
  ClearLog(outBase);

  auto shared = MakeSharedLayout();
  shared->header.crash_seq = 0u;
  shared->header.crash.exception_code = 0xC0000005u;
  shared->header.crash.exception_addr = 0x1234u;
  shared->header.state_flags = skydiag::kState_Frozen;

  auto child = LaunchSleepingChildProcess();
  auto proc = MakeAttachedProcessForChild(child, shared.get());
  proc.crashEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
  Require(proc.crashEvent != nullptr, "CreateEventW failed");

  HelperConfig cfg = MakeTestConfig();
  CrashCaptureState crashState{};
  PendingCrashEtwCapture pendingCrashEtw{};
  PendingCrashAnalysis pendingCrashAnalysis{};
  std::wstring lastCrashDumpPath;
  std::wstring pendingHangViewerDumpPath;
  std::wstring pendingCrashViewerDumpPath;

  const bool handled = HandleCrashEventTick(
    cfg,
    proc,
    outBase,
    /*waitMs=*/0,
    &crashState,
    &pendingCrashEtw,
    &pendingCrashAnalysis,
    &lastCrashDumpPath,
    &pendingHangViewerDumpPath,
    &pendingCrashViewerDumpPath);

  Require(!handled, "Zero crash sequence must be rejected before dump capture");
  Require(!crashState.latched, "Rejected crash sequence must not set capture latch");
  Require(lastCrashDumpPath.empty(), "Rejected crash sequence must not expose a dump path");

  shared->header.crash_seq = 1u;
  Require(SetEvent(proc.crashEvent) != FALSE, "SetEvent failed for odd-sequence check");
  const bool oddHandled = HandleCrashEventTick(
    cfg,
    proc,
    outBase,
    /*waitMs=*/0,
    &crashState,
    &pendingCrashEtw,
    &pendingCrashAnalysis,
    &lastCrashDumpPath,
    &pendingHangViewerDumpPath,
    &pendingCrashViewerDumpPath);
  Require(!oddHandled, "Odd crash sequence must be rejected before dump capture");
  Require(!crashState.latched, "Odd crash sequence must not set capture latch");
  Require(lastCrashDumpPath.empty(), "Odd crash sequence must not expose a dump path");

  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(outBase, ec)) {
    Require(!ec, "Failed to enumerate zero-sequence output directory");
    Require(entry.path().extension() != L".dmp", "Zero crash sequence must not write a dump file");
  }
  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "crash_seq is not a non-zero committed sequence", "Rejected sequence must be logged");

  CloseHandle(proc.crashEvent);
  proc.crashEvent = nullptr;
  TerminateChildProcess(&child);
  std::filesystem::remove_all(outBase);
}

void TestCleanupCrashArtifactsAfterZeroExit_RemovesHandledStrongCrashArtifacts()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_runtime_preserve");
  ClearLog(outBase);

  const auto dumpPath = outBase / "SkyrimDiag_Crash_20260315_120000_001.dmp";
  WriteAllTextUtf8(dumpPath, "dump");

  HelperConfig cfg = MakeTestConfig();
  skydiag::helper::AttachedProcess proc{};
  skydiag::helper::internal::HelperLoopState state{};
  state.crashCaptured.latched = true;
  state.capturedCrashDumpPath = dumpPath.wstring();
  state.pendingCrashViewerDumpPath = dumpPath.wstring();

  CleanupCrashArtifactsAfterZeroExit(cfg, proc, outBase, &state);

  Require(!FileExists(dumpPath), "Handled strong exception with exit_code=0 must remove artifacts");
  Require(!state.crashCaptured.latched, "Handled strong exception with exit_code=0 must clear capture state");
  Require(state.pendingCrashViewerDumpPath.empty(), "Handled strong exception with exit_code=0 must suppress viewer");

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "removed", "Handled strong zero-exit exception must be logged as filtered");

  std::filesystem::remove_all(outBase);
}

}  // namespace

int main()
{
  try {
    TestHandleCrashEventTick_WritesCrashArtifacts();
    TestRecoveredCrashThaw_AllowsStableFollowupSnapshot();
    TestHandleCrashEventTick_RejectsUncommittedCrashSequenceBeforeDump();
    TestCleanupCrashArtifactsAfterZeroExit_RemovesHandledStrongCrashArtifacts();
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
