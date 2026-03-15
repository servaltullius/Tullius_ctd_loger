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
using skydiag::helper::internal::HandleCrashEventTick;
using skydiag::helper::internal::PendingCrashAnalysis;
using skydiag::helper::internal::PendingCrashEtwCapture;
using skydiag::helper::internal::ShutdownRetentionWorker;
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
  shared->header.crash_seq = 1;
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

  bool crashCaptured = false;
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
    &crashCaptured,
    &pendingCrashEtw,
    &pendingCrashAnalysis,
    &lastCrashDumpPath,
    &pendingHangViewerDumpPath,
    &pendingCrashViewerDumpPath);

  Require(handled, "Crash event should be consumed");
  Require(crashCaptured, "Crash capture state should flip true");
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

void TestCleanupCrashArtifactsAfterZeroExit_PreservesStrongCrashArtifacts()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_runtime_preserve");
  ClearLog(outBase);

  const auto dumpPath = outBase / "SkyrimDiag_Crash_20260315_120000_001.dmp";
  WriteAllTextUtf8(dumpPath, "dump");

  HelperConfig cfg = MakeTestConfig();
  skydiag::helper::AttachedProcess proc{};
  skydiag::helper::internal::HelperLoopState state{};
  state.crashCaptured = true;
  state.capturedCrashDumpPath = dumpPath.wstring();
  state.pendingCrashViewerDumpPath = dumpPath.wstring();

  CleanupCrashArtifactsAfterZeroExit(
    cfg,
    proc,
    outBase,
    /*exitCode0StrongCrash=*/true,
    /*exceptionCode=*/0xC0000005u,
    &state);

  Require(FileExists(dumpPath), "Strong zero-exit crash must preserve artifacts");
  Require(state.crashCaptured, "Strong zero-exit crash must preserve capture state");
  Require(!state.pendingCrashViewerDumpPath.empty(), "Strong zero-exit crash must preserve deferred viewer path");

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "preserving crash artifacts", "Strong zero-exit crash must be logged as preserved");

  std::filesystem::remove_all(outBase);
}

}  // namespace

int main()
{
  try {
    TestHandleCrashEventTick_WritesCrashArtifacts();
    TestCleanupCrashArtifactsAfterZeroExit_PreservesStrongCrashArtifacts();
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
