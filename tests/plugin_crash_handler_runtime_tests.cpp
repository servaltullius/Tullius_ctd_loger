// Runs the plugin's real vectored handler against a stand-in CrashLogger.dll.
//
// Field case (v0.2.59-rc2): a mod's assert raised a breakpoint, which the
// fatal-only filter does not record. CrashLogger reported it and faulted in its
// own memory probe, and that probe was recorded as the crash, so the dump and
// report pointed at CrashLogger.dll instead of the mod.

#include <Windows.h>
#include <Psapi.h>
#include <intrin.h>
#include <malloc.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "SkyrimDiag/CrashHandler.h"
#include "SkyrimDiag/SharedMemory.h"
#include "SkyrimDiagHelper/ProcessAttach.h"
#include "SkyrimDiagProtocol.h"
#include "SkyrimDiagSecondFault.h"
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

// ADR-0037: shared observation page between parent and stack-overflow guard child.
struct StackOverflowChildObs
{
  volatile LONG otherHandlerCalls = 0;
  volatile LONG guardStackMeasured = 0;
  volatile LONG skippedMoveResult = -1;
  volatile LONG forceMoveResult = -1;
  volatile LONG workerDone = 0;
  volatile LONG nonSoHandlerSawUntouchedShm = 0;
  std::uint64_t stackLimit = 0;
  std::uint64_t handlerEntryRsp = 0;
  std::uint64_t lowestNonZero = 0;
  std::uint64_t guardStackUsedBytes = 0;
};

StackOverflowChildObs* g_childObs = nullptr;
HANDLE g_childObsMapping = nullptr;

void OpenChildObsIfPresent(const char* goEventName) noexcept
{
  const std::string obsName = std::string(goEventName) + "_obs";
  g_childObsMapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, obsName.c_str());
  if (g_childObsMapping) {
    g_childObs = static_cast<StackOverflowChildObs*>(
      MapViewOfFile(g_childObsMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(StackOverflowChildObs)));
  }
}

__declspec(noinline) std::uint8_t ExhaustRemainingStackInFrontVeh(int remainingFrames = 48) noexcept
{
  // Descend in 2 KB frames (< 4 KB so RSP itself advances down through every
  // page instead of __chkstk faulting while RSP is still high). 48 * 2 KB =
  // 96 KB (> 64 KB), exceeding even AMX KeepStackForExceptionContext room.
  volatile std::uint8_t chunk[2048];
  chunk[0] = static_cast<std::uint8_t>(remainingFrames);
  chunk[sizeof(chunk) - 1u] = static_cast<std::uint8_t>(remainingFrames ^ 0x5Au);
  if (remainingFrames <= 0) {
    return chunk[0] ^ chunk[sizeof(chunk) - 1u];
  }
  return static_cast<std::uint8_t>(ExhaustRemainingStackInFrontVeh(remainingFrames - 1) ^ chunk[0]);
}

__declspec(noinline) void MeasureStackUsedBeforeCurrentVeh(std::uintptr_t entryRsp) noexcept
{
  if (!g_childObs) {
    return;
  }
  const auto* const tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
  const auto stackLimit = reinterpret_cast<std::uintptr_t>(tib->StackLimit);
  std::uintptr_t lowestNonZero = entryRsp;
  if (entryRsp > stackLimit) {
    const auto* bytes = reinterpret_cast<const volatile std::uint8_t*>(stackLimit);
    const std::size_t span = static_cast<std::size_t>(entryRsp - stackLimit);
    for (std::size_t i = 0; i < span; ++i) {
      if (bytes[i] != 0u) {
        lowestNonZero = stackLimit + i;
        break;
      }
    }
  }
  g_childObs->stackLimit = stackLimit;
  g_childObs->handlerEntryRsp = entryRsp;
  g_childObs->lowestNonZero = lowestNonZero;
  g_childObs->guardStackUsedBytes = entryRsp > lowestNonZero ? (entryRsp - lowestNonZero) : 0u;
  InterlockedExchange(&g_childObs->guardStackMeasured, 1);
}

__declspec(noinline) LONG CALLBACK ChildFrontHeavyStackOverflowVeh(EXCEPTION_POINTERS* ep) noexcept
{
  if (!ep || !ep->ExceptionRecord || ep->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  MeasureStackUsedBeforeCurrentVeh(reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress()));
  if (g_childObs) {
    InterlockedIncrement(&g_childObs->otherHandlerCalls);
  }
  (void)ExhaustRemainingStackInFrontVeh();
  return EXCEPTION_CONTINUE_SEARCH;
}

__declspec(noinline) LONG CALLBACK ChildStackOverflowMarkerVeh(EXCEPTION_POINTERS* ep) noexcept
{
  if (!ep || !ep->ExceptionRecord || ep->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  if (g_childObs) {
    InterlockedIncrement(&g_childObs->otherHandlerCalls);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

std::uint32_t g_childExpectedWriteIndex = 0;

LONG CALLBACK ChildNonStackOverflowConsumingVeh(EXCEPTION_POINTERS* ep) noexcept
{
  if (!ep || !ep->ExceptionRecord || ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  auto* shm = skydiag::plugin::GetShared();
  const bool untouched = shm &&
    shm->header.crash_seq == 0u &&
    shm->header.write_index == g_childExpectedWriteIndex &&
    (shm->header.state_flags & skydiag::kState_Frozen) == 0u &&
    WaitForSingleObject(skydiag::plugin::GetCrashEvent(), 0) == WAIT_TIMEOUT;
  if (g_childObs) {
    InterlockedIncrement(&g_childObs->otherHandlerCalls);
    if (untouched) {
      InterlockedExchange(&g_childObs->nonSoHandlerSawUntouchedShm, 1);
    }
  }
  return EXCEPTION_CONTINUE_EXECUTION;
}

__declspec(noinline) std::uint64_t RecurseUntil(std::uint64_t depth, volatile std::uint64_t* limit);

__declspec(noinline) bool TriggerHandledStackOverflowAndReset()
{
  bool caught = false;
  __try {
    volatile std::uint64_t limit = ~0ull;
    (void)RecurseUntil(0, &limit);
  } __except (GetExceptionCode() == EXCEPTION_STACK_OVERFLOW ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
    caught = true;
  }
  if (!caught) {
    return false;
  }
  return _resetstkoflw() != 0;
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
// 0: not measured.
DWORD g_exceptionContextBytes = 0;
// Larger than any context: a distance beyond it is not one (the context was
// not under the stack pointer).
constexpr std::uint64_t kExceptionContextBytesMax = 256u * 1024u;

int NoteExceptionContextBytes(const EXCEPTION_POINTERS* ep)
{
  // Unsigned: a context above the stack pointer gives a huge distance.
  const std::uint64_t bytes = ep->ContextRecord->Rsp - reinterpret_cast<std::uintptr_t>(ep->ContextRecord);
  g_exceptionContextBytes = bytes <= kExceptionContextBytesMax ? static_cast<DWORD>(bytes) : 0u;
  return EXCEPTION_EXECUTE_HANDLER;
}

// Takes a fault of its own to see it, so only before the plugin's handlers
// are installed: they would record it. Returns 0 when it could not tell.
__declspec(noinline) DWORD MeasureExceptionContextBytes()
{
  g_exceptionContextBytes = 0;
  __try {
    WriteThroughNull();
  } __except (NoteExceptionContextBytes(GetExceptionInformation())) {
  }
  return g_exceptionContextBytes;
}

// Windows puts the exception's CONTEXT, with the processor's extended state,
// on the stack that overflowed, and keeps three pages of every stack back for
// that (11,744 to 12,288 bytes were left where it was measured). On those
// hosts a context takes 1,792 to 3,448 bytes and the handlers and filters here
// are done in the rest. Where Windows saves a large extended state (Intel AMX
// on Windows Server 2022: 11,648 to 11,696 bytes, any thread) the context does not
// fit: Windows ends the process and calls no handler, the plugin's included,
// so there is nothing of the plugin's to test (ADR-0032, Decision 5). There,
// and only there, the child has Windows keep back the context and two pages;
// elsewhere it overflows with the default, as the game does. Not the same
// room: Windows keeps a page more than asked, so 12,416 to 12,928 bytes were
// left after the context there, and 8,320 to 10,496 on the ordinary hosts.
// A handler that came to need more would fail on the ordinary hosts first.
constexpr DWORD kDefaultStackOverflowReserveBytes = 3u * 4096u;
constexpr DWORD kStackOverflowHandlerRoomBytes = 2u * 4096u;

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
  if (contextBytes == 0u) {
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
  OpenChildObsIfPresent(goEventName);
  SetUnhandledExceptionFilter(std::strcmp(mode, "resumed-fault") == 0 ? ChildResumingFilter : ChildPreviousFilter);
  std::signal(SIGABRT, ChildPreviousAbortHandler);
  if (std::strcmp(mode, "unhandled-fault-short-hold") == 0 || std::strcmp(mode, "so-short-hold") == 0) {
    skydiag::plugin::SetUnhandledCrashHoldMs(400);
  }
  if (std::strcmp(mode, "so-duplicate-guard") == 0) {
    skydiag::plugin::SetUnhandledCrashHoldMs(1500);
  }
  if (std::strcmp(mode, "unhandled-fault-no-hold") == 0 || std::strcmp(mode, "so-no-hold") == 0) {
    skydiag::plugin::SetUnhandledCrashHoldMs(0);
  }
  if (std::strcmp(mode, "so-large-hold-capped") == 0) {
    skydiag::plugin::SetUnhandledCrashHoldMs(15000);
  }
  const bool stackOverflowKeptBack =
    std::strcmp(mode, "unhandled-stack-overflow-kept-back") == 0 ||
    std::strcmp(mode, "so-front-heavy-veh-kept-back") == 0;
  const bool soFrontHeavyVehUnmoved = std::strcmp(mode, "so-front-heavy-veh-unmoved") == 0;
  const bool soFrontHeavyVeh =
    std::strcmp(mode, "so-front-heavy-veh") == 0 ||
    std::strcmp(mode, "so-front-heavy-veh-kept-back") == 0;
  const bool soMode0 = std::strcmp(mode, "so-mode-0") == 0;
  const bool soMode2 = std::strcmp(mode, "so-mode-2") == 0;
  const bool soNonSo = std::strcmp(mode, "so-non-so-ignores-shm") == 0;
  const bool soDuplicate = std::strcmp(mode, "so-duplicate-guard") == 0;
  const bool soWhileFrozen = std::strcmp(mode, "so-while-frozen") == 0;
  const bool soHandledThenSecond = std::strcmp(mode, "so-handled-then-second") == 0;
  const bool soThenHelperGoneSo = std::strcmp(mode, "so-then-helper-gone-so") == 0;
  const bool soThenHelperGoneFilter = std::strcmp(mode, "so-then-helper-gone-filter") == 0;
  const bool soThawedDuringGuardHold = std::strcmp(mode, "so-thawed-during-guard-hold") == 0;
  const bool soRemoveWhileActive = std::strcmp(mode, "so-remove-while-active") == 0;
  const bool stackOverflow =
    stackOverflowKeptBack ||
    std::strcmp(mode, "unhandled-stack-overflow") == 0 ||
    soFrontHeavyVehUnmoved ||
    soFrontHeavyVeh ||
    std::strcmp(mode, "so-short-hold") == 0 ||
    std::strcmp(mode, "so-no-hold") == 0 ||
    std::strcmp(mode, "so-large-hold-capped") == 0 ||
    soMode2 ||
    soDuplicate ||
    soWhileFrozen ||
    soHandledThenSecond ||
    soThenHelperGoneSo ||
    soThenHelperGoneFilter ||
    soThawedDuringGuardHold ||
    soRemoveWhileActive;
  if (stackOverflow && !KeepStackForExceptionContext(MeasureExceptionContextBytes(), stackOverflowKeptBack)) {
    return 97;
  }
  const std::uint32_t hookMode = soMode0 ? 0u : (soMode2 ? 2u : 1u);
  if (!skydiag::plugin::InitSharedMemory() || !skydiag::plugin::InstallCrashHandler(hookMode)) {
    return 90;
  }
  if (soMode0) {
    if (skydiag::plugin::IsStackOverflowGuardInstalled()) {
      return 80;
    }
    if (skydiag::plugin::MoveStackOverflowGuardToFront() !=
        skydiag::plugin::StackOverflowGuardMoveResult::kNotInstalled) {
      return 81;
    }
    HANDLE go = OpenEventA(SYNCHRONIZE, FALSE, goEventName);
    if (!go || WaitForSingleObject(go, 30000) != WAIT_OBJECT_0) {
      return 91;
    }
    return 0;
  }
  if (soFrontHeavyVehUnmoved) {
    // Control run: heavy front VEH registered AFTER InstallCrashHandler without
    // calling MoveStackOverflowGuardToFront(), reproducing the lost crash.
    if (!AddVectoredExceptionHandler(1, ChildFrontHeavyStackOverflowVeh)) {
      return 98;
    }
  }
  if (soFrontHeavyVeh || soMode2) {
    if (!skydiag::plugin::IsStackOverflowGuardInstalled()) {
      return 82;
    }
    // Registered at the front AFTER InstallCrashHandler, like a late-loaded
    // third-party module (e.g., frame-generation DLL) that adds a heavy VEH.
    if (!AddVectoredExceptionHandler(1, ChildFrontHeavyStackOverflowVeh)) {
      return 98;
    }
    if (skydiag::plugin::MoveStackOverflowGuardToFront() != skydiag::plugin::StackOverflowGuardMoveResult::kMoved) {
      return 83;
    }
  }
  if (soNonSo) {
    if (!AddVectoredExceptionHandler(1, ChildNonStackOverflowConsumingVeh)) {
      return 98;
    }
    if (skydiag::plugin::MoveStackOverflowGuardToFront() != skydiag::plugin::StackOverflowGuardMoveResult::kMoved) {
      return 83;
    }
  }
  if (soDuplicate) {
    if (!skydiag::plugin::AddDuplicateStackOverflowGuardRegistrationForTest()) {
      return 84;
    }
  }
  if (soRemoveWhileActive) {
    if (!AddVectoredExceptionHandler(1, ChildStackOverflowMarkerVeh)) {
      return 98;
    }
    if (skydiag::plugin::MoveStackOverflowGuardToFront() != skydiag::plugin::StackOverflowGuardMoveResult::kMoved) {
      return 83;
    }
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
  if (std::strcmp(mode, "unhandled-fault-after-thaw") == 0) {
    AddVectoredExceptionHandler(0, [](EXCEPTION_POINTERS* ep) noexcept -> LONG {
      if (ep && ep->ExceptionRecord && ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        if (auto* shm = skydiag::plugin::GetShared()) {
          InterlockedAnd(
            reinterpret_cast<volatile LONG*>(&shm->header.state_flags),
            ~static_cast<LONG>(skydiag::kState_Frozen));
        }
      }
      return EXCEPTION_CONTINUE_SEARCH;
    });
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
  if (soNonSo) {
    g_childExpectedWriteIndex = skydiag::plugin::GetShared()->header.write_index;
    // Continuable AV handled by ChildNonStackOverflowConsumingVeh right behind
    // StackOverflowFrontGuard (and ahead of VectoredHandler).
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    return 0;
  }
  if (soWhileFrozen) {
    if (CatchWriteThroughNull() != 1) {
      return 85;
    }
    const std::string go2Name = std::string(goEventName) + "_go2";
    HANDLE go2 = OpenEventA(SYNCHRONIZE, FALSE, go2Name.c_str());
    if (!go2 || WaitForSingleObject(go2, 30000) != WAIT_OBJECT_0) {
      return 86;
    }
    volatile std::uint64_t limit = ~0ull;
    return static_cast<int>(RecurseUntil(0, &limit) & 1u) + 94;
  }
  if (soHandledThenSecond || soThenHelperGoneSo || soThenHelperGoneFilter) {
    if (!TriggerHandledStackOverflowAndReset()) {
      return 87;
    }
    const std::string readyName = std::string(goEventName) + "_ready";
    HANDLE ready = OpenEventA(EVENT_MODIFY_STATE, FALSE, readyName.c_str());
    if (!ready) {
      return 89;
    }
    SetEvent(ready);
    CloseHandle(ready);
    const std::string go2Name = std::string(goEventName) + "_go2";
    HANDLE go2 = OpenEventA(SYNCHRONIZE, FALSE, go2Name.c_str());
    if (!go2 || WaitForSingleObject(go2, 30000) != WAIT_OBJECT_0) {
      return 86;
    }
    if (soThenHelperGoneFilter) {
      WriteThroughNull();
      return 88;
    }
    volatile std::uint64_t limit = ~0ull;
    return static_cast<int>(RecurseUntil(0, &limit) & 1u) + 94;
  }
  if (soRemoveWhileActive) {
    const std::string go2Name = std::string(goEventName) + "_go2";
    std::thread remover([go2Name]() {
      HANDLE go2 = OpenEventA(SYNCHRONIZE, FALSE, go2Name.c_str());
      if (!go2 || WaitForSingleObject(go2, 30000) != WAIT_OBJECT_0) {
        return;
      }
      const auto skipped = skydiag::plugin::MoveStackOverflowGuardToFront();
      const auto forced = skydiag::plugin::MoveStackOverflowGuardToFrontIgnoringActiveCountForTest();
      if (g_childObs) {
        InterlockedExchange(&g_childObs->skippedMoveResult, static_cast<LONG>(skipped));
        InterlockedExchange(&g_childObs->forceMoveResult, static_cast<LONG>(forced));
        InterlockedExchange(&g_childObs->workerDone, 1);
      }
      CloseHandle(go2);
    });
    remover.detach();
    volatile std::uint64_t limit = ~0ull;
    return static_cast<int>(RecurseUntil(0, &limit) & 1u) + 94;
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

LONG WINAPI ReenterSecondFaultFilter(EXCEPTION_POINTERS* ep)
{
  g_childChainingFilterPrevious(ep);
  return EXCEPTION_EXECUTE_HANDLER;
}

LONG WINAPI ChildSecondFaultChainingFilter(EXCEPTION_POINTERS*)
{
  // Give the reentrant call its own first-chance candidate, before the outer
  // filter returns. It must not count as another eligible B.
  __try {
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
  } __except (ReenterSecondFaultFilter(GetExceptionInformation())) {
  }
  return EXCEPTION_CONTINUE_EXECUTION;
}

int RunSecondFaultChild(const char* mode, const char* goEventName, const char* crashLoggerPath)
{
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  const bool stackOverflow = std::strcmp(mode, "stack-overflow") == 0;
  SetUnhandledExceptionFilter(stackOverflow ? ChildPreviousFilter : ChildResumingFilter);
  if (stackOverflow && !KeepStackForExceptionContext(MeasureExceptionContextBytes(), false)) {
    return 89;
  }
  const bool haveRange = std::strcmp(mode, "range-unpublished") != 0;
  const auto crashLogger = haveRange ? LoadLibraryA(crashLoggerPath) : nullptr;
  if (haveRange && !crashLogger) {
    return 90;
  }
  if (std::strcmp(mode, "zero-hold") == 0) {
    skydiag::plugin::SetUnhandledCrashHoldMs(0);
  }
  if (std::strcmp(mode, "short-hold") == 0) {
    skydiag::plugin::SetUnhandledCrashHoldMs(1500);
  }
  if (!skydiag::plugin::InitSharedMemory() || !skydiag::plugin::InstallCrashHandler(1)) {
    return 91;
  }
  // The SKSE lifecycle's lookup once every plugin is loaded.
  if (std::strcmp(mode, "lookups-pending") != 0) {
    skydiag::plugin::NoteCrashLoggerModuleRangesFinal();
  }
  const std::string startBName = std::string(goEventName) + "_B";
  HANDLE go = OpenEventA(SYNCHRONIZE, FALSE, goEventName);
  HANDLE startB = OpenEventA(SYNCHRONIZE, FALSE, startBName.c_str());
  if (!go || !startB || WaitForSingleObject(go, 10000) != WAIT_OBJECT_0 || CatchWriteThroughNull() != 1) {
    return 92;
  }
  if (std::strcmp(mode, "thawed") == 0) {
    if (!AddVectoredExceptionHandler(0, [](EXCEPTION_POINTERS*) -> LONG {
          RearmIncident(skydiag::plugin::GetShared());
          return EXCEPTION_CONTINUE_SEARCH;
        })) {
      return 93;
    }
  }
  if (std::strcmp(mode, "range-inside") == 0) {
    // Put this synthetic exception inside the stand-in module's image,
    // without changing the machine context that allows it to resume.
    g_probe = reinterpret_cast<ProbeFn>(GetProcAddress(crashLogger, "SkydiagFakeCrashLoggerProbe"));
    if (!g_probe || !AddVectoredExceptionHandler(1, [](EXCEPTION_POINTERS* ep) -> LONG {
          ep->ExceptionRecord->ExceptionAddress = reinterpret_cast<void*>(g_probe);
          return EXCEPTION_CONTINUE_SEARCH;
        })) {
      return 93;
    }
  }
  if (std::strcmp(mode, "reentered") == 0) {
    g_childChainingFilterPrevious = SetUnhandledExceptionFilter(ChildSecondFaultChainingFilter);
    if (skydiag::plugin::ReclaimTopLevelFilter() != reinterpret_cast<std::uintptr_t>(ChildSecondFaultChainingFilter)) {
      return 93;
    }
  }
  if (WaitForSingleObject(startB, 10000) != WAIT_OBJECT_0) {
    return 94;
  }
  CloseHandle(startB);
  CloseHandle(go);
  if (stackOverflow) {
    volatile std::uint64_t limit = ~0ull;
    return static_cast<int>(RecurseUntil(0, &limit) & 1u) + 94;
  }
  if (std::strcmp(mode, "different-thread") == 0) {
    std::thread worker([] { RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr); });
    worker.join();
  } else {
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    if (std::strcmp(mode, "same-thread") == 0) {
      RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
  }
  return 0;
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

// For a failure message: what this host's exceptions take (measured by the
// test itself before any child runs).
std::string DescribeHost()
{
  return g_exceptionContextBytes != 0u
    ? "exception context " + std::to_string(g_exceptionContextBytes) + " bytes on this host"
    : std::string("exception context on this host not measured");
}

// What the child left behind, for a failure message: a failure that shows on
// one CI runner in several cannot be run again to look at.
std::string Describe(const ChildOutcome& outcome)
{
  char text[256]{};
  std::snprintf(
    text,
    sizeof(text),
    " (child: crash event signaled %d, recorded code 0x%08lX on thread %lu, main thread %lu, crash_seq %lu, "
    "state flags 0x%lX, exit code 0x%08lX; ",
    outcome.crashEventSignaled ? 1 : 0,
    static_cast<unsigned long>(outcome.crash.exception_code),
    static_cast<unsigned long>(outcome.crash.faulting_tid),
    static_cast<unsigned long>(outcome.mainTid),
    static_cast<unsigned long>(outcome.crashSeq),
    static_cast<unsigned long>(outcome.stateFlags),
    static_cast<unsigned long>(outcome.exitCode));
  return text + DescribeHost() + ")";
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
    "the helper could not attach to the child's shared memory and crash event (" + state + "; " +
    DescribeHost() + ")");
}

void TestSecondFaultChild(const char* mode, const char* crashLoggerPath)
{
  namespace fault = skydiag::second_fault;
  char exePath[MAX_PATH]{};
  Require(GetModuleFileNameA(nullptr, exePath, MAX_PATH) != 0, "GetModuleFileNameA failed");
  const std::string goName = "Local\\skydiag_fault2_test_" + std::to_string(GetCurrentProcessId());
  const std::string startBName = goName + "_B";
  const HANDLE go = CreateEventA(nullptr, TRUE, FALSE, goName.c_str());
  const HANDLE startB = CreateEventA(nullptr, TRUE, FALSE, startBName.c_str());
  Require(go && startB, "second fault barrier creation failed");

  std::string command = std::string("\"") + exePath + "\" --child-second-fault " + mode + " " +
                        goName + " \"" + crashLoggerPath + "\"";
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  Require(CreateProcessA(exePath, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi),
          "second fault child creation failed");

  skydiag::helper::AttachedProcess proc{};
  bool attached = false;
  for (int attempt = 0; attempt < 500 && !attached; ++attempt) {
    attached = skydiag::helper::AttachByPid(pi.dwProcessId, proc, nullptr) && proc.crashEvent;
    if (!attached && WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
      break;
    }
  }
  if (!attached) {
    CloseHandle(startB);
    FailChildNotAttached(pi, go);
  }

  HANDLE settled = nullptr;
  const auto cleanup = [&] {
    if (WaitForSingleObject(pi.hProcess, 0) != WAIT_OBJECT_0) {
      TerminateProcess(pi.hProcess, 1);
      WaitForSingleObject(pi.hProcess, 2000);
    }
    if (settled) {
      CloseHandle(settled);
    }
    skydiag::helper::Detach(proc);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(startB);
    CloseHandle(go);
  };
  try {
    Require(proc.secondFault && proc.secondFaultPosted, "plugin did not create the second fault mapping and event");
    const bool sameThread = std::strcmp(mode, "same-thread") == 0;
    const bool differentThread = std::strcmp(mode, "different-thread") == 0;
    const bool zeroHold = std::strcmp(mode, "zero-hold") == 0;
    const bool reentered = std::strcmp(mode, "reentered") == 0;
    const bool stackOverflow = std::strcmp(mode, "stack-overflow") == 0;
    const bool shortHold = std::strcmp(mode, "short-hold") == 0;
    const bool stateSettle = std::strcmp(mode, "state-settle") == 0;
    const bool expectPost = sameThread || differentThread || zeroHold || reentered || shortHold || stateSettle;
    const bool haveHelper = std::strcmp(mode, "no-helper") != 0;
    if (haveHelper) {
      const auto name = skydiag::protocol::MakeKernelName(pi.dwProcessId, fault::kSettledSuffix);
      settled = CreateEventW(nullptr, FALSE, FALSE, name.c_str());
      Require(settled, "second fault settlement event creation failed");
      // A stale wake must never release a different publication.
      Require(SetEvent(settled), "setting stale second fault wake failed");
    }

    SetEvent(go);
    Require(WaitForSingleObject(proc.crashEvent, 5000) == WAIT_OBJECT_0, "A did not publish");
    const auto seq = proc.shm->header.crash_seq;
    const auto addressA = proc.shm->header.crash.exception_addr;
    Require(seq != 0 && (seq & 1u) == 0 && addressA != 0, "A is not committed");
    Require(fault::ReadState(*proc.secondFault) == fault::kEmpty, "handled A filled the second fault slot");
    SetEvent(startB);
    if (expectPost) {
      Require(WaitForSingleObject(proc.secondFaultPosted, 2000) == WAIT_OBJECT_0, "B did not post");
      const auto& b = *proc.secondFault;
      Require(fault::ReadState(b) == fault::kPosted && b.a_crash_seq == seq, "B has the wrong A generation");
      Require(b.tid != 0 && (b.tid == pi.dwThreadId) == !differentThread, "B has the wrong thread");
      Require(b.code == EXCEPTION_ACCESS_VIOLATION && b.address != addressA && b.address != 0,
              "B has the wrong exception");
      Require(b.exception_record.ExceptionCode == b.code && b.context.ContextFlags && b.context.Rsp &&
                b.qpc && (b.hold_until_qpc > b.qpc) == !zeroHold,
              "B does not carry its own context and hold deadline");
      if (shortHold) {
        // Nobody settles it: the finite hold (1.5 s from the filter's entry)
        // ends the wait, and B stays posted for the helper.
        Require(WaitForSingleObject(pi.hProcess, 150) == WAIT_TIMEOUT, "B was not held until its limit");
        Require(WaitForSingleObject(pi.hProcess, 3000) == WAIT_OBJECT_0, "the hold limit did not end the wait");
        Require(fault::ReadState(*proc.secondFault) == fault::kPosted, "an expired hold left the slot as posted");
      } else if (stateSettle) {
        // The wake went to another waiter: the state alone must end the wait
        // long before the 5 s limit.
        Require(WaitForSingleObject(pi.hProcess, 100) == WAIT_TIMEOUT, "B was released early");
        const auto settledAt = std::chrono::steady_clock::now();
        InterlockedExchange(&proc.secondFault->state, fault::kSettled);
        Require(WaitForSingleObject(pi.hProcess, 1000) == WAIT_OBJECT_0, "B waited past its settled state");
        Require(std::chrono::steady_clock::now() - settledAt < std::chrono::milliseconds(1000),
                "a settled state without a wake ends the wait within a slice");
      } else if (!zeroHold) {
        Require(WaitForSingleObject(pi.hProcess, 100) == WAIT_TIMEOUT, "B was released by a stale wake");
        InterlockedExchange(&proc.secondFault->state, fault::kSettled);
        SetEvent(settled);
      }
    }
    Require(WaitForSingleObject(pi.hProcess, 2000) == WAIT_OBJECT_0, "B never continued after its lower filter");
    DWORD exit = 1;
    Require(GetExitCodeProcess(pi.hProcess, &exit) && exit == (stackOverflow ? kChildFilterExitCode : 0),
            "the lower filter was not reached");
    if (expectPost) {
      Require(proc.secondFault->resumed == 1, "B's resume was not recorded");
      Require(fault::ReadDropped(*proc.secondFault, seq) == (sameThread ? 1 : 0),
              "only the second B must be counted as dropped");
      Require(WaitForSingleObject(proc.secondFaultPosted, 0) == WAIT_TIMEOUT, "another B overwrote A's slot");
    } else {
      Require(fault::ReadState(*proc.secondFault) == fault::kEmpty &&
                WaitForSingleObject(proc.secondFaultPosted, 0) == WAIT_TIMEOUT,
              "B posted without a matching A, known range, or compatible helper");
    }
    Require(proc.shm->header.crash_seq == seq && proc.shm->header.crash.exception_addr == addressA,
            "B changed the v4 record");
    Require(WaitForSingleObject(proc.unhandledEvent, 0) == WAIT_TIMEOUT &&
              WaitForSingleObject(proc.dumpSettledEvent, 0) == WAIT_TIMEOUT,
            "B used A's events");
  } catch (...) {
    cleanup();
    throw;
  }
  cleanup();
}

namespace fault_tests {

namespace fault = skydiag::second_fault;

// Item: B candidates. Two threads that pick the same free entry at once must
// both end up with an entry of their own.
void TestCandidateTableConcurrentClaims()
{
  constexpr int kRounds = 20000;
  fault::CandidateTable* table = nullptr;
  std::atomic<int> round{0};
  std::atomic<int> done{0};
  std::atomic<bool> stop{false};
  bool noted[2]{};
  const auto worker = [&](int index) {
    int seen = 0;
    for (;;) {
      int current = round.load(std::memory_order_acquire);
      while (current == seen && !stop.load(std::memory_order_acquire)) {
        YieldProcessor();
        current = round.load(std::memory_order_acquire);
      }
      if (stop.load(std::memory_order_acquire)) {
        return;
      }
      seen = current;
      noted[index] = table->Note(DWORD{100} + static_cast<DWORD>(index),&noted[index], 2u, [](std::uint32_t) noexcept {
        return true;
      });
      done.fetch_add(1, std::memory_order_acq_rel);
    }
  };
  std::thread first(worker, 0);
  std::thread second(worker, 1);
  int lost = 0;
  for (int i = 1; i <= kRounds; ++i) {
    auto fresh = std::make_unique<fault::CandidateTable>();
    table = fresh.get();
    done.store(0, std::memory_order_release);
    round.store(i, std::memory_order_release);
    while (done.load(std::memory_order_acquire) != 2) {
      YieldProcessor();
    }
    if (!noted[0] || !noted[1] || table->Take(100, &noted[0]) != 2u || table->Take(101, &noted[1]) != 2u) {
      ++lost;
    }
  }
  stop.store(true, std::memory_order_release);
  first.join();
  second.join();
  if (lost != 0) {
    std::fprintf(stderr, "candidate claims lost in %d of %d rounds\n", lost, kRounds);
  }
  Require(lost == 0, "two threads claiming free entries at once must both keep a candidate");
}

void TestCandidateTableOwnershipAndSaturation()
{
  const auto live = [](std::uint32_t seq) noexcept { return seq == 4u; };
  int records[fault::CandidateTable::kCount + 2]{};
  fault::CandidateTable table;
  for (DWORD i = 0; i < fault::CandidateTable::kCount; ++i) {
    Require(table.Note(10 + i, &records[i], 4u, live), "a free entry takes a candidate");
  }
  Require(!table.Note(99, &records[8], 4u, live), "a live candidate of another thread is never replaced");
  Require(table.Note(10, &records[9], 4u, live), "a thread updates its own entry");
  Require(table.Take(10, &records[0]) == 0u && table.Take(10, &records[9]) == 4u,
          "only the thread's latest candidate is kept");
  Require(table.Note(10, &records[0], 4u, live), "the freed entry is reusable");
  Require(!table.Note(10, &records[1], 0u, live) && table.Take(10, &records[0]) == 4u,
          "forgetting another record keeps the candidate");
  Require(table.Note(11, &records[1], 0u, live) && table.Take(11, &records[1]) == 0u,
          "forgetting the same record drops the candidate");
  Require(table.Take(12, &records[1]) == 0u, "another thread's record is not taken");

  fault::CandidateTable stale;
  for (DWORD i = 0; i < fault::CandidateTable::kCount; ++i) {
    Require(stale.Note(10 + i, &records[i], i == 3u ? 2u : 4u, live), "fill");
  }
  Require(stale.Note(99, &records[8], 4u, live), "an entry whose generation is over can be reused");
  Require(stale.Take(13, &records[3]) == 0u && stale.Take(99, &records[8]) == 4u, "the over entry was reused");
  for (DWORD i = 0; i < fault::CandidateTable::kCount; ++i) {
    Require(i == 3u || stale.Take(10 + i, &records[i]) == 4u, "live entries survive the reuse");
  }
}

void TestSlotClaimAndDropped()
{
  auto slot = std::make_unique<fault::Slot>();
  Require(fault::ClaimSlot(*slot, 4u, []() noexcept { return true; }) == fault::SlotClaim::kClaimed &&
            fault::ReadState(*slot) == fault::kWriting && fault::ReadOwner(*slot) == 4u,
          "an empty slot is claimed with its owner written at once");
  Require(fault::ClaimSlot(*slot, 4u, []() noexcept { return true; }) == fault::SlotClaim::kTaken &&
            fault::ReadDropped(*slot, 4u) == 1,
          "the same A's second B is counted as dropped");
  Require(fault::ClaimSlot(*slot, 6u, []() noexcept { return true; }) == fault::SlotClaim::kTaken &&
            fault::ReadDropped(*slot, 6u) == 0 && fault::ReadDropped(*slot, 4u) == 1,
          "a B of another A is not counted on this A's slot");
  Require(fault::ClaimSlot(*slot, 4u, []() noexcept { return false; }) == fault::SlotClaim::kTaken &&
            fault::ReadDropped(*slot, 4u) == 1,
          "a B whose A was let go is not counted");

  // A late count of the old generation never carries into the next.
  fault::CountDropped(*slot, 4u);
  Require(fault::ReadDropped(*slot, 4u) == 2, "counts accumulate per generation");
  // The helper empties the slot (it leaves the counter as it is).
  InterlockedExchange(&slot->state, fault::kEmpty);
  Require(fault::ClaimSlot(*slot, 6u, []() noexcept { return true; }) == fault::SlotClaim::kClaimed &&
            fault::ReadDropped(*slot, 6u) == 0,
          "the next A starts with no dropped count");
  fault::CountDropped(*slot, 4u);
  Require(fault::ReadDropped(*slot, 6u) == 0, "a late count of the earlier A is not the next A's");

  auto ended = std::make_unique<fault::Slot>();
  Require(fault::ClaimSlot(*ended, 4u, []() noexcept { return false; }) == fault::SlotClaim::kGenerationEnded &&
            fault::ReadState(*ended) == fault::kSettled && fault::ReadOwner(*ended) == 4u,
          "a claim after A was let go leaves a settled slot for the helper to empty");
}

void TestCandidateRule()
{
  using fault::IsSecondFaultCandidate;
  Require(IsSecondFaultCandidate(true, false, 2u, 2u), "a fatal exception that lost Frozen is a candidate");
  Require(!IsSecondFaultCandidate(false, false, 2u, 2u), "another publish failure is not");
  Require(!IsSecondFaultCandidate(true, true, 2u, 2u), "Crash Logger reporting a kept exception is not");
  Require(!IsSecondFaultCandidate(true, false, 3u, 3u) && !IsSecondFaultCandidate(true, false, 0u, 0u),
          "an odd or zero generation is not");
  Require(!IsSecondFaultCandidate(true, false, 2u, 4u), "a generation that changed meanwhile is not");
}

}  // namespace fault_tests

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
  RequireOf(
    outcome,
    (outcome.stateFlags & skydiag::kState_Frozen) != 0u,
    (prefix + "the incident must be claimed").c_str());
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
    "still reported at the exit %d, alive while held %d, %lu ms from the report to the exit; ",
    static_cast<unsigned long>(outcome.recordedCode),
    static_cast<unsigned long>(outcome.crashSeq),
    static_cast<unsigned long>(outcome.stateFlags),
    static_cast<unsigned long>(outcome.exitCode),
    outcome.unhandledSignaled ? 1 : 0,
    outcome.unhandledSignaledAtExit ? 1 : 0,
    outcome.aliveWhileHeld ? 1 : 0,
    static_cast<unsigned long>(outcome.msFromSignalToExit));
  return text + DescribeHost() + ")";
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
// before, and the top-level filter neither reports nor holds it, so the filter
// under the plugin's still ends the process. Since ADR-0037, the front guard
// holds the thread until DUMP_SETTLED (up to its 5 s cap) before releasing it
// to the top-level filter; because the parent passes HelperSettle::kNever here,
// each mode waits out that 5 s guard cap before reaching the filter.
// "-kept-back": the same with stack kept back on every host, the path a host
// with a large exception context takes (KeepStackForExceptionContext).
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

// A fault whose incident was already thawed by the helper (released during a
// recovery/thread-dump check) before reaching the top-level filter must still
// signal unhandledEvent (without holding on settled) so the helper can report
// the released crash.
void TestReleasedFaultIsStillReportedAsUnhandledWithoutHolding()
{
  const auto outcome = RunChildToUnhandled("unhandled-fault-after-thaw", HelperSettle::kNever, 0);
  RequireOf(outcome, outcome.recordedCode == EXCEPTION_ACCESS_VIOLATION, "the fault was recorded at first chance");
  RequireOf(outcome, (outcome.stateFlags & skydiag::kState_Frozen) == 0u, "the incident was thawed before the filter");
  RequireOf(outcome, outcome.unhandledSignaled, "a thawed fault that reaches the top-level filter must still signal");
  RequireOf(outcome, outcome.msFromSignalToExit < 2000u, "a thawed fault must not block waiting on dumpSettled");
  RequireOf(outcome, outcome.exitCode == kChildFilterExitCode, "the filter under the plugin's must still run");
}

// A stale unhandled signal left behind after a previous crash thawed must be
// cleared when the next crash claims ownership and publishes a new crash_seq.
void TestStaleUnhandledSignalIsClearedWhenNextCrashPublishes(skydiag::SharedLayout* shm)
{
  RearmIncident(shm);
  const HANDLE unhandled = skydiag::plugin::GetUnhandledEvent();
  Require(unhandled != nullptr, "GetUnhandledEvent must be available");
  Require(SetEvent(unhandled) != FALSE, "SetEvent(unhandled) failed");
  Require(WaitForSingleObject(unhandled, 0) == WAIT_OBJECT_0, "precondition: stale unhandled event is set");

  const auto seqBefore = CrashSeq(shm);
  const int caught = CatchWriteThroughNull();
  Require(caught == 1, "CatchWriteThroughNull should catch the fault");
  Require(CrashSeq(shm) != seqBefore, "the new fault must publish a new crash_seq");
  Require(
    WaitForSingleObject(unhandled, 0) == WAIT_TIMEOUT,
    "TryPublishCrashRecord must clear a stale unhandled signal before publishing the new crash");
  RearmIncident(shm);
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

// ADR-0037: parent runner for stack-overflow front guard tests. Waits for the
// crash event (or process exit) rather than unhandledEvent, checks hold while
// DUMP_SETTLED is unset, then settles and reads observation state.
struct StackOverflowGuardOutcome
{
  DWORD mainTid = 0;
  DWORD exitCode = 0;
  bool crashEventSignaled = false;
  bool unhandledSignaledAtExit = false;
  bool aliveWhileHeld = false;
  LONG otherHandlerCallsBeforeSettle = 0;
  LONG otherHandlerCallsAtExit = 0;
  bool guardStackMeasured = false;
  std::uint64_t stackLimit = 0;
  std::uint64_t handlerEntryRsp = 0;
  std::uint64_t lowestNonZero = 0;
  std::uint64_t guardStackUsedBytes = 0;
  DWORD msFromCrashToExit = 0;
  std::uint32_t crashSeq = 0;
  std::uint32_t stateFlags = 0;
  skydiag::CrashInfo crash{};
  skydiag::BlackboxEvent lastEvent{};
};

std::string Describe(const StackOverflowGuardOutcome& outcome)
{
  char text[384]{};
  std::snprintf(
    text,
    sizeof(text),
    " (child: crashEvent=%d, code=0x%08lX, tid=%lu, mainTid=%lu, crash_seq=%lu, flags=0x%lX, exit=0x%08lX, "
    "aliveWhileHeld=%d, otherBeforeSettle=%ld, otherAtExit=%ld, msCrashToExit=%lu, guardStackUsed=%llu; ",
    outcome.crashEventSignaled ? 1 : 0,
    static_cast<unsigned long>(outcome.crash.exception_code),
    static_cast<unsigned long>(outcome.crash.faulting_tid),
    static_cast<unsigned long>(outcome.mainTid),
    static_cast<unsigned long>(outcome.crashSeq),
    static_cast<unsigned long>(outcome.stateFlags),
    static_cast<unsigned long>(outcome.exitCode),
    outcome.aliveWhileHeld ? 1 : 0,
    static_cast<long>(outcome.otherHandlerCallsBeforeSettle),
    static_cast<long>(outcome.otherHandlerCallsAtExit),
    static_cast<unsigned long>(outcome.msFromCrashToExit),
    static_cast<unsigned long long>(outcome.guardStackUsedBytes));
  return text + DescribeHost() + ")";
}

void RequireOf(const StackOverflowGuardOutcome& outcome, bool condition, const char* message)
{
  if (!condition) {
    throw std::runtime_error(message + Describe(outcome));
  }
}

StackOverflowGuardOutcome RunChildForStackOverflowGuard(
  const char* mode,
  HelperSettle settle,
  DWORD holdCheckMs)
{
  char exePath[MAX_PATH]{};
  Require(GetModuleFileNameA(nullptr, exePath, MAX_PATH) != 0, "GetModuleFileNameA failed");
  const std::string goEventName = "Local\\skydiag_so_guard_test_go_" + std::to_string(GetCurrentProcessId()) +
    "_" + std::to_string(GetTickCount64());
  const std::string obsName = goEventName + "_obs";
  HANDLE go = CreateEventA(nullptr, TRUE, FALSE, goEventName.c_str());
  Require(go != nullptr, "CreateEventA failed");
  HANDLE obsMapping = CreateFileMappingA(
    INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(StackOverflowChildObs), obsName.c_str());
  Require(obsMapping != nullptr, "CreateFileMappingA failed");
  auto* obs = static_cast<StackOverflowChildObs*>(
    MapViewOfFile(obsMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(StackOverflowChildObs)));
  Require(obs != nullptr, "MapViewOfFile failed");
  std::memset(obs, 0, sizeof(StackOverflowChildObs));

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
    UnmapViewOfFile(obs);
    CloseHandle(obsMapping);
    FailChildNotAttached(pi, go);
  }

  StackOverflowGuardOutcome outcome{};
  outcome.mainTid = pi.dwThreadId;
  if (settle == HelperSettle::kNoEvent && proc.dumpSettledEvent) {
    CloseHandle(proc.dumpSettledEvent);
    proc.dumpSettledEvent = nullptr;
  }

  SetEvent(go);
  const HANDLE crashOrExit[] = { proc.crashEvent, pi.hProcess };
  const DWORD waitResult = WaitForMultipleObjects(2, crashOrExit, FALSE, 30000);
  outcome.crashEventSignaled = (waitResult == WAIT_OBJECT_0) ||
    (WaitForSingleObject(proc.crashEvent, 0) == WAIT_OBJECT_0);
  const ULONGLONG crashTick = GetTickCount64();

  if (outcome.crashEventSignaled && settle == HelperSettle::kAfterCheck) {
    outcome.aliveWhileHeld = WaitForSingleObject(pi.hProcess, holdCheckMs) == WAIT_TIMEOUT;
    outcome.otherHandlerCallsBeforeSettle = InterlockedCompareExchange(&obs->otherHandlerCalls, 0, 0);
    if (proc.dumpSettledEvent) {
      SetEvent(proc.dumpSettledEvent);
    }
  }

  if (WaitForSingleObject(pi.hProcess, 30000) != WAIT_OBJECT_0) {
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 5000);
  }
  outcome.msFromCrashToExit = static_cast<DWORD>(GetTickCount64() - crashTick);
  outcome.unhandledSignaledAtExit =
    proc.unhandledEvent && WaitForSingleObject(proc.unhandledEvent, 0) == WAIT_OBJECT_0;
  GetExitCodeProcess(pi.hProcess, &outcome.exitCode);

  outcome.crashSeq = proc.shm->header.crash_seq;
  outcome.stateFlags = proc.shm->header.state_flags;
  outcome.crash = proc.shm->header.crash;
  if (proc.shm->header.write_index > 0u && proc.shm->header.capacity > 0u) {
    outcome.lastEvent = LastEvent(proc.shm);
  }
  outcome.otherHandlerCallsAtExit = InterlockedCompareExchange(&obs->otherHandlerCalls, 0, 0);
  outcome.guardStackMeasured = InterlockedCompareExchange(&obs->guardStackMeasured, 0, 0) != 0;
  outcome.stackLimit = obs->stackLimit;
  outcome.handlerEntryRsp = obs->handlerEntryRsp;
  outcome.lowestNonZero = obs->lowestNonZero;
  outcome.guardStackUsedBytes = obs->guardStackUsedBytes;

  UnmapViewOfFile(obs);
  CloseHandle(obsMapping);
  skydiag::helper::Detach(proc);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  CloseHandle(go);
  return outcome;
}

// ADR-0037 Completion Condition 1 & 2: a late-registered front VEH that touches
// >64 KB of stack on stack overflow kills the process before SkyrimDiag's
// VectoredHandler runs unless the stack-overflow front guard is moved ahead of it.
void TestStackOverflowWithFrontHeavyVehRecordsAndHolds()
{
  // Control run: without moving the guard to the front, the heavy front VEH
  // exhausts the remaining stack first and SkyrimDiag never records the crash.
  const auto lost = RunChildForStackOverflowGuard("so-front-heavy-veh-unmoved", HelperSettle::kAfterCheck, 700);
  RequireOf(
    lost,
    !lost.crashEventSignaled && lost.crashSeq == 0u,
    "without moving the guard to the front, the heavy front VEH must kill the process before any crash is recorded");
  RequireOf(lost, lost.otherHandlerCallsAtExit >= 1, "the heavy front VEH must have run in the control run");
  RequireOf(
    lost,
    lost.exitCode != kChildFilterExitCode && lost.exitCode != 0u,
    "in the control run, the heavy front VEH must terminate the process before the lower filter");

  for (const char* mode : { "so-front-heavy-veh", "so-front-heavy-veh-kept-back" }) {
    const auto outcome = RunChildForStackOverflowGuard(mode, HelperSettle::kAfterCheck, 700);
    const std::string prefix = std::string(mode) + ": ";
    RequireOf(outcome, outcome.crashEventSignaled, (prefix + "stack overflow must signal the crash event").c_str());
    RequireOf(
      outcome,
      outcome.crashSeq != 0u && (outcome.crashSeq & 1u) == 0u,
      (prefix + "stack overflow must commit a crash record").c_str());
    RequireOf(
      outcome,
      (outcome.stateFlags & skydiag::kState_Frozen) != 0u,
      (prefix + "stack overflow must claim Frozen").c_str());
    RequireOf(
      outcome,
      outcome.crash.exception_code == EXCEPTION_STACK_OVERFLOW,
      (prefix + "stack overflow must record EXCEPTION_STACK_OVERFLOW (0xC00000FD)").c_str());
    RequireOf(
      outcome,
      outcome.crash.faulting_tid == outcome.mainTid,
      (prefix + "stack overflow must record the main thread").c_str());
    RequireOf(
      outcome,
      outcome.lastEvent.type == static_cast<std::uint16_t>(skydiag::EventType::kCrash) &&
        outcome.lastEvent.payload.a == EXCEPTION_STACK_OVERFLOW,
      (prefix + "blackbox must end with kCrash for 0xC00000FD").c_str());
    RequireOf(
      outcome,
      outcome.aliveWhileHeld,
      (prefix + "the guard must hold the overflowing thread until the helper sets DUMP_SETTLED").c_str());
    RequireOf(
      outcome,
      outcome.otherHandlerCallsBeforeSettle == 0,
      (prefix + "the other front VEH must not run while the guard holds for DUMP_SETTLED").c_str());
    RequireOf(
      outcome,
      outcome.otherHandlerCallsAtExit >= 1,
      (prefix + "the other front VEH must run once DUMP_SETTLED releases the guard").c_str());
    RequireOf(
      outcome,
      outcome.exitCode != kChildFilterExitCode && outcome.exitCode != 0u,
      (prefix + "after DUMP_SETTLED releases the guard, the heavy front VEH must exhaust the stack and end the process before the lower filter").c_str());
    RequireOf(
      outcome,
      !outcome.unhandledSignaledAtExit,
      (prefix + "the guard must not set unhandledEvent on stack overflow").c_str());
    std::printf(
      "StackOverflowFrontGuard [%s] stack usage: %llu bytes (entryRsp=0x%llX, lowestNonZero=0x%llX, stackLimit=0x%llX)\n",
      mode,
      static_cast<unsigned long long>(outcome.guardStackUsedBytes),
      static_cast<unsigned long long>(outcome.handlerEntryRsp),
      static_cast<unsigned long long>(outcome.lowestNonZero),
      static_cast<unsigned long long>(outcome.stackLimit));
    RequireOf(
      outcome,
      outcome.guardStackMeasured && outcome.guardStackUsedBytes > 0u && outcome.guardStackUsedBytes <= 4096u,
      (prefix + "guard stack usage must be measured (>0) and <= 4096 bytes").c_str());
  }
}

// ADR-0037 Completion Condition 3: hold caps (timeout, 0, missing helper, >5000 ms clamped to 5000 ms).
void TestStackOverflowGuardHoldCapsAndNoHold()
{
  const auto shortHold = RunChildForStackOverflowGuard("so-short-hold", HelperSettle::kNever, 0);
  RequireOf(shortHold, shortHold.crashEventSignaled, "short-hold SO must signal crash event");
  RequireOf(shortHold, shortHold.crash.exception_code == EXCEPTION_STACK_OVERFLOW, "short-hold SO must record 0xC00000FD");
  RequireOf(shortHold, shortHold.msFromCrashToExit >= 300u, "short-hold SO must hold until its 400 ms cap");
  RequireOf(shortHold, shortHold.msFromCrashToExit < 3500u, "short-hold SO must release at its cap");
  RequireOf(shortHold, shortHold.exitCode == kChildFilterExitCode, "short-hold SO must reach lower filter");

  const auto noHold = RunChildForStackOverflowGuard("so-no-hold", HelperSettle::kNever, 0);
  RequireOf(noHold, noHold.crashEventSignaled, "hold=0 SO must signal crash event");
  RequireOf(noHold, noHold.crash.exception_code == EXCEPTION_STACK_OVERFLOW, "hold=0 SO must record 0xC00000FD");
  RequireOf(noHold, noHold.msFromCrashToExit < 2000u, "hold=0 SO must not hold");
  RequireOf(noHold, noHold.exitCode == kChildFilterExitCode, "hold=0 SO must reach lower filter");

  const auto noEvent = RunChildForStackOverflowGuard("unhandled-stack-overflow", HelperSettle::kNoEvent, 0);
  RequireOf(noEvent, noEvent.crashEventSignaled, "no-helper SO must signal crash event");
  RequireOf(noEvent, noEvent.crash.exception_code == EXCEPTION_STACK_OVERFLOW, "no-helper SO must record 0xC00000FD");
  RequireOf(noEvent, noEvent.msFromCrashToExit < 2000u, "no-helper SO must not hold when DUMP_SETTLED does not exist");
  RequireOf(noEvent, noEvent.exitCode == kChildFilterExitCode, "no-helper SO must reach lower filter");

  const auto largeHold = RunChildForStackOverflowGuard("so-large-hold-capped", HelperSettle::kNever, 0);
  RequireOf(largeHold, largeHold.crashEventSignaled, "large-hold SO must signal crash event");
  RequireOf(largeHold, largeHold.crash.exception_code == EXCEPTION_STACK_OVERFLOW, "large-hold SO must record 0xC00000FD");
  RequireOf(largeHold, largeHold.msFromCrashToExit >= 4500u, "large-hold SO must hold for ~5 s cap");
  RequireOf(
    largeHold,
    largeHold.msFromCrashToExit < 9000u,
    "UnhandledCrashHoldMs > 5000 must still release the stack-overflow guard at 5 s");
  RequireOf(largeHold, largeHold.exitCode == kChildFilterExitCode, "large-hold SO must reach lower filter");
}

// ADR-0037 Completion Condition 3: non-stack-overflow exceptions pass straight
// through StackOverflowFrontGuard without touching shared memory.
void TestStackOverflowGuardIgnoresNonStackOverflowExceptions()
{
  char exePath[MAX_PATH]{};
  Require(GetModuleFileNameA(nullptr, exePath, MAX_PATH) != 0, "GetModuleFileNameA failed");
  const std::string goEventName = "Local\\skydiag_so_nonso_go_" + std::to_string(GetCurrentProcessId()) +
    "_" + std::to_string(GetTickCount64());
  const std::string obsName = goEventName + "_obs";
  HANDLE go = CreateEventA(nullptr, TRUE, FALSE, goEventName.c_str());
  HANDLE obsMapping = CreateFileMappingA(
    INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(StackOverflowChildObs), obsName.c_str());
  Require(go && obsMapping, "non-SO test event/mapping creation failed");
  auto* obs = static_cast<StackOverflowChildObs*>(
    MapViewOfFile(obsMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(StackOverflowChildObs)));
  Require(obs != nullptr, "MapViewOfFile failed");
  std::memset(obs, 0, sizeof(StackOverflowChildObs));

  std::string commandLine = std::string("\"") + exePath + "\" --child so-non-so-ignores-shm " + goEventName;
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
    UnmapViewOfFile(obs);
    CloseHandle(obsMapping);
    FailChildNotAttached(pi, go);
  }

  const auto cleanup = [&] {
    if (WaitForSingleObject(pi.hProcess, 0) != WAIT_OBJECT_0) {
      TerminateProcess(pi.hProcess, 1);
      WaitForSingleObject(pi.hProcess, 2000);
    }
    UnmapViewOfFile(obs);
    CloseHandle(obsMapping);
    skydiag::helper::Detach(proc);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(go);
  };
  try {
    SetEvent(go);
    Require(WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0, "non-SO child did not exit");
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    const bool crashSignaled = WaitForSingleObject(proc.crashEvent, 0) == WAIT_OBJECT_0;
    const auto crashSeq = proc.shm->header.crash_seq;
    const LONG otherCalls = InterlockedCompareExchange(&obs->otherHandlerCalls, 0, 0);
    const LONG sawUntouched = InterlockedCompareExchange(&obs->nonSoHandlerSawUntouchedShm, 0, 0);

    Require(exitCode == 0u, "non-SO child must exit cleanly after front handler resumes");
    Require(otherCalls == 1, "the front handler behind the guard must receive the non-SO exception");
    Require(sawUntouched == 1, "the guard must not touch shared memory for non-SO exceptions");
    Require(!crashSignaled && crashSeq == 0u, "non-SO exception consumed ahead of VectoredHandler must not record a crash");
  } catch (...) {
    cleanup();
    throw;
  }
  cleanup();
}

// ADR-0037 Completion Condition 3: duplicate guard registration records once
// and holds only once (never settles DUMP_SETTLED so a second hold would double
// the 1500 ms wait to ~3000 ms).
void TestStackOverflowGuardDuplicateRegistrationRecordsOnce()
{
  const auto outcome = RunChildForStackOverflowGuard("so-duplicate-guard", HelperSettle::kNever, 0);
  RequireOf(outcome, outcome.crashEventSignaled, "duplicate guard must signal crash event");
  RequireOf(outcome, outcome.crashSeq == 2u, "duplicate guard must publish only one crash_seq (2)");
  RequireOf(outcome, outcome.crash.exception_code == EXCEPTION_STACK_OVERFLOW, "duplicate guard must record 0xC00000FD");
  RequireOf(outcome, outcome.msFromCrashToExit >= 1200u, "duplicate guard must hold for its ~1500 ms cap on the writing call");
  RequireOf(
    outcome,
    outcome.msFromCrashToExit < 2600u,
    "the second duplicate registration did not write the record and must not hold a second 1500 ms");
  RequireOf(outcome, outcome.exitCode == kChildFilterExitCode, "duplicate guard must continue to lower filter");
}

// ADR-0037 Completion Condition 3: CrashHookMode=0 leaves guard uninstalled and
// MoveStackOverflowGuardToFront no-ops; CrashHookMode=2 installs and runs guard.
void TestStackOverflowGuardModes0And2()
{
  const auto mode0 = RunChildForStackOverflowGuard("so-mode-0", HelperSettle::kNever, 0);
  RequireOf(mode0, mode0.exitCode == 0u, "CrashHookMode=0 must leave guard uninstalled and MoveToFront as kNotInstalled");
  RequireOf(mode0, !mode0.crashEventSignaled && mode0.crashSeq == 0u, "CrashHookMode=0 must not record");

  const auto mode2 = RunChildForStackOverflowGuard("so-mode-2", HelperSettle::kAfterCheck, 700);
  RequireOf(mode2, mode2.crashEventSignaled, "CrashHookMode=2 guard must signal crash event");
  RequireOf(mode2, mode2.crash.exception_code == EXCEPTION_STACK_OVERFLOW, "CrashHookMode=2 guard must record 0xC00000FD");
  RequireOf(mode2, mode2.aliveWhileHeld, "CrashHookMode=2 guard must hold until DUMP_SETTLED");
  RequireOf(mode2, mode2.otherHandlerCallsBeforeSettle == 0, "CrashHookMode=2 guard must run ahead of front VEH");
  RequireOf(mode2, mode2.otherHandlerCallsAtExit >= 1, "CrashHookMode=2 guard must release to front VEH after settle");
  RequireOf(
    mode2,
    mode2.exitCode != kChildFilterExitCode && mode2.exitCode != 0u,
    "CrashHookMode=2 heavy front VEH must exhaust the stack and end the process after settle");
}

// ADR-0037 Decision 2: VectoredHandler must skip EXCEPTION_STACK_OVERFLOW when
// StackOverflowFrontGuard is installed. If Frozen is thawed while the guard is
// still holding and then released, VectoredHandler runs later in the same
// dispatch and must NOT record the same stack overflow a second time.
void TestVectoredHandlerSkipsStackOverflowWhenGuardInstalled()
{
  char exePath[MAX_PATH]{};
  Require(GetModuleFileNameA(nullptr, exePath, MAX_PATH) != 0, "GetModuleFileNameA failed");
  const std::string goName = "Local\\skydiag_so_skip_veh_go_" + std::to_string(GetCurrentProcessId()) +
    "_" + std::to_string(GetTickCount64());
  HANDLE go = CreateEventA(nullptr, TRUE, FALSE, goName.c_str());
  Require(go != nullptr, "skip-VEH test barrier creation failed");

  std::string commandLine = std::string("\"") + exePath + "\" --child so-thawed-during-guard-hold " + goName;
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

  const auto cleanup = [&] {
    if (WaitForSingleObject(pi.hProcess, 0) != WAIT_OBJECT_0) {
      TerminateProcess(pi.hProcess, 1);
      WaitForSingleObject(pi.hProcess, 2000);
    }
    skydiag::helper::Detach(proc);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(go);
  };
  try {
    SetEvent(go);
    Require(WaitForSingleObject(proc.crashEvent, 10000) == WAIT_OBJECT_0, "SO child did not signal crashEvent");
    Require(proc.shm->header.crash_seq == 2u && proc.shm->header.crash.exception_code == EXCEPTION_STACK_OVERFLOW,
            "guard must record 0xC00000FD with crash_seq == 2");
    Require(WaitForSingleObject(pi.hProcess, 150) == WAIT_TIMEOUT,
            "main thread must be held inside StackOverflowFrontGuard");

    // Thaw Frozen while the guard still holds, reset crashEvent, then settle.
    // When the guard returns EXCEPTION_CONTINUE_SEARCH, VectoredHandler runs
    // for the same exception and must skip EXCEPTION_STACK_OVERFLOW.
    InterlockedAnd(
      reinterpret_cast<volatile LONG*>(&proc.shmWritable->header.state_flags),
      ~static_cast<LONG>(skydiag::kState_Frozen));
    ResetEvent(proc.crashEvent);
    SetEvent(proc.dumpSettledEvent);

    Require(WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0, "SO child did not exit after settle");
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    const auto seqAfter = proc.shm->header.crash_seq;
    const bool crashEventReSignaled = WaitForSingleObject(proc.crashEvent, 0) == WAIT_OBJECT_0;
    const bool frozenAfter = (proc.shm->header.state_flags & skydiag::kState_Frozen) != 0u;

    Require(exitCode == kChildFilterExitCode, "SO child must reach lower filter after guard releases");
    Require(seqAfter == 2u, "VectoredHandler must skip stack overflow and keep crash_seq == 2");
    Require(!crashEventReSignaled, "VectoredHandler must not re-signal crashEvent for stack overflow");
    Require(!frozenAfter, "VectoredHandler must not re-claim Frozen for stack overflow");
  } catch (...) {
    cleanup();
    throw;
  }
  cleanup();
}

// ADR-0037 Completion Condition 3: stack overflow while another incident owns
// kState_Frozen must neither overwrite the record nor hold.
void TestStackOverflowWhileFrozenNeitherRecordsNorHolds()
{
  char exePath[MAX_PATH]{};
  Require(GetModuleFileNameA(nullptr, exePath, MAX_PATH) != 0, "GetModuleFileNameA failed");
  const std::string goName = "Local\\skydiag_so_frozen_go_" + std::to_string(GetCurrentProcessId()) +
    "_" + std::to_string(GetTickCount64());
  const std::string go2Name = goName + "_go2";
  HANDLE go = CreateEventA(nullptr, TRUE, FALSE, goName.c_str());
  HANDLE go2 = CreateEventA(nullptr, TRUE, FALSE, go2Name.c_str());
  Require(go && go2, "frozen SO test barrier creation failed");

  std::string commandLine = std::string("\"") + exePath + "\" --child so-while-frozen " + goName;
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
    CloseHandle(go2);
    FailChildNotAttached(pi, go);
  }

  const auto cleanup = [&] {
    if (WaitForSingleObject(pi.hProcess, 0) != WAIT_OBJECT_0) {
      TerminateProcess(pi.hProcess, 1);
      WaitForSingleObject(pi.hProcess, 2000);
    }
    skydiag::helper::Detach(proc);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(go2);
    CloseHandle(go);
  };
  try {
    SetEvent(go);
    Require(WaitForSingleObject(proc.crashEvent, 10000) == WAIT_OBJECT_0, "first incident A did not publish");
    const auto seqA = proc.shm->header.crash_seq;
    const auto codeA = proc.shm->header.crash.exception_code;
    const auto writeIndexA = proc.shm->header.write_index;
    Require(codeA == EXCEPTION_ACCESS_VIOLATION, "first incident A must be ACCESS_VIOLATION");
    Require((proc.shm->header.state_flags & skydiag::kState_Frozen) != 0u, "first incident A must hold Frozen");

    // Keep Frozen held and DUMP_SETTLED unset, clear crashEvent, then trigger SO.
    ResetEvent(proc.crashEvent);
    const ULONGLONG triggerTick = GetTickCount64();
    SetEvent(go2);
    const DWORD waitExit = WaitForSingleObject(pi.hProcess, 15000);
    const DWORD elapsedMs = static_cast<DWORD>(GetTickCount64() - triggerTick);
    DWORD exitCode = 1;
    if (waitExit == WAIT_OBJECT_0) {
      GetExitCodeProcess(pi.hProcess, &exitCode);
    }
    const bool crashEventAfter = WaitForSingleObject(proc.crashEvent, 0) == WAIT_OBJECT_0;
    const auto seqAfter = proc.shm->header.crash_seq;
    const auto codeAfter = proc.shm->header.crash.exception_code;
    const auto writeIndexAfter = proc.shm->header.write_index;

    Require(waitExit == WAIT_OBJECT_0, "frozen SO child did not exit");
    Require(elapsedMs < 2000u, "stack overflow while Frozen must not hold waiting on DUMP_SETTLED");
    Require(!crashEventAfter, "stack overflow while Frozen must not signal crashEvent again");
    Require(seqAfter == seqA && codeAfter == codeA && writeIndexAfter == writeIndexA,
            "stack overflow while Frozen must not overwrite the existing crash record");
    Require(exitCode == kChildFilterExitCode, "frozen SO must reach lower filter");
  } catch (...) {
    cleanup();
    throw;
  }
  cleanup();
}

// ADR-0037 Completion Condition 3:
// - Handled stack overflow + _resetstkoflw + thaw -> second stack overflow on
//   the same thread is also recorded and held.
// - After helper resets and closes DUMP_SETTLED (helper gone), neither the
//   guard nor the ADR-0032 filter holds (plugin never retains the DUMP_SETTLED handle).
void TestHandledStackOverflowRecoveryAndHelperGoneNoHold()
{
  // 1) Same thread recovers via __except + _resetstkoflw, helper thaws, second SO records & holds.
  {
    char exePath[MAX_PATH]{};
    Require(GetModuleFileNameA(nullptr, exePath, MAX_PATH) != 0, "GetModuleFileNameA failed");
    const std::string goName = "Local\\skydiag_so_recover_go_" + std::to_string(GetCurrentProcessId()) +
      "_" + std::to_string(GetTickCount64());
    const std::string readyName = goName + "_ready";
    const std::string go2Name = goName + "_go2";
    HANDLE go = CreateEventA(nullptr, TRUE, FALSE, goName.c_str());
    HANDLE ready = CreateEventA(nullptr, TRUE, FALSE, readyName.c_str());
    HANDLE go2 = CreateEventA(nullptr, TRUE, FALSE, go2Name.c_str());
    Require(go && ready && go2, "recover SO test barrier creation failed");

    std::string commandLine = std::string("\"") + exePath + "\" --child so-handled-then-second " + goName;
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
      CloseHandle(ready);
      CloseHandle(go2);
      FailChildNotAttached(pi, go);
    }

    const auto cleanup = [&] {
      if (WaitForSingleObject(pi.hProcess, 0) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
      }
      skydiag::helper::Detach(proc);
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      CloseHandle(ready);
      CloseHandle(go2);
      CloseHandle(go);
    };
    try {
      SetEvent(go);
      Require(WaitForSingleObject(proc.crashEvent, 10000) == WAIT_OBJECT_0, "first handled SO did not signal crashEvent");
      const auto seq1 = proc.shm->header.crash_seq;
      Require(seq1 == 2u && proc.shm->header.crash.exception_code == EXCEPTION_STACK_OVERFLOW,
              "first handled SO must record 0xC00000FD");
      // Settle first dump and wait until child finishes __except + _resetstkoflw.
      SetEvent(proc.dumpSettledEvent);
      Require(WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0,
              "child did not signal ready after handling first SO and _resetstkoflw");
      // Simulate helper thawing Frozen after recovery and resetting events for next incident.
      ResetEvent(proc.dumpSettledEvent);
      ResetEvent(proc.crashEvent);
      InterlockedAnd(
        reinterpret_cast<volatile LONG*>(&proc.shmWritable->header.state_flags),
        ~static_cast<LONG>(skydiag::kState_Frozen));

      SetEvent(go2);
      Require(WaitForSingleObject(proc.crashEvent, 10000) == WAIT_OBJECT_0,
              "second SO on same thread after _resetstkoflw must also signal crashEvent");
      const bool aliveSecond = WaitForSingleObject(pi.hProcess, 500) == WAIT_TIMEOUT;
      const auto seq2 = proc.shm->header.crash_seq;
      const auto code2 = proc.shm->header.crash.exception_code;
      SetEvent(proc.dumpSettledEvent);
      const DWORD waitExit = WaitForSingleObject(pi.hProcess, 15000);
      DWORD exitCode = 1;
      if (waitExit == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &exitCode);
      }

      Require(seq2 == seq1 + 2u && code2 == EXCEPTION_STACK_OVERFLOW,
              "second SO after _resetstkoflw must publish a new 0xC00000FD crash_seq");
      Require(aliveSecond, "second SO must also be held until DUMP_SETTLED");
      Require(waitExit == WAIT_OBJECT_0 && exitCode == kChildFilterExitCode,
              "second SO must finish cleanly through the lower filter");
    } catch (...) {
      cleanup();
      throw;
    }
    cleanup();
  }

  // 2) After a first SO hold settles and helper resets + closes DUMP_SETTLED
  //    (helper gone), neither the guard (second SO) nor the ADR-0032 filter
  //    (unhandled AV) holds.
  for (const char* mode : { "so-then-helper-gone-so", "so-then-helper-gone-filter" }) {
    const bool isFilter = std::strcmp(mode, "so-then-helper-gone-filter") == 0;
    char exePath[MAX_PATH]{};
    Require(GetModuleFileNameA(nullptr, exePath, MAX_PATH) != 0, "GetModuleFileNameA failed");
    const std::string goName = "Local\\skydiag_so_gone_go_" + std::to_string(GetCurrentProcessId()) +
      "_" + std::to_string(GetTickCount64());
    const std::string readyName = goName + "_ready";
    const std::string go2Name = goName + "_go2";
    HANDLE go = CreateEventA(nullptr, TRUE, FALSE, goName.c_str());
    HANDLE ready = CreateEventA(nullptr, TRUE, FALSE, readyName.c_str());
    HANDLE go2 = CreateEventA(nullptr, TRUE, FALSE, go2Name.c_str());
    Require(go && ready && go2, "helper-gone test barrier creation failed");

    std::string commandLine = std::string("\"") + exePath + "\" --child " + mode + " " + goName;
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
      CloseHandle(ready);
      CloseHandle(go2);
      FailChildNotAttached(pi, go);
    }

    const auto cleanup = [&] {
      if (WaitForSingleObject(pi.hProcess, 0) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
      }
      skydiag::helper::Detach(proc);
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      CloseHandle(ready);
      CloseHandle(go2);
      CloseHandle(go);
    };
    try {
      SetEvent(go);
      Require(WaitForSingleObject(proc.crashEvent, 10000) == WAIT_OBJECT_0,
              "first SO before helper exit did not signal");
      // Hold briefly so the guard definitely opened DUMP_SETTLED, then settle
      // and wait for child handshake after __except + _resetstkoflw.
      Require(WaitForSingleObject(pi.hProcess, 150) == WAIT_TIMEOUT, "first SO must be held before settle");
      SetEvent(proc.dumpSettledEvent);
      Require(WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0,
              "child did not signal ready after handling first SO before helper exit");

      // Helper resets DUMP_SETTLED (as CrashCapture does when releasing an
      // incident) and then disappears (closes its handle). If the plugin kept
      // a handle to DUMP_SETTLED, the kernel event would stay alive in the
      // unsignaled state and the next crash would block for 5 seconds!
      ResetEvent(proc.dumpSettledEvent);
      CloseHandle(proc.dumpSettledEvent);
      proc.dumpSettledEvent = nullptr;
      ResetEvent(proc.crashEvent);
      InterlockedAnd(
        reinterpret_cast<volatile LONG*>(&proc.shmWritable->header.state_flags),
        ~static_cast<LONG>(skydiag::kState_Frozen));

      const ULONGLONG triggerTick = GetTickCount64();
      SetEvent(go2);
      const DWORD waitExit = WaitForSingleObject(pi.hProcess, 15000);
      const DWORD elapsedMs = static_cast<DWORD>(GetTickCount64() - triggerTick);
      DWORD exitCode = 1;
      if (waitExit == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &exitCode);
      }
      const bool unhandledAfter = proc.unhandledEvent && WaitForSingleObject(proc.unhandledEvent, 0) == WAIT_OBJECT_0;
      const auto codeAfter = proc.shm->header.crash.exception_code;

      Require(waitExit == WAIT_OBJECT_0 && exitCode == kChildFilterExitCode,
              "second crash after helper closed DUMP_SETTLED must reach lower filter");
      Require(elapsedMs < 2000u,
              "after helper resets and closes DUMP_SETTLED, neither guard nor ADR-0032 filter may hold (plugin must not retain event handle)");
      if (isFilter) {
        Require(codeAfter == EXCEPTION_ACCESS_VIOLATION && unhandledAfter,
                "ADR-0032 filter must still record and signal unhandled AV after helper closed DUMP_SETTLED");
      } else {
        Require(codeAfter == EXCEPTION_STACK_OVERFLOW,
                "guard must still record second SO after helper closed DUMP_SETTLED");
      }
    } catch (...) {
      cleanup();
      throw;
    }
    cleanup();
  }
}

// ADR-0037 Completion Condition 4: while a thread is held inside
// StackOverflowFrontGuard, another thread verifies that normal
// MoveStackOverflowGuardToFront skips (kSkippedBusy), then removes the active
// VEH registration via the test-only entry point; releasing DUMP_SETTLED must
// still let the thread exit the removed registration cleanly to the next VEH.
void TestRemovingActiveStackOverflowGuardRegistration()
{
  char exePath[MAX_PATH]{};
  Require(GetModuleFileNameA(nullptr, exePath, MAX_PATH) != 0, "GetModuleFileNameA failed");
  const std::string goName = "Local\\skydiag_so_active_rm_go_" + std::to_string(GetCurrentProcessId()) +
    "_" + std::to_string(GetTickCount64());
  const std::string go2Name = goName + "_go2";
  const std::string obsName = goName + "_obs";
  HANDLE go = CreateEventA(nullptr, TRUE, FALSE, goName.c_str());
  HANDLE go2 = CreateEventA(nullptr, TRUE, FALSE, go2Name.c_str());
  HANDLE obsMapping = CreateFileMappingA(
    INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(StackOverflowChildObs), obsName.c_str());
  Require(go && go2 && obsMapping, "active-removal test object creation failed");
  auto* obs = static_cast<StackOverflowChildObs*>(
    MapViewOfFile(obsMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(StackOverflowChildObs)));
  Require(obs != nullptr, "MapViewOfFile failed");
  std::memset(obs, 0, sizeof(StackOverflowChildObs));
  obs->skippedMoveResult = -1;
  obs->forceMoveResult = -1;

  std::string commandLine = std::string("\"") + exePath + "\" --child so-remove-while-active " + goName;
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
    UnmapViewOfFile(obs);
    CloseHandle(obsMapping);
    CloseHandle(go2);
    FailChildNotAttached(pi, go);
  }

  const auto cleanup = [&] {
    if (WaitForSingleObject(pi.hProcess, 0) != WAIT_OBJECT_0) {
      TerminateProcess(pi.hProcess, 1);
      WaitForSingleObject(pi.hProcess, 2000);
    }
    UnmapViewOfFile(obs);
    CloseHandle(obsMapping);
    skydiag::helper::Detach(proc);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(go2);
    CloseHandle(go);
  };
  try {
    SetEvent(go);
    Require(WaitForSingleObject(proc.crashEvent, 10000) == WAIT_OBJECT_0,
            "active-removal child did not signal crashEvent");
    // Confirm main thread is actively held inside StackOverflowFrontGuard.
    Require(WaitForSingleObject(pi.hProcess, 150) == WAIT_TIMEOUT,
            "main thread must be held inside StackOverflowFrontGuard");

    // Wake child's worker thread to attempt normal move (expect kSkippedBusy) and
    // force-move (removes active VEH registration while main thread is inside it).
    SetEvent(go2);
    bool workerFinished = false;
    for (int attempt = 0; attempt < 200; ++attempt) {
      if (InterlockedCompareExchange(&obs->workerDone, 0, 0) != 0) {
        workerFinished = true;
        break;
      }
      Sleep(10);
    }
    const bool aliveAfterRemoval = WaitForSingleObject(pi.hProcess, 100) == WAIT_TIMEOUT;
    const LONG otherBeforeSettle = InterlockedCompareExchange(&obs->otherHandlerCalls, 0, 0);
    const LONG skippedResult = InterlockedCompareExchange(&obs->skippedMoveResult, 0, 0);
    const LONG forcedResult = InterlockedCompareExchange(&obs->forceMoveResult, 0, 0);

    // Release DUMP_SETTLED so the main thread unwinds out of the removed VEH entry
    // and continues down the VEH chain to ChildStackOverflowMarkerVeh and ChildPreviousFilter.
    SetEvent(proc.dumpSettledEvent);
    const DWORD waitExit = WaitForSingleObject(pi.hProcess, 15000);
    DWORD exitCode = 1;
    if (waitExit == WAIT_OBJECT_0) {
      GetExitCodeProcess(pi.hProcess, &exitCode);
    }
    const LONG otherAtExit = InterlockedCompareExchange(&obs->otherHandlerCalls, 0, 0);
    const auto recordedCode = proc.shm->header.crash.exception_code;

    std::printf(
      "Active VEH removal test: workerFinished=%d skippedMove=%ld forceMove=%ld aliveAfterRemoval=%d "
      "otherAtExit=%ld exitCode=%lu\n",
      workerFinished ? 1 : 0,
      static_cast<long>(skippedResult),
      static_cast<long>(forcedResult),
      aliveAfterRemoval ? 1 : 0,
      static_cast<long>(otherAtExit),
      static_cast<unsigned long>(exitCode));

    Require(workerFinished, "worker thread did not complete active-removal sequence");
    Require(
      skippedResult == static_cast<LONG>(skydiag::plugin::StackOverflowGuardMoveResult::kSkippedBusy),
      "MoveStackOverflowGuardToFront must return kSkippedBusy while a thread is inside the guard");
    Require(
      forcedResult == static_cast<LONG>(skydiag::plugin::StackOverflowGuardMoveResult::kMoved),
      "force-move test helper must succeed in removing the active VEH registration");
    Require(aliveAfterRemoval && otherBeforeSettle == 0,
            "removing the active VEH registration must not disturb the held thread before DUMP_SETTLED");
    Require(recordedCode == EXCEPTION_STACK_OVERFLOW, "active-removal child must record 0xC00000FD");
    Require(otherAtExit == 1, "after DUMP_SETTLED, execution must proceed cleanly to the next VEH");
    Require(waitExit == WAIT_OBJECT_0 && exitCode == kChildFilterExitCode,
            "after removing active VEH registration, process must reach lower filter cleanly");
  } catch (...) {
    cleanup();
    throw;
  }
  cleanup();
}

void RunStackOverflowFrontGuardTests()
{
  TestStackOverflowWithFrontHeavyVehRecordsAndHolds();
  TestStackOverflowGuardHoldCapsAndNoHold();
  TestStackOverflowGuardIgnoresNonStackOverflowExceptions();
  TestStackOverflowGuardDuplicateRegistrationRecordsOnce();
  TestStackOverflowGuardModes0And2();
  TestVectoredHandlerSkipsStackOverflowWhenGuardInstalled();
  TestStackOverflowWhileFrozenNeitherRecordsNorHolds();
  TestHandledStackOverflowRecoveryAndHelperGoneNoHold();
  TestRemovingActiveStackOverflowGuardRegistration();
}

}  // namespace

int main(int argc, char** argv)
{
  if (argc == 5 && std::strcmp(argv[1], "--child-second-fault") == 0) {
    return RunSecondFaultChild(argv[2], argv[3], argv[4]);
  }
  if (argc == 4 && std::strcmp(argv[1], "--child") == 0) {
    return RunChild(argv[2], argv[3]);
  }
  try {
    if (argc == 2 && std::strcmp(argv[1], "--stack-overflow-guard") == 0) {
      MeasureExceptionContextBytes();
      RunStackOverflowFrontGuardTests();
      std::puts("stack overflow front guard tests passed");
      return 0;
    }
    if (argc == 3 && std::strcmp(argv[2], "--second-fault") == 0) {
      fault_tests::TestCandidateRule();
      fault_tests::TestCandidateTableOwnershipAndSaturation();
      fault_tests::TestCandidateTableConcurrentClaims();
      fault_tests::TestSlotClaimAndDropped();
      for (const char* mode : {"different-thread", "same-thread", "zero-hold", "no-helper", "range-unpublished",
                              "range-inside", "reentered", "stack-overflow", "thawed", "lookups-pending",
                              "short-hold", "state-settle"}) {
        std::printf("Second fault: %s\n", mode);
        TestSecondFaultChild(mode, argv[1]);
      }
      return 0;
    }
    Require(argc == 2, "usage: plugin_crash_handler_runtime_tests <path to fake CrashLogger.dll>");
    const HMODULE crashLogger = LoadLibraryA(argv[1]);
    Require(crashLogger != nullptr, "failed to load the fake CrashLogger.dll");
    g_probe = reinterpret_cast<ProbeFn>(GetProcAddress(crashLogger, "SkydiagFakeCrashLoggerProbe"));
    Require(g_probe != nullptr, "fake CrashLogger.dll has no probe export");
    g_callGameCode =
      reinterpret_cast<CallGameCodeFn>(GetProcAddress(crashLogger, "SkydiagFakeCrashLoggerCallGameCode"));
    Require(g_callGameCode != nullptr, "fake CrashLogger.dll has no game-code call export");

    // For the failure messages only (DescribeHost), so it may fail. Before the
    // plugin's handlers are installed (see the function).
    MeasureExceptionContextBytes();
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
    TestReleasedFaultIsStillReportedAsUnhandledWithoutHolding();
    TestStaleUnhandledSignalIsClearedWhenNextCrashPublishes(shm);
    TestHandledFaultIsNotReportedAsUnhandled();
    RunStackOverflowFrontGuardTests();
    std::puts("plugin crash handler runtime tests passed");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
