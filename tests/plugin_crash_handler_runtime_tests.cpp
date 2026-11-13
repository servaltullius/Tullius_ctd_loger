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

// A filter that keeps the one it replaced and calls it when it is done, as a
// well-behaved filter does. Registered after the plugin's, that one is the
// plugin's.
LPTOP_LEVEL_EXCEPTION_FILTER g_childChainingFilterPrevious = nullptr;

LONG WINAPI ChildChainingFilter(EXCEPTION_POINTERS* ep)
{
  return g_childChainingFilterPrevious ? g_childChainingFilterPrevious(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// A stand-in for a crash handler that probes memory while an exception is
// dispatched: a fault of its own, caught, inside the dispatch of the first.
bool g_childProbing = false;
int CatchWriteThroughNull();

LONG CALLBACK ChildProbingVectoredHandler(EXCEPTION_POINTERS* ep)
{
  if (!g_childProbing && ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
    g_childProbing = true;
    CatchWriteThroughNull();
    g_childProbing = false;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

// A filter that makes the thread run on (a continuable exception).
LONG WINAPI ChildResumingFilter(EXCEPTION_POINTERS*)
{
  return EXCEPTION_CONTINUE_EXECUTION;
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

// The bytes the last exception measured here took on its thread's stack for
// its CONTEXT: Windows puts that right under the stack pointer the thread had.
DWORD g_exceptionContextBytes = 0;

int NoteExceptionContextBytes(const EXCEPTION_POINTERS* ep)
{
  g_exceptionContextBytes =
    static_cast<DWORD>(ep->ContextRecord->Rsp - reinterpret_cast<std::uintptr_t>(ep->ContextRecord));
  return EXCEPTION_EXECUTE_HANDLER;
}

// Takes a fault of its own to see it, so only before the plugin's handlers
// are installed: they would record it.
__declspec(noinline) DWORD MeasureExceptionContextBytes()
{
  __try {
    WriteThroughNull();
  } __except (NoteExceptionContextBytes(GetExceptionInformation())) {
  }
  return g_exceptionContextBytes;
}

// Windows puts the exception's CONTEXT, with the processor's extended state,
// on the stack that overflowed, and keeps three pages of every stack back for
// that (11,744 to 12,288 bytes were left where it was measured). On those
// hosts a context takes 1,792 to 3,440 bytes and the handlers and filters here
// are done in the rest. Where Windows saves a large extended state (Intel AMX
// on Windows Server 2022: 11,696 bytes for every thread) the context does not
// fit: Windows ends the process and calls no handler, the plugin's included,
// so there is nothing of the plugin's to test (ADR-0032, Decision 5). There,
// and only there, the child keeps back what an ordinary host has left after
// its context; elsewhere it overflows with the default, as the game does.
constexpr DWORD kDefaultStackOverflowReserveBytes = 3u * 4096u;
constexpr DWORD kStackOverflowHandlerRoomBytes = 2u * 4096u;
// Larger than any context: a measurement beyond it is not one.
constexpr DWORD kExceptionContextBytesMax = 256u * 1024u;

// The stack to have Windows keep back for a context of that size; 0 leaves
// the default.
constexpr DWORD StackGuaranteeForExceptionContext(DWORD contextBytes) noexcept
{
  return contextBytes + kStackOverflowHandlerRoomBytes <= kDefaultStackOverflowReserveBytes
    ? 0u
    : contextBytes + kStackOverflowHandlerRoomBytes;
}

static_assert(StackGuaranteeForExceptionContext(1792u) == 0u);
static_assert(StackGuaranteeForExceptionContext(3440u) == 0u);
static_assert(StackGuaranteeForExceptionContext(4096u) == 0u);
static_assert(StackGuaranteeForExceptionContext(4097u) == 4097u + kStackOverflowHandlerRoomBytes);
static_assert(StackGuaranteeForExceptionContext(11696u) == 19888u);

// `always`: also where the default would do, so that this path runs on every
// host and not only on the few with a large context.
bool KeepStackForExceptionContext(DWORD contextBytes, bool always)
{
  if (contextBytes == 0u || contextBytes > kExceptionContextBytesMax) {
    return false;
  }
  ULONG guarantee = StackGuaranteeForExceptionContext(contextBytes);
  if (guarantee == 0u) {
    if (!always) {
      return true;
    }
    guarantee = contextBytes + kStackOverflowHandlerRoomBytes;
  }
  const ULONG asked = guarantee;
  if (!SetThreadStackGuarantee(&guarantee)) {
    return false;
  }
  // Asked with 0, it changes nothing and says what the thread has.
  ULONG kept = 0;
  return SetThreadStackGuarantee(&kept) != FALSE && kept >= asked;
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
  SetUnhandledExceptionFilter(std::strcmp(mode, "resumed-fault") == 0 ? ChildResumingFilter : ChildPreviousFilter);
  std::signal(SIGABRT, ChildPreviousAbortHandler);
  if (std::strcmp(mode, "unhandled-fault-short-hold") == 0) {
    skydiag::plugin::SetUnhandledCrashHoldMs(400);
  }
  if (std::strcmp(mode, "unhandled-fault-no-hold") == 0) {
    skydiag::plugin::SetUnhandledCrashHoldMs(0);
  }
  const bool stackOverflowKeptBack = std::strcmp(mode, "unhandled-stack-overflow-kept-back") == 0;
  const bool stackOverflow = stackOverflowKeptBack || std::strcmp(mode, "unhandled-stack-overflow") == 0;
  if (stackOverflow && !KeepStackForExceptionContext(MeasureExceptionContextBytes(), stackOverflowKeptBack)) {
    return 97;
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
  if (std::strcmp(mode, "unhandled-throw-chaining-filter-reclaimed") == 0) {
    // Something registers a filter that chains to the one it replaced (the
    // plugin's), and the plugin's reclaim then puts the plugin's back on top.
    g_childChainingFilterPrevious = SetUnhandledExceptionFilter(ChildChainingFilter);
    if (skydiag::plugin::ReclaimTopLevelFilter() != reinterpret_cast<std::uintptr_t>(&ChildChainingFilter)) {
      return 96;
    }
  }
  if (std::strcmp(mode, "unhandled-fault-with-nested-probe") == 0) {
    AddVectoredExceptionHandler(0, ChildProbingVectoredHandler);
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
  if (std::strcmp(mode, "unhandled-fault-repeat-of-held-record") == 0) {
    // This thread's own fault, handled, takes the incident; the helper is
    // still checking it when the same fault comes again and is not handled.
    // Same thread, code and address as the record, but not its exception.
    CatchWriteThroughNull();
  }
  if (std::strcmp(mode, "resumed-fault") == 0) {
    // Nothing handles it, the top-level filter is reached, and the filter
    // under the plugin's resumes the thread.
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    Sleep(300);
    return 0;
  }
  if (std::strncmp(mode, "unhandled-fault", 15) == 0) {
    WriteThroughNull();
  }
  if (stackOverflow) {
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

// What the child left behind, for a failure message: a failure that shows on
// one CI runner in several cannot be run again to look at.
std::string Describe(const ChildOutcome& outcome)
{
  char text[256]{};
  std::snprintf(
    text,
    sizeof(text),
    " (child: crash event signaled %d, recorded code 0x%08lX on thread %lu, main thread %lu, crash_seq %lu, "
    "state flags 0x%lX, exit code 0x%08lX; exception context %lu bytes on this host)",
    outcome.crashEventSignaled ? 1 : 0,
    static_cast<unsigned long>(outcome.crash.exception_code),
    static_cast<unsigned long>(outcome.crash.faulting_tid),
    static_cast<unsigned long>(outcome.mainTid),
    static_cast<unsigned long>(outcome.crashSeq),
    static_cast<unsigned long>(outcome.stateFlags),
    static_cast<unsigned long>(outcome.exitCode),
    static_cast<unsigned long>(g_exceptionContextBytes));
  return text;
}

void RequireOf(const ChildOutcome& outcome, bool condition, const char* message)
{
  if (!condition) {
    throw std::runtime_error(message + Describe(outcome));
  }
}

// The child made no shared memory and crash event to attach to. Ends it, and
// says with which exit code it had ended by itself if it had (RunChild).
[[noreturn]] void FailChildNotAttached(const PROCESS_INFORMATION& pi, HANDLE go)
{
  DWORD exitCode = 0;
  const bool ended =
    WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &exitCode) != FALSE;
  TerminateProcess(pi.hProcess, 1);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  CloseHandle(go);
  const std::string state =
    ended ? "the child had ended with exit code " + std::to_string(exitCode) : "the child was still running";
  throw std::runtime_error(
    "the helper could not attach to the child's shared memory and crash event (" + state +
    "; exception context " + std::to_string(g_exceptionContextBytes) + " bytes on this host)");
}

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
    FailChildNotAttached(pi, go);
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
  RequireOf(outcome, outcome.crashEventSignaled, (prefix + "the helper must be signaled").c_str());
  RequireOf(
    outcome,
    outcome.crashSeq != 0u && (outcome.crashSeq & 1u) == 0u,
    (prefix + "a committed crash record must be published").c_str());
  Require((outcome.stateFlags & skydiag::kState_Frozen) != 0u, (prefix + "the incident must be claimed").c_str());
  RequireOf(
    outcome,
    outcome.crash.exception_code == code,
    (prefix + "the unhandled exception must be the recorded crash").c_str());
  RequireOf(
    outcome,
    outcome.crash.faulting_tid == outcome.mainTid,
    (prefix + "the throwing thread must be recorded").c_str());
  RequireOf(
    outcome,
    outcome.lastEvent.type == static_cast<std::uint16_t>(skydiag::EventType::kCrash) &&
      outcome.lastEvent.payload.a == code,
    (prefix + "the blackbox must end with the kCrash event").c_str());
  RequireOf(
    outcome,
    outcome.exitCode == exitCode,
    (prefix + "the handler installed before the plugin's must still run").c_str());
}

// ADR-0032: what the helper sees when a fault in the child is not handled.
struct UnhandledOutcome
{
  bool attachedSignalEvents = false;
  bool unhandledSignaled = false;
  // Whether the signal was still set once the child had ended.
  bool unhandledSignaledAtExit = false;
  // Whether the child was still running some time after the signal.
  bool aliveWhileHeld = false;
  DWORD msFromSignalToExit = 0;
  DWORD exitCode = 0;
  std::uint32_t recordedCode = 0;
  std::uint32_t crashSeq = 0;
  std::uint32_t stateFlags = 0;
};

std::string Describe(const UnhandledOutcome& outcome)
{
  char text[320]{};
  std::snprintf(
    text,
    sizeof(text),
    " (child: recorded code 0x%08lX, crash_seq %lu, state flags 0x%lX, exit code 0x%08lX, reported as unhandled %d, "
    "still reported at the exit %d, alive while held %d, %lu ms from the report to the exit; exception context "
    "%lu bytes on this host)",
    static_cast<unsigned long>(outcome.recordedCode),
    static_cast<unsigned long>(outcome.crashSeq),
    static_cast<unsigned long>(outcome.stateFlags),
    static_cast<unsigned long>(outcome.exitCode),
    outcome.unhandledSignaled ? 1 : 0,
    outcome.unhandledSignaledAtExit ? 1 : 0,
    outcome.aliveWhileHeld ? 1 : 0,
    static_cast<unsigned long>(outcome.msFromSignalToExit),
    static_cast<unsigned long>(g_exceptionContextBytes));
  return text;
}

void RequireOf(const UnhandledOutcome& outcome, bool condition, const char* message)
{
  if (!condition) {
    throw std::runtime_error(message + Describe(outcome));
  }
}

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
    FailChildNotAttached(pi, go);
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
  outcome.unhandledSignaledAtExit =
    proc.unhandledEvent && WaitForSingleObject(proc.unhandledEvent, 0) == WAIT_OBJECT_0;
  GetExitCodeProcess(pi.hProcess, &outcome.exitCode);
  outcome.recordedCode = proc.shm->header.crash.exception_code;
  outcome.crashSeq = proc.shm->header.crash_seq;
  outcome.stateFlags = proc.shm->header.state_flags;

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
  RequireOf(
    outcome,
    outcome.attachedSignalEvents,
    "the helper must find the plugin's unhandled event and make its own");
  RequireOf(outcome, outcome.unhandledSignaled, "an unhandled fault must be reported to the helper");
  RequireOf(outcome, outcome.recordedCode == EXCEPTION_ACCESS_VIOLATION, "the fault must be the recorded crash");
  RequireOf(outcome, outcome.aliveWhileHeld, "the crashing thread must be held until the helper settles the dump");
  RequireOf(
    outcome,
    outcome.msFromSignalToExit < 4000u,
    "the hold must end when the dump is settled, not at its cap");
  RequireOf(
    outcome,
    outcome.exitCode == kChildFilterExitCode,
    "the filter under the plugin's must still run after the hold");
}

// CrashLogger takes the top-level filter again on every exception. The
// plugin's last vectored handler takes it back, and the plugin's filter then
// calls CrashLogger's.
void TestUnhandledFaultIsHeldWhenAnotherHandlerRetakesTheFilter()
{
  const auto outcome = RunChildToUnhandled("unhandled-fault-filter-retaken", HelperSettle::kAfterCheck, 700);
  RequireOf(
    outcome,
    outcome.unhandledSignaled,
    "the plugin's filter must be called although another handler retook the slot");
  RequireOf(outcome, outcome.aliveWhileHeld, "the hold must work with a retaking handler");
  RequireOf(
    outcome,
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
  RequireOf(outcome, !outcome.unhandledSignaled, "the plugin's filter must not be put back for a C++ throw");
  RequireOf(
    outcome,
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
  RequireOf(
    lost,
    !lost.crashEventSignaled && lost.crashSeq == 0u,
    "without the reclaim the displaced filter records nothing");
  RequireOf(lost, lost.exitCode == kChildRetakingFilterExitCode, "the other filter ends the process");

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
  RequireOf(outcome, outcome.recordedCode == EXCEPTION_ACCESS_VIOLATION, "the first, handled fault holds the record");
  RequireOf(outcome, !outcome.unhandledSignaled, "another exception's record must not be reported as unhandled");
  RequireOf(
    outcome,
    outcome.exitCode == kChildFilterExitCode,
    "the filter under the plugin's must still end the process");
}

// The incident is held by this thread's own earlier fault at the same
// address, which the game handled and the helper is still checking. The fault
// that comes again and is not handled could not be recorded, so the record,
// though it names the same thread, code and address, is not its record.
void TestUnhandledFaultIsNotReportedWhenTheRecordIsAnEarlierOccurrence()
{
  const auto outcome = RunChildToUnhandled("unhandled-fault-repeat-of-held-record", HelperSettle::kAfterCheck, 700);
  RequireOf(outcome, outcome.recordedCode == EXCEPTION_ACCESS_VIOLATION, "the first, handled fault holds the record");
  RequireOf(
    outcome,
    !outcome.unhandledSignaled,
    "an earlier occurrence's record must not be reported as this unhandled exception's");
  RequireOf(
    outcome,
    outcome.exitCode == kChildFilterExitCode,
    "the filter under the plugin's must still end the process");
}

// A handler faults and catches its own fault while the crash is dispatched,
// as a crash handler that probes memory does. That inner fault cannot be
// recorded (the crash holds the incident), but it is another exception: the
// crash itself was recorded and must still be reported and held.
void TestUnhandledFaultIsReportedDespiteAFaultDispatchedInsideIt()
{
  const auto outcome = RunChildToUnhandled("unhandled-fault-with-nested-probe", HelperSettle::kAfterCheck, 700);
  RequireOf(outcome, outcome.unhandledSignaled, "a fault dispatched inside the crash must not hide the crash");
  RequireOf(outcome, outcome.aliveWhileHeld, "the crashing thread must still be held for the dump");
  RequireOf(
    outcome,
    outcome.exitCode == kChildFilterExitCode,
    "the filter under the plugin's must still end the process");
}

// A filter registered after the plugin's keeps the plugin's as its previous
// filter and calls it. Once the plugin has taken the slot back, that filter is
// the one under the plugin's, and its call comes back into the plugin's
// filter: the chain must go on to the filter that was there when the plugin
// loaded, as it did before the slot was taken back, and not end there.
void TestFilterPresentAtInstallStillRunsUnderAChainingFilter()
{
  RequireChildCrashRecorded(
    RunChildToCrash("unhandled-throw-chaining-filter-reclaimed"),
    kCppException,
    kChildFilterExitCode,
    "unhandled C++ throw under a chaining filter after the reclaim");
}

// A filter under the plugin's resumes the thread: the helper was told while
// the exception was unhandled, and the plugin takes that back when the thread
// runs on, so the helper does not go on reading a crash in progress.
void TestUnhandledSignalIsTakenBackWhenAFilterResumesTheThread()
{
  const auto outcome = RunChildToUnhandled("resumed-fault", HelperSettle::kAfterCheck, 300);
  RequireOf(outcome, outcome.unhandledSignaled, "the exception reached the top-level filter and must be reported");
  RequireOf(outcome, outcome.aliveWhileHeld, "the thread is held for the dump as for any unhandled exception");
  RequireOf(outcome, outcome.exitCode == 0u, "the child must have run on and ended normally");
  RequireOf(outcome, !outcome.unhandledSignaledAtExit, "the signal must be taken back once the thread runs on");
}

// A stack overflow leaves little stack: it is recorded at first chance as
// before, but the filter neither reports nor holds it, and the filter under
// the plugin's still ends the process. "-kept-back": the same with stack kept
// back on every host, the path a host with a large exception context takes
// (KeepStackForExceptionContext).
void TestUnhandledStackOverflowIsNotHeld()
{
  for (const char* mode : { "unhandled-stack-overflow", "unhandled-stack-overflow-kept-back" }) {
    const auto outcome = RunChildToUnhandled(mode, HelperSettle::kNever, 0);
    const std::string prefix = std::string(mode) + ": ";
    RequireOf(
      outcome,
      outcome.recordedCode == EXCEPTION_STACK_OVERFLOW,
      (prefix + "the stack overflow is recorded at first chance").c_str());
    RequireOf(
      outcome,
      !outcome.unhandledSignaled,
      (prefix + "a stack overflow must not be reported from the top-level filter").c_str());
    RequireOf(
      outcome,
      outcome.exitCode == kChildFilterExitCode,
      (prefix + "the filter under the plugin's must still end the process").c_str());
  }
}

// A helper that never answers: the hold ends at its cap.
void TestUnhandledFaultHoldEndsAtItsCap()
{
  const auto outcome = RunChildToUnhandled("unhandled-fault-short-hold", HelperSettle::kNever, 0);
  RequireOf(outcome, outcome.unhandledSignaled, "the fault must be reported");
  RequireOf(outcome, outcome.msFromSignalToExit >= 300u, "the thread must be held for about the cap (400 ms)");
  RequireOf(outcome, outcome.msFromSignalToExit < 4000u, "the cap must end the hold");
  RequireOf(outcome, outcome.exitCode == kChildFilterExitCode, "the filter under the plugin's must run after the cap");
}

// No helper event (the helper is gone), or a hold of 0: the helper is told,
// and the crash goes on at once.
void TestUnhandledFaultIsNotHeldWithoutAHelperOrWithHoldOff()
{
  const auto noHelper = RunChildToUnhandled("unhandled-fault", HelperSettle::kNoEvent, 0);
  RequireOf(noHelper, noHelper.unhandledSignaled, "without the helper's event the fault is still reported");
  RequireOf(noHelper, noHelper.msFromSignalToExit < 2000u, "without the helper's event nothing is waited for");
  RequireOf(noHelper, noHelper.exitCode == kChildFilterExitCode, "the filter under the plugin's must run");

  const auto holdOff = RunChildToUnhandled("unhandled-fault-no-hold", HelperSettle::kNever, 0);
  RequireOf(holdOff, holdOff.unhandledSignaled, "with the hold off the fault is still reported");
  RequireOf(holdOff, holdOff.msFromSignalToExit < 2000u, "with the hold off nothing is waited for");
  RequireOf(holdOff, holdOff.exitCode == kChildFilterExitCode, "the filter under the plugin's must run");
}

// A fault the game handles never reaches the top-level filter.
void TestHandledFaultIsNotReportedAsUnhandled()
{
  const auto outcome = RunChildToUnhandled("handled-fault", HelperSettle::kNever, 0);
  RequireOf(
    outcome,
    outcome.recordedCode == EXCEPTION_ACCESS_VIOLATION,
    "the handled fault is recorded at first chance");
  RequireOf(outcome, !outcome.unhandledSignaled, "a handled fault must not be reported as unhandled");
  RequireOf(outcome, outcome.exitCode == 0u, "the child must have caught the fault and ended normally");
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
  RequireOf(outcome, !outcome.crashEventSignaled, "an abort after a caught throw must not signal a crash");
  RequireOf(outcome, outcome.crashSeq == 0u, "an abort after a caught throw must not publish a crash record");
  RequireOf(
    outcome,
    outcome.exitCode == kChildAbortExitCode,
    "the SIGABRT handler installed before the plugin's must still run");
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

    // Before the plugin's handlers are installed (see the function).
    Require(MeasureExceptionContextBytes() != 0u, "could not measure an exception context");
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
    TestUnhandledFaultIsNotReportedWhenTheRecordIsAnEarlierOccurrence();
    TestUnhandledFaultIsReportedDespiteAFaultDispatchedInsideIt();
    TestFilterPresentAtInstallStillRunsUnderAChainingFilter();
    TestUnhandledSignalIsTakenBackWhenAFilterResumesTheThread();
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
