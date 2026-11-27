// ADR-0038 runtime tests: exercise HandleProcessExitTick end-to-end across
// every exit branch with a swappable fake WER lookup (no real Windows Event
// Log dependency and no 1 s sleep).

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "CrashCapture.h"
#include "HelperLog.h"
#include "HelperMainInternal.h"
#include "HelperRuntimeTestUtils.h"
#include "WerExitEvent.h"

using skydiag::helper::HelperConfig;
using skydiag::helper::internal::ClearLog;
using skydiag::helper::internal::HandleProcessExitTick;
using skydiag::helper::internal::HelperLoopState;
using skydiag::helper::internal::kExitCodeClosedAsNotResponding;
using skydiag::helper::internal::SetWerExitEventLineLookupOverrideForTests;
using skydiag::helper::internal::WerExitEventLineLookupFn;
using skydiag::tests::runtime::AssertContains;
using skydiag::tests::runtime::FileExists;
using skydiag::tests::runtime::MakeSharedLayout;
using skydiag::tests::runtime::MakeTempDir;
using skydiag::tests::runtime::MakeTestConfig;
using skydiag::tests::runtime::ReadAllTextUtf8;
using skydiag::tests::runtime::Require;

namespace {

constexpr const wchar_t* kFakeWerLineW =
  L"Windows Error Reporting recorded this exit (Application log, event 1000): "
  L"exception 0xc0000005 in ntdll.dll 10.0.26100.9974 at offset 0x55ae0. "
  L"That is where the process ended, not necessarily where the problem began.";
constexpr const char* kFakeWerLineUtf8 =
  "Windows Error Reporting recorded this exit (Application log, event 1000): "
  "exception 0xc0000005 in ntdll.dll 10.0.26100.9974 at offset 0x55ae0. "
  "That is where the process ended, not necessarily where the problem began.";

std::atomic<int> g_lookupCalls{ 0 };
std::atomic<std::uint32_t> g_lastLookupPid{ 0 };
std::atomic<std::uint32_t> g_lastLookupExitCode{ 0 };
std::atomic<bool> g_throwOnLookup{ false };

void ResetFakeLookupState()
{
  g_lookupCalls.store(0);
  g_lastLookupPid.store(0);
  g_lastLookupExitCode.store(0);
  g_throwOnLookup.store(false);
}

std::optional<std::wstring> FakeWerLookup(HANDLE, std::uint32_t processId, std::uint32_t exitCode)
{
  g_lookupCalls.fetch_add(1);
  g_lastLookupPid.store(processId);
  g_lastLookupExitCode.store(exitCode);
  if (g_throwOnLookup.load()) {
    throw std::runtime_error("synthetic WER lookup exception");
  }
  return std::wstring(kFakeWerLineW);
}

struct ScopedWerLookupOverride
{
  explicit ScopedWerLookupOverride(WerExitEventLineLookupFn fn)
    : previous_(SetWerExitEventLineLookupOverrideForTests(fn))
  {}
  ~ScopedWerLookupOverride() { SetWerExitEventLineLookupOverrideForTests(previous_); }
  WerExitEventLineLookupFn previous_ = nullptr;
};

struct ExitedChild
{
  PROCESS_INFORMATION pi{};
  ~ExitedChild()
  {
    if (pi.hThread) {
      CloseHandle(pi.hThread);
    }
    if (pi.hProcess) {
      CloseHandle(pi.hProcess);
    }
  }
};

// Launches a lightweight copy of this test executable in --sleep mode and ends
// it with `exitCode`, returning an open process handle with that exact exit code.
ExitedChild MakeExitedChild(DWORD exitCode)
{
  wchar_t exePath[MAX_PATH]{};
  const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
  Require(len > 0 && len < MAX_PATH, "GetModuleFileNameW failed");
  std::wstring cmdLine = L"\"" + std::wstring(exePath) + L"\" --sleep";

  ExitedChild child{};
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  const BOOL ok = CreateProcessW(
    exePath,
    cmdLine.data(),
    nullptr,
    nullptr,
    FALSE,
    CREATE_NO_WINDOW,
    nullptr,
    nullptr,
    &si,
    &child.pi);
  Require(ok != FALSE, "CreateProcessW(self --sleep) failed");
  Require(TerminateProcess(child.pi.hProcess, exitCode) != FALSE, "TerminateProcess failed");
  Require(WaitForSingleObject(child.pi.hProcess, 5000) == WAIT_OBJECT_0, "Child did not exit");
  return child;
}

std::uint64_t QpcNow()
{
  LARGE_INTEGER v{};
  QueryPerformanceCounter(&v);
  return static_cast<std::uint64_t>(v.QuadPart);
}

std::uint64_t QpcFreq()
{
  LARGE_INTEGER v{};
  QueryPerformanceFrequency(&v);
  return static_cast<std::uint64_t>(v.QuadPart);
}

skydiag::helper::AttachedProcess AttachExitedChild(const ExitedChild& child, skydiag::SharedLayout* shared)
{
  skydiag::helper::AttachedProcess proc{};
  proc.pid = child.pi.dwProcessId;
  proc.process = child.pi.hProcess;
  proc.shm = shared;
  proc.shmWritable = shared;
  proc.shmSize = sizeof(skydiag::SharedLayout);
  return proc;
}

void AssertOrderInLog(const std::string& log, std::string_view first, std::string_view second, const char* msg)
{
  const auto p1 = log.find(first);
  const auto p2 = log.find(second);
  Require(p1 != std::string::npos && p2 != std::string::npos && p1 < p2, msg);
}

// Target branch 1: hangSkippedAsClosingThisEpisode && exitCode < 0xC0000000
void TestClosingHangBranchLogsWerLineAfterExistingLine()
{
  for (const bool enableHint : { true, false }) {
    ResetFakeLookupState();
    const auto outBase = MakeTempDir(L"skydiag_wer_rt_closing");
    ClearLog(outBase);

    auto shared = MakeSharedLayout();
    shared->header.qpc_freq = QpcFreq();
    shared->header.last_heartbeat_qpc = QpcNow();
    auto child = MakeExitedChild(1u);
    auto proc = AttachExitedChild(child, shared.get());

    HelperConfig cfg = MakeTestConfig();
    cfg.enableWerDumpFallbackHint = enableHint;
    HelperLoopState state{};
    state.hangState.hangSkippedAsClosingThisEpisode = true;

    Require(HandleProcessExitTick(cfg, proc, outBase, &state), "HandleProcessExitTick must return true");
    Require(g_lookupCalls.load() == 1, "Closing-hang branch must invoke WER lookup once");
    Require(g_lastLookupPid.load() == proc.pid && g_lastLookupExitCode.load() == 1u, "Passed pid/exitCode");
    Require(!FileExists(outBase / "SkyrimDiag_WER_LocalDumps_Hint.txt"), "Closing-hang branch writes no hint file");

    const std::string log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
    AssertOrderInLog(
      log,
      "The game exited while it was still closing",
      kFakeWerLineUtf8,
      "WER line must follow the closing-hang log line");
    std::filesystem::remove_all(outBase);
  }
}

// Target branch 2: IsCloseOfUnresponsiveGame with exitCode != 0xCFFFFFFF
void TestUnresponsiveCloseBranchLogsWerLineAfterExistingLine()
{
  for (const bool enableHint : { true, false }) {
    ResetFakeLookupState();
    const auto outBase = MakeTempDir(L"skydiag_wer_rt_unresp");
    ClearLog(outBase);

    auto shared = MakeSharedLayout();
    shared->header.qpc_freq = QpcFreq();
    shared->header.last_heartbeat_qpc = QpcNow();
    auto child = MakeExitedChild(1u);
    auto proc = AttachExitedChild(child, shared.get());

    HelperConfig cfg = MakeTestConfig();
    cfg.enableWerDumpFallbackHint = enableHint;
    HelperLoopState state{};
    state.hangState.hangCapturedThisEpisode = true;

    Require(HandleProcessExitTick(cfg, proc, outBase, &state), "HandleProcessExitTick must return true");
    Require(g_lookupCalls.load() == 1, "Unresponsive non-0xCFFFFFFF branch must invoke WER lookup once");
    Require(!FileExists(outBase / "SkyrimDiag_WER_LocalDumps_Hint.txt"), "Unresponsive branch writes no hint file");

    const std::string log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
    AssertOrderInLog(
      log,
      "The game stopped responding and then exited",
      kFakeWerLineUtf8,
      "WER line must follow the unresponsive-exit log line");
    std::filesystem::remove_all(outBase);
  }
}

// Target branch 3: Abnormal exit without recorded crash, with EnableWerDumpFallbackHint=1 and =0.
void TestAbnormalExitBranchLogsWerLineRegardlessOfHintConfig()
{
  // EnableWerDumpFallbackHint = 1: writes hint file + hint log line, THEN WER line.
  {
    ResetFakeLookupState();
    const auto outBase = MakeTempDir(L"skydiag_wer_rt_else_hint1");
    ClearLog(outBase);

    auto shared = MakeSharedLayout();
    shared->header.qpc_freq = QpcFreq();
    shared->header.last_heartbeat_qpc = QpcNow();
    auto child = MakeExitedChild(0xC0000409u);
    auto proc = AttachExitedChild(child, shared.get());

    HelperConfig cfg = MakeTestConfig();
    cfg.enableWerDumpFallbackHint = true;
    HelperLoopState state{};

    Require(HandleProcessExitTick(cfg, proc, outBase, &state), "HandleProcessExitTick must return true");
    Require(g_lookupCalls.load() == 1, "Abnormal exit (hint=1) must invoke WER lookup once");
    Require(g_lastLookupExitCode.load() == 0xC0000409u, "Passed fail-fast exitCode");
    Require(FileExists(outBase / "SkyrimDiag_WER_LocalDumps_Hint.txt"), "Hint=1 must write hint file");

    const std::string log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
    AssertOrderInLog(
      log,
      "wrote WER LocalDumps fallback guidance",
      kFakeWerLineUtf8,
      "WER line must follow the fallback guidance log line");
    std::filesystem::remove_all(outBase);
  }

  // EnableWerDumpFallbackHint = 0: NO hint file, NO hint log line, STILL invokes WER lookup and logs line.
  {
    ResetFakeLookupState();
    const auto outBase = MakeTempDir(L"skydiag_wer_rt_else_hint0");
    ClearLog(outBase);

    auto shared = MakeSharedLayout();
    shared->header.qpc_freq = QpcFreq();
    shared->header.last_heartbeat_qpc = QpcNow();
    auto child = MakeExitedChild(0xC0000005u);
    auto proc = AttachExitedChild(child, shared.get());

    HelperConfig cfg = MakeTestConfig();
    cfg.enableWerDumpFallbackHint = false;
    HelperLoopState state{};

    Require(HandleProcessExitTick(cfg, proc, outBase, &state), "HandleProcessExitTick must return true");
    Require(g_lookupCalls.load() == 1, "Abnormal exit (hint=0) must STILL invoke WER lookup once");
    Require(!FileExists(outBase / "SkyrimDiag_WER_LocalDumps_Hint.txt"), "Hint=0 must not write hint file");

    const std::string log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
    Require(log.find("wrote WER LocalDumps fallback guidance") == std::string::npos, "Hint=0 must not log hint");
    AssertContains(log, kFakeWerLineUtf8, "Hint=0 must still log the WER event line");
    std::filesystem::remove_all(outBase);
  }
}

// Excluded branches: exit 0, latched crash, crashOnRecord, dumpWriteFailedCrashSeq != 0, 0xCFFFFFFF.
void TestExcludedBranchesNeverInvokeWerLookup()
{
  // 1. Exit code 0
  {
    ResetFakeLookupState();
    const auto outBase = MakeTempDir(L"skydiag_wer_rt_ex0");
    ClearLog(outBase);

    auto shared = MakeSharedLayout();
    shared->header.qpc_freq = QpcFreq();
    shared->header.last_heartbeat_qpc = QpcNow();
    auto child = MakeExitedChild(0u);
    auto proc = AttachExitedChild(child, shared.get());

    HelperConfig cfg = MakeTestConfig();
    cfg.enableWerDumpFallbackHint = true;
    HelperLoopState state{};

    Require(HandleProcessExitTick(cfg, proc, outBase, &state), "exit 0 tick");
    Require(g_lookupCalls.load() == 0, "Exit code 0 must not invoke WER lookup");
    const std::string log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
    Require(log.find(kFakeWerLineUtf8) == std::string::npos, "Exit code 0 must not log WER line");
    std::filesystem::remove_all(outBase);
  }

  // 2. Captured crash latched
  {
    ResetFakeLookupState();
    const auto outBase = MakeTempDir(L"skydiag_wer_rt_latched");
    ClearLog(outBase);

    auto shared = MakeSharedLayout();
    shared->header.qpc_freq = QpcFreq();
    shared->header.last_heartbeat_qpc = QpcNow();
    auto child = MakeExitedChild(0xC0000005u);
    auto proc = AttachExitedChild(child, shared.get());

    HelperConfig cfg = MakeTestConfig();
    cfg.enableWerDumpFallbackHint = true;
    HelperLoopState state{};
    state.crashCaptured.latched = true;
    state.crashCaptured.capturedInfo.crashSeq = 2u;
    state.crashCaptured.capturedInfo.exceptionCode = 0xC0000005u;

    Require(HandleProcessExitTick(cfg, proc, outBase, &state), "latched crash tick");
    Require(g_lookupCalls.load() == 0, "Captured crash latched must not invoke WER lookup");
    const std::string log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
    Require(log.find(kFakeWerLineUtf8) == std::string::npos, "Latched crash must not log WER line");
    std::filesystem::remove_all(outBase);
  }

  // 3. crashOnRecord (committed fault in shared memory drained at exit)
  {
    ResetFakeLookupState();
    const auto outBase = MakeTempDir(L"skydiag_wer_rt_onrecord");
    ClearLog(outBase);

    auto shared = MakeSharedLayout();
    shared->header.qpc_freq = QpcFreq();
    shared->header.last_heartbeat_qpc = QpcNow();
    shared->header.crash_seq = 2u;  // committed
    shared->header.crash.exception_code = 0xC0000005u;
    shared->header.crash.faulting_tid = 1234u;
    shared->header.crash.exception_addr = 0x7ff612340000ull;
    auto child = MakeExitedChild(0xC0000005u);
    auto proc = AttachExitedChild(child, shared.get());

    HelperConfig cfg = MakeTestConfig();
    cfg.enableWerDumpFallbackHint = true;
    HelperLoopState state{};

    Require(HandleProcessExitTick(cfg, proc, outBase, &state), "crashOnRecord tick");
    Require(g_lookupCalls.load() == 0, "crashOnRecord must not invoke WER lookup");
    const std::string log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
    AssertContains(log, "crash on record as post-exit metadata", "Existing crashOnRecord log line preserved");
    Require(log.find(kFakeWerLineUtf8) == std::string::npos, "crashOnRecord must not log WER line");
    std::filesystem::remove_all(outBase);
  }

  // 4. dumpWriteFailedCrashSeq != 0
  {
    ResetFakeLookupState();
    const auto outBase = MakeTempDir(L"skydiag_wer_rt_dumpfail");
    ClearLog(outBase);

    auto shared = MakeSharedLayout();
    shared->header.qpc_freq = QpcFreq();
    shared->header.last_heartbeat_qpc = QpcNow();
    auto child = MakeExitedChild(0xC0000005u);
    auto proc = AttachExitedChild(child, shared.get());

    HelperConfig cfg = MakeTestConfig();
    cfg.enableWerDumpFallbackHint = true;
    HelperLoopState state{};
    state.crashCaptured.dumpWriteFailedCrashSeq = 4u;

    Require(HandleProcessExitTick(cfg, proc, outBase, &state), "dumpWriteFailedCrashSeq tick");
    Require(g_lookupCalls.load() == 0, "dumpWriteFailedCrashSeq != 0 must not invoke WER lookup");
    const std::string log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
    AssertContains(log, "whose dump could not be written", "Existing dump-failure log line preserved");
    Require(log.find(kFakeWerLineUtf8) == std::string::npos, "dumpWriteFailedCrashSeq must not log WER line");
    std::filesystem::remove_all(outBase);
  }

  // 5. 0xCFFFFFFF (closed by Windows as not responding -> event 1002, not 1000)
  for (const bool modalDialogSeen : { false, true }) {
    ResetFakeLookupState();
    const auto outBase = MakeTempDir(L"skydiag_wer_rt_notresp");
    ClearLog(outBase);

    auto shared = MakeSharedLayout();
    shared->header.qpc_freq = QpcFreq();
    shared->header.last_heartbeat_qpc = QpcNow();
    auto child = MakeExitedChild(kExitCodeClosedAsNotResponding);
    auto proc = AttachExitedChild(child, shared.get());

    HelperConfig cfg = MakeTestConfig();
    cfg.enableWerDumpFallbackHint = true;
    HelperLoopState state{};
    state.hangState.hangCapturedThisEpisode = true;
    state.hangState.modalDialogSeenThisEpisode = modalDialogSeen;

    Require(HandleProcessExitTick(cfg, proc, outBase, &state), "0xCFFFFFFF tick");
    Require(g_lookupCalls.load() == 0, "0xCFFFFFFF must never invoke WER lookup");
    const std::string log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
    if (!modalDialogSeen) {
      AssertContains(log, "closed by Windows as not responding", "Existing not-responding log line preserved");
      Require(!FileExists(outBase / "SkyrimDiag_WER_LocalDumps_Hint.txt"), "Not-responding close writes no hint");
    }
    Require(log.find(kFakeWerLineUtf8) == std::string::npos, "0xCFFFFFFF must not log WER line");
    std::filesystem::remove_all(outBase);
  }
}

// Exception safety (Item 2): an exception during WER lookup must not interrupt
// HandleProcessExitTick's remaining exit handling.
void TestExceptionInWerLookupDoesNotInterruptExitTick()
{
  ResetFakeLookupState();
  g_throwOnLookup.store(true);

  const auto outBase = MakeTempDir(L"skydiag_wer_rt_throw");
  ClearLog(outBase);

  auto shared = MakeSharedLayout();
  shared->header.qpc_freq = QpcFreq();
  shared->header.last_heartbeat_qpc = QpcNow();
  auto child = MakeExitedChild(0xC0000005u);
  auto proc = AttachExitedChild(child, shared.get());

  HelperConfig cfg = MakeTestConfig();
  cfg.enableWerDumpFallbackHint = false;
  HelperLoopState state{};

  Require(HandleProcessExitTick(cfg, proc, outBase, &state), "HandleProcessExitTick must survive lookup exception");
  Require(g_lookupCalls.load() == 1, "Lookup was attempted");
  const std::string log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  Require(log.find(kFakeWerLineUtf8) == std::string::npos, "Failed lookup writes no WER line");
  AssertContains(log, "Target process exited (exit_code=", "Remaining exit handling must still complete");
  std::filesystem::remove_all(outBase);
}

}  // namespace

int main(int argc, char** argv)
{
  if (argc == 2 && std::strcmp(argv[1], "--sleep") == 0) {
    Sleep(30000);
    return 0;
  }
  try {
    ScopedWerLookupOverride overrideGuard(&FakeWerLookup);
    SKYDIAG_RUN_TEST(TestClosingHangBranchLogsWerLineAfterExistingLine);
    SKYDIAG_RUN_TEST(TestUnresponsiveCloseBranchLogsWerLineAfterExistingLine);
    SKYDIAG_RUN_TEST(TestAbnormalExitBranchLogsWerLineRegardlessOfHintConfig);
    SKYDIAG_RUN_TEST(TestExcludedBranchesNeverInvokeWerLookup);
    SKYDIAG_RUN_TEST(TestExceptionInWerLookupDoesNotInterruptExitTick);
    std::puts("wer exit runtime tests passed");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
