// Runs the plugin's real vectored handler against a stand-in CrashLogger.dll.
//
// Field case (v0.2.59-rc2): a mod's assert raised a breakpoint, which the
// fatal-only filter does not record. CrashLogger reported it and faulted in its
// own memory probe, and that probe was recorded as the crash, so the dump and
// report pointed at CrashLogger.dll instead of the mod.

#include <Windows.h>
#include <Psapi.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>

#include "SkyrimDiag/CrashHandler.h"
#include "SkyrimDiag/SharedMemory.h"
#include "SkyrimDiagHelper/ProcessAttach.h"
#include "SkyrimDiagShared.h"

// CloseRequestHooks.cpp needs CommonLibSSE; with no game window there the
// crash handler's call into it records nothing anyway (ADR-0022).
namespace skydiag::plugin {
void RecordCloseRequestAtCrash() noexcept {}
}  // namespace skydiag::plugin

namespace {

constexpr std::uint32_t kBreakpoint = 0x80000003u;
constexpr std::uint32_t kCppException = 0xE06D7363u;

using ProbeFn = int (*)();
ProbeFn g_probe = nullptr;
using CallGameCodeFn = int (*)(int (*)());
CallGameCodeFn g_callGameCode = nullptr;

void Require(bool condition, const char* message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

struct ModuleRange
{
  std::uint64_t begin = 0;
  std::uint64_t end = 0;

  bool Contains(std::uint64_t address) const { return address >= begin && address < end; }
};

ModuleRange RangeOf(HMODULE module)
{
  MODULEINFO info{};
  Require(GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)) != 0, "GetModuleInformation failed");
  const auto begin = reinterpret_cast<std::uint64_t>(info.lpBaseOfDll);
  return { begin, begin + info.SizeOfImage };
}

int ProbeFromFilter()
{
  g_probe();
  return EXCEPTION_EXECUTE_HANDLER;
}

// CrashLogger runs while the breakpoint is still being dispatched, like the
// filter expression here, and faults in its probe on the same thread.
void BreakpointReportedByCrashLogger()
{
  __try {
    __debugbreak();
  } __except (ProbeFromFilter()) {
  }
}

void HandledBreakpoint()
{
  __try {
    __debugbreak();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

void RearmIncident(skydiag::SharedLayout* shm)
{
  InterlockedAnd(
    reinterpret_cast<volatile LONG*>(&shm->header.state_flags),
    ~static_cast<LONG>(skydiag::kState_Frozen));
}

std::uint32_t CrashSeq(const skydiag::SharedLayout* shm)
{
  return shm->header.crash_seq;
}

void TestBreakpointReportedByCrashLoggerIsRecordedInsteadOfTheProbe(
  skydiag::SharedLayout* shm,
  const ModuleRange& testExe)
{
  BreakpointReportedByCrashLogger();

  const auto& crash = shm->header.crash;
  Require(crash.exception_code == kBreakpoint, "the reported breakpoint must be the recorded crash");
  Require(crash.faulting_tid == GetCurrentThreadId(), "the recorded crash must be on the breakpoint thread");
  Require(testExe.Contains(crash.exception_addr), "the recorded address must be the breakpoint, not the probe");
  Require(crash.context.Rip == crash.exception_addr, "the recorded context must be the breakpoint's context");

  const auto seq = CrashSeq(shm);
  g_probe();
  Require(CrashSeq(shm) == seq, "later CrashLogger probes must not replace the recorded crash");
}

// Field case (v0.2.59-rc4): CrashLogger 1.25 writes a thread dump on
// Ctrl+Shift+F12, then the same keys as the manual capture, and its probes there
// were recorded as crashes; each press wrote and discarded full crash dumps.
void TestCrashLoggerProbeWithNothingToReportIsIgnored(skydiag::SharedLayout* shm)
{
  RearmIncident(shm);

  // A breakpoint handled on another thread says nothing about this probe.
  std::thread other(HandledBreakpoint);
  other.join();

  const auto seq = CrashSeq(shm);
  g_probe();
  Require(CrashSeq(shm) == seq, "a CrashLogger probe with no kept exception on its thread must not be recorded");
  Require(
    (shm->header.state_flags & skydiag::kState_Frozen) == 0u,
    "an ignored CrashLogger probe must leave capture armed for a real crash");
}

void TestRealCrashAfterIgnoredProbeIsStillRecorded(skydiag::SharedLayout* shm, const ModuleRange& testExe)
{
  const auto seq = CrashSeq(shm);
  BreakpointReportedByCrashLogger();
  Require(CrashSeq(shm) != seq, "a crash after ignored probes must still be recorded");
  Require(shm->header.crash.exception_code == kBreakpoint, "the reported breakpoint must be recorded");
  Require(testExe.Contains(shm->header.crash.exception_addr), "the recorded address must be the breakpoint");
}

int FaultInGameCode()
{
  __try {
    *reinterpret_cast<volatile int*>(static_cast<ULONG_PTR>(0x20)) = 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return 0;
}

bool IsCrashEventSignaled()
{
  return WaitForSingleObject(skydiag::plugin::GetCrashEvent(), 0) == WAIT_OBJECT_0;
}

const skydiag::BlackboxEvent& LastEvent(const skydiag::SharedLayout* shm)
{
  return shm->events[(shm->header.write_index - 1u) % shm->header.capacity];
}

// The direct path for an ordinary fatal fault: the record, the Frozen claim,
// the kCrash event and the helper signal; a second fault leaves all of it.
void TestFatalFaultIsPublishedOnceAndSignalsTheHelper(skydiag::SharedLayout* shm, const ModuleRange& testExe)
{
  RearmIncident(shm);
  ResetEvent(skydiag::plugin::GetCrashEvent());
  const auto seq = CrashSeq(shm);

  FaultInGameCode();

  const auto& crash = shm->header.crash;
  Require(CrashSeq(shm) == seq + 2u, "a fatal fault must commit the next even crash sequence");
  Require(crash.exception_code == EXCEPTION_ACCESS_VIOLATION, "the fault must be recorded");
  Require(crash.faulting_tid == GetCurrentThreadId(), "the faulting thread must be recorded");
  Require(testExe.Contains(crash.exception_addr), "the faulting instruction must be recorded");
  Require(crash.context.Rip == crash.exception_addr, "the fault's context must be recorded");
  Require((shm->header.state_flags & skydiag::kState_Frozen) != 0u, "the fault must claim the incident");
  Require(IsCrashEventSignaled(), "the helper must be signaled");
  const auto& event = LastEvent(shm);
  Require(
    event.type == static_cast<std::uint16_t>(skydiag::EventType::kCrash) &&
      event.payload.a == EXCEPTION_ACCESS_VIOLATION && event.payload.b == crash.exception_addr,
    "the blackbox must end with the kCrash event");

  ResetEvent(skydiag::plugin::GetCrashEvent());
  const auto writeIndex = shm->header.write_index;
  const auto address = crash.exception_addr;
  FaultInGameCode();
  Require(CrashSeq(shm) == seq + 2u, "a second fault must not replace the incident");
  Require(crash.exception_addr == address, "a second fault must not rewrite the record");
  Require(shm->header.write_index == writeIndex, "a second fault must not add a kCrash event");
  Require(!IsCrashEventSignaled(), "a second fault must not signal the helper again");
}

// CrashLogger reports an unhandled C++ throw (a filter, like the dispatch it
// runs in) and, doing so, calls game code that faults outside its image.
int ReportThroughGameCodeFromFilter()
{
  g_callGameCode(FaultInGameCode);
  return EXCEPTION_EXECUTE_HANDLER;
}

void CppThrowReportedByCrashLoggerThatFaultsInGameCode()
{
  __try {
    RaiseException(kCppException, 0, 0, nullptr);
  } __except (ReportThroughGameCodeFromFilter()) {
  }
}

int FaultFromFilter()
{
  FaultInGameCode();
  return EXCEPTION_EXECUTE_HANDLER;
}

void CppThrowFollowedByFaultWithoutCrashLogger()
{
  __try {
    RaiseException(kCppException, 0, 0, nullptr);
  } __except (FaultFromFilter()) {
  }
}

void TestCrashLoggerSideFaultRecordsTheExceptionItReports(skydiag::SharedLayout* shm)
{
  RearmIncident(shm);
  const auto seq = CrashSeq(shm);
  CppThrowReportedByCrashLoggerThatFaultsInGameCode();
  Require(CrashSeq(shm) == seq + 2u, "the report's fault must record one crash");
  Require(
    shm->header.crash.exception_code == kCppException,
    "a fault in game code under CrashLogger's report must record the C++ throw it reports");
  Require(shm->header.crash.faulting_tid == GetCurrentThreadId(), "the throw's thread must be recorded");
}

// The same throw and fault without CrashLogger on the stack: the fault is the
// crash. A fresh thread, so no stale CrashLogger return address from the test
// above can be on its stack.
void TestFaultWithoutCrashLoggerFramesIsRecordedItself(skydiag::SharedLayout* shm)
{
  RearmIncident(shm);
  const auto seq = CrashSeq(shm);
  DWORD tid = 0;
  std::thread worker([&tid]() {
    tid = GetCurrentThreadId();
    CppThrowFollowedByFaultWithoutCrashLogger();
  });
  worker.join();
  Require(CrashSeq(shm) == seq + 2u, "the fault must be recorded");
  Require(
    shm->header.crash.exception_code == EXCEPTION_ACCESS_VIOLATION,
    "a fault with no CrashLogger frame above it must not be replaced by a kept C++ throw");
  Require(shm->header.crash.faulting_tid == tid, "the faulting thread must be recorded");
}

// Child processes: an unhandled exception ends the process, so these run in a
// copy of this test that the parent attaches to the way the helper does.
constexpr UINT kChildFilterExitCode = 41;
constexpr UINT kChildAbortExitCode = 42;
constexpr UINT kChildRetakingFilterExitCode = 43;

// Stand-ins for the handlers installed before the plugin's (the CRT's filter,
// another mod's SIGABRT handler). The child must still end through them.
LONG WINAPI ChildPreviousFilter(EXCEPTION_POINTERS*)
{
  TerminateProcess(GetCurrentProcess(), kChildFilterExitCode);
  return EXCEPTION_CONTINUE_SEARCH;
}

void __cdecl ChildPreviousAbortHandler(int)
{
  TerminateProcess(GetCurrentProcess(), kChildAbortExitCode);
}

// ADR-0032. A stand-in for CrashLogger: it registers its own top-level filter
// again while every exception is dispatched.
LONG WINAPI ChildRetakingFilter(EXCEPTION_POINTERS*)
{
  TerminateProcess(GetCurrentProcess(), kChildRetakingFilterExitCode);
  return EXCEPTION_CONTINUE_SEARCH;
}

LONG CALLBACK ChildRetakingVectoredHandler(EXCEPTION_POINTERS*)
{
  SetUnhandledExceptionFilter(ChildRetakingFilter);
  return EXCEPTION_CONTINUE_SEARCH;
}

__declspec(noinline) void WriteThroughNull()
{
  volatile int* nowhere = nullptr;
  *nowhere = 1;
}

__declspec(noinline) int CatchWriteThroughNull()
{
  __try {
    WriteThroughNull();
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 1;
  }
}

// Recursion the compiler cannot see the end of; the limit is never reached.
__declspec(noinline) std::uint64_t RecurseUntil(std::uint64_t depth, volatile std::uint64_t* limit)
{
  volatile char pad[512];
  pad[depth % sizeof(pad)] = static_cast<char>(depth);
  if (depth >= *limit) {
    return depth;
  }
  return RecurseUntil(depth + 1, limit) + static_cast<std::uint64_t>(pad[0]);
}

[[noreturn]] __declspec(noinline) void ThrowRuntimeError()
{
  throw std::runtime_error("unhandled in a SkyrimDiag crash handler test child");
}

// Leaving a noexcept function calls terminate() during the search for a
// handler, so the top-level filter never sees the throw.
__declspec(noinline) void CallThrowerFromNoexcept() noexcept
{
  ThrowRuntimeError();
}

int RunChild(const char* mode, const char* goEventName)
{
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  SetUnhandledExceptionFilter(ChildPreviousFilter);
  std::signal(SIGABRT, ChildPreviousAbortHandler);
  if (std::strcmp(mode, "unhandled-fault-short-hold") == 0) {
    skydiag::plugin::SetUnhandledCrashHoldMs(400);
  }
  if (std::strcmp(mode, "unhandled-fault-no-hold") == 0) {
    skydiag::plugin::SetUnhandledCrashHoldMs(0);
  }
  if (!skydiag::plugin::InitSharedMemory() || !skydiag::plugin::InstallCrashHandler(/*crashHookMode=*/1)) {
    return 90;
  }
  if (std::strncmp(mode, "unhandled-throw-filter-taken-once", 33) == 0) {
    // Something registers its own filter once after the plugin loaded, and
    // never again. "-reclaimed": the plugin's once-a-second reclaim then ran.
    SetUnhandledExceptionFilter(ChildRetakingFilter);
    if (std::strcmp(mode, "unhandled-throw-filter-taken-once-reclaimed") == 0) {
      const auto displaced = skydiag::plugin::ReclaimTopLevelFilter();
      if (displaced != reinterpret_cast<std::uintptr_t>(&ChildRetakingFilter) ||
          skydiag::plugin::ReclaimTopLevelFilter() != 0u) {
        return 95;
      }
    }
  }
  if (std::strcmp(mode, "unhandled-fault-filter-retaken") == 0 ||
      std::strcmp(mode, "unhandled-throw-filter-retaken") == 0) {
    // After the plugin's handlers, as CrashLogger's when it loads later.
    AddVectoredExceptionHandler(1, ChildRetakingVectoredHandler);
  }
  HANDLE go = OpenEventA(SYNCHRONIZE, FALSE, goEventName);
  if (!go || WaitForSingleObject(go, 30000) != WAIT_OBJECT_0) {
    return 91;
  }
  if (std::strncmp(mode, "unhandled-throw", 15) == 0) {
    ThrowRuntimeError();
  }
  if (std::strcmp(mode, "unhandled-breakpoint") == 0) {
    __debugbreak();
  }
  if (std::strcmp(mode, "noexcept-throw") == 0) {
    CallThrowerFromNoexcept();
  }
  if (std::strcmp(mode, "unhandled-fault-other-record") == 0) {
    // Another thread's handled fault takes the incident; the helper is still
    // checking it when this thread crashes (ADR-0017).
    std::thread other([] { CatchWriteThroughNull(); });
    other.join();
  }
  if (std::strncmp(mode, "unhandled-fault", 15) == 0) {
    WriteThroughNull();
  }
  if (std::strcmp(mode, "unhandled-stack-overflow") == 0) {
    volatile std::uint64_t limit = ~0ull;
    return static_cast<int>(RecurseUntil(0, &limit) & 1u) + 94;
  }
  if (std::strcmp(mode, "handled-fault") == 0) {
    // The record is published at first chance; the parent then sees whether
    // the handled fault was reported as unhandled.
    const int caught = CatchWriteThroughNull();
    Sleep(1500);
    return caught == 1 ? 0 : 93;
  }
  if (std::strcmp(mode, "abort-after-caught-throw") == 0) {
    try {
      ThrowRuntimeError();
    } catch (const std::runtime_error&) {
    }
    Sleep(1500);
    std::abort();
  }
  return 92;
}

struct ChildOutcome
{
  DWORD mainTid = 0;
  DWORD exitCode = 0;
  bool crashEventSignaled = false;
  std::uint32_t crashSeq = 0;
  std::uint32_t stateFlags = 0;
  skydiag::CrashInfo crash{};
  skydiag::BlackboxEvent lastEvent{};
};

ChildOutcome RunChildToCrash(const char* mode)
{
  char exePath[MAX_PATH]{};
  Require(GetModuleFileNameA(nullptr, exePath, MAX_PATH) != 0, "GetModuleFileNameA failed");
  const std::string goEventName = "Local\\skydiag_crash_handler_test_go_" + std::to_string(GetCurrentProcessId()) +
    "_" + std::to_string(GetTickCount64());
  HANDLE go = CreateEventA(nullptr, TRUE, FALSE, goEventName.c_str());
  Require(go != nullptr, "CreateEventA failed");

  std::string commandLine = std::string("\"") + exePath + "\" --child " + mode + " " + goEventName;
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  Require(
    CreateProcessA(exePath, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) !=
      FALSE,
    "CreateProcessA failed");

  // Attach the way the helper does once the plugin has created its objects.
  skydiag::helper::AttachedProcess proc{};
  bool attached = false;
  for (int attempt = 0; attempt < 500 && !attached; ++attempt) {
    attached = skydiag::helper::AttachByPid(pi.dwProcessId, proc, nullptr) && proc.crashEvent != nullptr;
    if (!attached) {
      if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
        break;
      }
    }
  }
  if (!attached) {
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(go);
    Require(false, "the helper could not attach to the child's shared memory and crash event");
  }

  ChildOutcome outcome{};
  outcome.mainTid = pi.dwThreadId;
  SetEvent(go);
  // The record is published before the child can end, so an exit first means
  // there is none.
  const HANDLE crashOrExit[] = { proc.crashEvent, pi.hProcess };
  WaitForMultipleObjects(2, crashOrExit, FALSE, 30000);
  outcome.crashEventSignaled = WaitForSingleObject(proc.crashEvent, 0) == WAIT_OBJECT_0;
  // ADR-0032: the plugin's filter holds the thread until the helper has
  // written the dump; this stand-in helper has nothing to write.
  if (proc.dumpSettledEvent) {
    SetEvent(proc.dumpSettledEvent);
  }
  if (WaitForSingleObject(pi.hProcess, 30000) != WAIT_OBJECT_0) {
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 5000);
  }
  GetExitCodeProcess(pi.hProcess, &outcome.exitCode);

  // The mapping outlives the child while the helper keeps its view.
  outcome.crashSeq = proc.shm->header.crash_seq;
  outcome.stateFlags = proc.shm->header.state_flags;
  outcome.crash = proc.shm->header.crash;
  outcome.lastEvent = LastEvent(proc.shm);

  skydiag::helper::Detach(proc);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  CloseHandle(go);
  return outcome;
}

void RequireChildCrashRecorded(const ChildOutcome& outcome, std::uint32_t code, DWORD exitCode, const char* what)
{
  const std::string prefix = std::string(what) + ": ";
  Require(outcome.crashEventSignaled, (prefix + "the helper must be signaled").c_str());
  Require(
    outcome.crashSeq != 0u && (outcome.crashSeq & 1u) == 0u,
    (prefix + "a committed crash record must be published").c_str());
  Require((outcome.stateFlags & skydiag::kState_Frozen) != 0u, (prefix + "the incident must be claimed").c_str());
  Require(outcome.crash.exception_code == code, (prefix + "the unhandled exception must be the recorded crash").c_str());
  Require(outcome.crash.faulting_tid == outcome.mainTid, (prefix + "the throwing thread must be recorded").c_str());
  Require(
    outcome.lastEvent.type == static_cast<std::uint16_t>(skydiag::EventType::kCrash) &&
      outcome.lastEvent.payload.a == code,
    (prefix + "the blackbox must end with the kCrash event").c_str());
  Require(
    outcome.exitCode == exitCode,
    (prefix + "the handler installed before the plugin's must still run (exit code " +
     std::to_string(outcome.exitCode) + ")").c_str());
}

// ADR-0032: what the helper sees when a fault in the child is not handled.
struct UnhandledOutcome
{
  bool attachedSignalEvents = false;
  bool unhandledSignaled = false;
  // Whether the child was still running some time after the signal.
  bool aliveWhileHeld = false;
  DWORD msFromSignalToExit = 0;
  DWORD exitCode = 0;
  std::uint32_t recordedCode = 0;
};

enum class HelperSettle
{
  // The helper sets "dump settled" after holdCheckMs.
  kAfterCheck,
  // The helper is there but never settles.
  kNever,
  // No helper event to wait for (the helper is gone).
  kNoEvent,
};

UnhandledOutcome RunChildToUnhandled(const char* mode, HelperSettle settle, DWORD holdCheckMs)
{
  char exePath[MAX_PATH]{};
  Require(GetModuleFileNameA(nullptr, exePath, MAX_PATH) != 0, "GetModuleFileNameA failed");
  const std::string goEventName = "Local\\skydiag_crash_handler_test_go_" + std::to_string(GetCurrentProcessId()) +
    "_" + std::to_string(GetTickCount64());
  HANDLE go = CreateEventA(nullptr, TRUE, FALSE, goEventName.c_str());
  Require(go != nullptr, "CreateEventA failed");

  std::string commandLine = std::string("\"") + exePath + "\" --child " + mode + " " + goEventName;
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  Require(
    CreateProcessA(exePath, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) !=
      FALSE,
    "CreateProcessA failed");

  skydiag::helper::AttachedProcess proc{};
  bool attached = false;
  for (int attempt = 0; attempt < 500 && !attached; ++attempt) {
    attached = skydiag::helper::AttachByPid(pi.dwProcessId, proc, nullptr) && proc.crashEvent != nullptr;
    if (!attached && WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
      break;
    }
  }
  if (!attached) {
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(go);
    Require(false, "the helper could not attach to the child's shared memory and crash event");
  }

  UnhandledOutcome outcome{};
  outcome.attachedSignalEvents = proc.unhandledEvent != nullptr && proc.dumpSettledEvent != nullptr;
  if (settle == HelperSettle::kNoEvent && proc.dumpSettledEvent) {
    // The only handle to it: the event is gone, as when the helper died.
    CloseHandle(proc.dumpSettledEvent);
    proc.dumpSettledEvent = nullptr;
  }
  SetEvent(go);
  if (proc.unhandledEvent) {
    const HANDLE unhandledOrExit[] = { proc.unhandledEvent, pi.hProcess };
    outcome.unhandledSignaled = WaitForMultipleObjects(2, unhandledOrExit, FALSE, 30000) == WAIT_OBJECT_0;
  }
  const ULONGLONG signalTick = GetTickCount64();
  if (outcome.unhandledSignaled && settle == HelperSettle::kAfterCheck) {
    outcome.aliveWhileHeld = WaitForSingleObject(pi.hProcess, holdCheckMs) == WAIT_TIMEOUT;
    SetEvent(proc.dumpSettledEvent);
  }
  if (WaitForSingleObject(pi.hProcess, 30000) != WAIT_OBJECT_0) {
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 5000);
  }
  outcome.msFromSignalToExit = static_cast<DWORD>(GetTickCount64() - signalTick);
  GetExitCodeProcess(pi.hProcess, &outcome.exitCode);
  outcome.recordedCode = proc.shm->header.crash.exception_code;

  skydiag::helper::Detach(proc);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  CloseHandle(go);
  return outcome;
}

// ADR-0032: a fault nothing handles reaches the plugin's top-level filter,
// which tells the helper and holds the thread until the helper has written
// the dump; then the filter under the plugin's still ends the process.
void TestUnhandledFaultIsSignaledAndHeldUntilTheDumpIsSettled()
{
  const auto outcome = RunChildToUnhandled("unhandled-fault", HelperSettle::kAfterCheck, 700);
  Require(outcome.attachedSignalEvents, "the helper must find the plugin's unhandled event and make its own");
  Require(outcome.unhandledSignaled, "an unhandled fault must be reported to the helper");
  Require(outcome.recordedCode == EXCEPTION_ACCESS_VIOLATION, "the fault must be the recorded crash");
  Require(outcome.aliveWhileHeld, "the crashing thread must be held until the helper settles the dump");
  Require(
    outcome.msFromSignalToExit < 4000u,
    "the hold must end when the dump is settled, not at its cap");
  Require(
    outcome.exitCode == kChildFilterExitCode,
    "the filter under the plugin's must still run after the hold");
}

// CrashLogger takes the top-level filter again on every exception. The
// plugin's last vectored handler takes it back, and the plugin's filter then
// calls CrashLogger's.
void TestUnhandledFaultIsHeldWhenAnotherHandlerRetakesTheFilter()
{
  const auto outcome = RunChildToUnhandled("unhandled-fault-filter-retaken", HelperSettle::kAfterCheck, 700);
  Require(outcome.unhandledSignaled, "the plugin's filter must be called although another handler retook the slot");
  Require(outcome.aliveWhileHeld, "the hold must work with a retaking handler");
  Require(
    outcome.exitCode == kChildRetakingFilterExitCode,
    "the retaking handler's filter is the one under the plugin's and must end the process");
}

// A C++ throw is not a fatal code: the plugin does not take the filter back
// for it, so with a handler like CrashLogger's the unhandled throw goes to
// that handler's filter as before ADR-0032 (CrashLogger then reports it, and
// its probe faults are what records the kept throw, ADR-0017).
void TestUnhandledThrowLeavesARetakenFilterAlone()
{
  const auto outcome = RunChildToUnhandled("unhandled-throw-filter-retaken", HelperSettle::kAfterCheck, 700);
  Require(!outcome.unhandledSignaled, "the plugin's filter must not be put back for a C++ throw");
  Require(
    outcome.exitCode == kChildRetakingFilterExitCode,
    "the retaking handler's filter must be the one that ends the process");
}

// Without CrashLogger only the plugin's filter records an unhandled C++
// throw. Something that takes the top-level filter once after the plugin
// loaded used to leave such a crash with no record at all (in game on
// 2026-11-13, three startup deaths). The plugin's reclaim, which it runs once
// a second, puts its filter back; the other filter is then the one under it.
void TestUnhandledThrowIsRecordedAfterTheFilterWasTakenOnceAndReclaimed()
{
  const auto lost = RunChildToCrash("unhandled-throw-filter-taken-once");
  Require(!lost.crashEventSignaled && lost.crashSeq == 0u, "without the reclaim the displaced filter records nothing");
  Require(lost.exitCode == kChildRetakingFilterExitCode, "the other filter ends the process");

  RequireChildCrashRecorded(
    RunChildToCrash("unhandled-throw-filter-taken-once-reclaimed"),
    kCppException,
    kChildRetakingFilterExitCode,
    "unhandled C++ throw after the filter was taken once and reclaimed");
}

// The incident is held by another thread's exception, which the helper is
// still checking. Saying "not handled" now would be said about that one, so
// the filter says nothing and does not hold.
void TestUnhandledFaultIsNotReportedWhenTheRecordIsAnotherExceptions()
{
  const auto outcome = RunChildToUnhandled("unhandled-fault-other-record", HelperSettle::kAfterCheck, 700);
  Require(outcome.recordedCode == EXCEPTION_ACCESS_VIOLATION, "the first, handled fault holds the record");
  Require(!outcome.unhandledSignaled, "another exception's record must not be reported as unhandled");
  Require(outcome.exitCode == kChildFilterExitCode, "the filter under the plugin's must still end the process");
}

// A stack overflow leaves little stack: it is recorded at first chance as
// before, but the filter neither reports nor holds it, and the filter under
// the plugin's still ends the process.
void TestUnhandledStackOverflowIsNotHeld()
{
  const auto outcome = RunChildToUnhandled("unhandled-stack-overflow", HelperSettle::kNever, 0);
  Require(outcome.recordedCode == EXCEPTION_STACK_OVERFLOW, "the stack overflow is recorded at first chance");
  Require(!outcome.unhandledSignaled, "a stack overflow must not be reported from the top-level filter");
  Require(outcome.exitCode == kChildFilterExitCode, "the filter under the plugin's must still end the process");
}

// A helper that never answers: the hold ends at its cap.
void TestUnhandledFaultHoldEndsAtItsCap()
{
  const auto outcome = RunChildToUnhandled("unhandled-fault-short-hold", HelperSettle::kNever, 0);
  Require(outcome.unhandledSignaled, "the fault must be reported");
  Require(outcome.msFromSignalToExit >= 300u, "the thread must be held for about the cap (400 ms)");
  Require(outcome.msFromSignalToExit < 4000u, "the cap must end the hold");
  Require(outcome.exitCode == kChildFilterExitCode, "the filter under the plugin's must run after the cap");
}

// No helper event (the helper is gone), or a hold of 0: the helper is told,
// and the crash goes on at once.
void TestUnhandledFaultIsNotHeldWithoutAHelperOrWithHoldOff()
{
  const auto noHelper = RunChildToUnhandled("unhandled-fault", HelperSettle::kNoEvent, 0);
  Require(noHelper.unhandledSignaled, "without the helper's event the fault is still reported");
  Require(noHelper.msFromSignalToExit < 2000u, "without the helper's event nothing is waited for");
  Require(noHelper.exitCode == kChildFilterExitCode, "the filter under the plugin's must run");

  const auto holdOff = RunChildToUnhandled("unhandled-fault-no-hold", HelperSettle::kNever, 0);
  Require(holdOff.unhandledSignaled, "with the hold off the fault is still reported");
  Require(holdOff.msFromSignalToExit < 2000u, "with the hold off nothing is waited for");
  Require(holdOff.exitCode == kChildFilterExitCode, "the filter under the plugin's must run");
}

// A fault the game handles never reaches the top-level filter.
void TestHandledFaultIsNotReportedAsUnhandled()
{
  const auto outcome = RunChildToUnhandled("handled-fault", HelperSettle::kNever, 0);
  Require(outcome.recordedCode == EXCEPTION_ACCESS_VIOLATION, "the handled fault is recorded at first chance");
  Require(!outcome.unhandledSignaled, "a handled fault must not be reported as unhandled");
  Require(outcome.exitCode == 0u, "the child must have caught the fault and ended normally");
}

// Without CrashLogger, nothing faulted on the throwing thread, so an unhandled
// C++ throw or breakpoint ended the game with no crash record (review M2).
void TestUnhandledExceptionsWithoutCrashLoggerAreRecorded()
{
  RequireChildCrashRecorded(
    RunChildToCrash("unhandled-throw"), kCppException, kChildFilterExitCode, "unhandled C++ throw");
  RequireChildCrashRecorded(
    RunChildToCrash("unhandled-breakpoint"), kBreakpoint, kChildFilterExitCode, "unhandled breakpoint");
  RequireChildCrashRecorded(
    RunChildToCrash("noexcept-throw"), kCppException, kChildAbortExitCode, "throw out of a noexcept function");
}

// abort() well after a throw the thread caught is not that throw's
// terminate(): the throw was handled, so it must not be published as the crash.
void TestAbortAfterACaughtThrowIsNotBlamedOnIt()
{
  const auto outcome = RunChildToCrash("abort-after-caught-throw");
  Require(!outcome.crashEventSignaled, "an abort after a caught throw must not signal a crash");
  Require(outcome.crashSeq == 0u, "an abort after a caught throw must not publish a crash record");
  Require(outcome.exitCode == kChildAbortExitCode, "the SIGABRT handler installed before the plugin's must still run");
}

}  // namespace

int main(int argc, char** argv)
{
  if (argc == 4 && std::strcmp(argv[1], "--child") == 0) {
    return RunChild(argv[2], argv[3]);
  }
  try {
    Require(argc == 2, "usage: plugin_crash_handler_runtime_tests <path to fake CrashLogger.dll>");
    const HMODULE crashLogger = LoadLibraryA(argv[1]);
    Require(crashLogger != nullptr, "failed to load the fake CrashLogger.dll");
    g_probe = reinterpret_cast<ProbeFn>(GetProcAddress(crashLogger, "SkydiagFakeCrashLoggerProbe"));
    Require(g_probe != nullptr, "fake CrashLogger.dll has no probe export");
    g_callGameCode =
      reinterpret_cast<CallGameCodeFn>(GetProcAddress(crashLogger, "SkydiagFakeCrashLoggerCallGameCode"));
    Require(g_callGameCode != nullptr, "fake CrashLogger.dll has no game-code call export");

    Require(skydiag::plugin::InitSharedMemory(), "InitSharedMemory failed");
    Require(skydiag::plugin::InstallCrashHandler(/*crashHookMode=*/1), "InstallCrashHandler failed");
    skydiag::plugin::RefreshCrashLoggerModuleRanges();

    auto* shm = skydiag::plugin::GetShared();
    const auto testExe = RangeOf(GetModuleHandleW(nullptr));
    TestBreakpointReportedByCrashLoggerIsRecordedInsteadOfTheProbe(shm, testExe);
    TestCrashLoggerProbeWithNothingToReportIsIgnored(shm);
    TestRealCrashAfterIgnoredProbeIsStillRecorded(shm, testExe);
    TestFatalFaultIsPublishedOnceAndSignalsTheHelper(shm, testExe);
    TestCrashLoggerSideFaultRecordsTheExceptionItReports(shm);
    TestFaultWithoutCrashLoggerFramesIsRecordedItself(shm);
    TestUnhandledExceptionsWithoutCrashLoggerAreRecorded();
    TestAbortAfterACaughtThrowIsNotBlamedOnIt();
    TestUnhandledFaultIsSignaledAndHeldUntilTheDumpIsSettled();
    TestUnhandledFaultIsHeldWhenAnotherHandlerRetakesTheFilter();
    TestUnhandledThrowLeavesARetakenFilterAlone();
    TestUnhandledThrowIsRecordedAfterTheFilterWasTakenOnceAndReclaimed();
    TestUnhandledFaultIsNotReportedWhenTheRecordIsAnotherExceptions();
    TestUnhandledStackOverflowIsNotHeld();
    TestUnhandledFaultHoldEndsAtItsCap();
    TestUnhandledFaultIsNotHeldWithoutAHelperOrWithHoldOff();
    TestHandledFaultIsNotReportedAsUnhandled();
    std::puts("plugin crash handler runtime tests passed");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
