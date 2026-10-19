#include "SkyrimDiag/CrashHandler.h"

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <limits>

#include "SkyrimDiag/Blackbox.h"
#include "SkyrimDiag/SharedMemory.h"
#include "SkyrimDiagCrashCodes.h"
#include "SkyrimDiagShared.h"

namespace skydiag::plugin {
namespace {

std::uint32_t g_crashHookMode = 1;
std::atomic<std::uint64_t> g_lastFirstChanceSignature{0};
std::atomic<std::uint64_t> g_lastFirstChanceQpc{0};
std::atomic<std::uint64_t> g_firstChanceWindowStartQpc{0};
std::atomic<std::uint32_t> g_firstChanceWindowCount{0};
// A module range that transitions from empty to published exactly once.
//
// The crash handler reads these ranges from an arbitrary faulting thread while
// the SKSE lifecycle may still be publishing them. Storing `end` before
// releasing `begin` means a reader either sees begin == 0 (treated as "no
// range", the pre-publication behavior) or sees both halves of a complete
// range. A published range is never rewritten, so no torn update is possible.
struct PublishOnceModuleRange
{
  std::atomic<std::uintptr_t> begin{0};
  std::atomic<std::uintptr_t> end{0};

  CrashHandlerModuleRange Load() const noexcept
  {
    const auto loadedBegin = begin.load(std::memory_order_acquire);
    if (loadedBegin == 0u) {
      return {};
    }
    return { loadedBegin, end.load(std::memory_order_relaxed) };
  }

  bool IsPublished() const noexcept
  {
    return begin.load(std::memory_order_acquire) != 0u;
  }

  void PublishOnce(CrashHandlerModuleRange range) noexcept
  {
    if (range.begin == 0u || range.end <= range.begin) {
      return;
    }
    if (IsPublished()) {
      return;
    }
    end.store(range.end, std::memory_order_relaxed);
    begin.store(range.begin, std::memory_order_release);
  }
};

PublishOnceModuleRange g_crashLoggerRange{};
PublishOnceModuleRange g_crashLoggerSseRange{};

constexpr std::uint32_t kFirstChancePerSecondLimit = 8;

// A thread's latest unrecorded breakpoint or C++ throw (see
// ShouldKeepUnrecordedException), one slot per recently throwing thread
// (SelectKeptExceptionSlot). Static storage, so the handler never puts a
// CONTEXT on a possibly exhausted stack. `busy` is a try-lock: a thread that
// finds it taken just skips; tid and qpc are read without it to pick a slot.
struct KeptException
{
  std::atomic<LONG> busy{0};
  std::atomic<DWORD> tid{0};
  std::atomic<std::uint64_t> qpc{0};
  EXCEPTION_RECORD record{};
  CONTEXT context{};
  // Where the dispatcher keeps this exception's CONTEXT on the thread's stack;
  // it stays there, unchanged, while the exception is still being dispatched
  // (IsCrashLoggerReportingKeptException).
  std::uintptr_t dispatchContextAddress = 0;
};

KeptException g_keptExceptions[kKeptExceptionSlotCount]{};

// When the last crash record was published. The helper hands the incident
// slot back while it checks a captured fault (ADR-0017), so CrashLogger may
// probe on a thread whose kept exception is older than the crash it is
// reporting; that one is not it.
std::atomic<std::uint64_t> g_lastPublishedCrashQpc{0};

// CrashLogger writes its report within seconds; this only bounds how stale a
// kept exception may be when it is matched to a CrashLogger fault.
constexpr std::uint64_t kKeptExceptionMaxAgeSeconds = 60;

// terminate() follows an unhandled throw at once (a noexcept frame or a
// catch(...) that terminates), so a kept throw older than this was handled
// and is not why abort() runs.
constexpr std::uint64_t kAbortKeptExceptionMaxAgeSeconds = 5;

// Bounds the stack scan for CrashLogger frames (see
// SelectCrashLoggerStackScanWindow).
constexpr std::uintptr_t kCrashLoggerStackScanMaxBytes = 64u * 1024u;

// The filter and SIGABRT handler that were installed before ours; both are
// chained so this never changes what happens after it records the crash.
std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> g_previousUnhandledExceptionFilter{ nullptr };
using AbortSignalHandlerFn = void(__cdecl*)(int);
std::atomic<AbortSignalHandlerFn> g_previousAbortSignalHandler{ nullptr };

CrashHandlerModuleRange QueryLoadedModuleRange(const wchar_t* moduleName) noexcept
{
  const HMODULE module = GetModuleHandleW(moduleName);
  if (!module) {
    return {};
  }

  MODULEINFO info{};
  if (!GetModuleInformation(
        GetCurrentProcess(),
        module,
        &info,
        static_cast<DWORD>(sizeof(info))) ||
      !info.lpBaseOfDll || info.SizeOfImage == 0u) {
    return {};
  }

  const auto begin = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
  const auto size = static_cast<std::uintptr_t>(info.SizeOfImage);
  const auto maxAddress = std::numeric_limits<std::uintptr_t>::max();
  if (size > maxAddress - begin) {
    return { begin, maxAddress };
  }
  return { begin, begin + size };
}

bool IsCrashCaptureFrozen(const skydiag::SharedLayout* shm) noexcept
{
  if (!shm) {
    return false;
  }
  auto* const flags = reinterpret_cast<volatile LONG*>(
    const_cast<volatile std::uint32_t*>(&shm->header.state_flags));
  const auto value = static_cast<std::uint32_t>(InterlockedCompareExchange(flags, 0, 0));
  return (value & skydiag::kState_Frozen) != 0u;
}

std::uint64_t QpcNow() noexcept
{
  LARGE_INTEGER li{};
  QueryPerformanceCounter(&li);
  return static_cast<std::uint64_t>(li.QuadPart);
}

bool IsBenignFirstChanceException(DWORD code) noexcept
{
  // Benign exceptions: first-chance C++ SEH, OutputDebugString, thread naming,
  // breakpoints in debuggers, etc.
  switch (code) {
    case skydiag::kStatusCppException:         // MSVC C++ exception (SEH __CxxThrowException)
    case skydiag::kStatusThreadNameLegacy:     // SetThreadName via RaiseException (legacy)
    case skydiag::kStatusBreakpoint:           // debugger breakpoint
    case skydiag::kStatusSingleStep:           // single step (debugger)
    case skydiag::kStatusOutputDebugStringA:   // OutputDebugStringA
    case skydiag::kStatusOutputDebugStringW:   // OutputDebugStringW
      return true;
    default:
      return false;
  }
}

std::uint32_t BucketExceptionAddress(void* address) noexcept
{
  const auto raw = reinterpret_cast<std::uintptr_t>(address);
  return static_cast<std::uint32_t>((raw >> 4) & 0xFFFFFFFFu);
}

std::uint64_t HashFirstChanceSignature(
  DWORD code,
  std::uint32_t addressBucket) noexcept
{
  std::uint64_t signature = static_cast<std::uint64_t>(code);
  signature ^= (static_cast<std::uint64_t>(addressBucket) << 32);
  return signature;
}

bool ConsumeFirstChanceTelemetryBudget(std::uint64_t signature, std::uint64_t nowQpc) noexcept
{
  auto* shm = GetShared();
  const std::uint64_t qpcFreq = (shm && shm->header.qpc_freq != 0u) ? shm->header.qpc_freq : 10000000ull;
  const std::uint64_t dedupeWindowQpc = std::max<std::uint64_t>(1ull, qpcFreq / 2ull);

  const auto lastSignature = g_lastFirstChanceSignature.load(std::memory_order_relaxed);
  const auto lastQpc = g_lastFirstChanceQpc.load(std::memory_order_relaxed);
  if (lastSignature == signature && nowQpc >= lastQpc && (nowQpc - lastQpc) <= dedupeWindowQpc) {
    return false;
  }
  g_lastFirstChanceSignature.store(signature, std::memory_order_relaxed);
  g_lastFirstChanceQpc.store(nowQpc, std::memory_order_relaxed);

  auto windowStart = g_firstChanceWindowStartQpc.load(std::memory_order_relaxed);
  if (windowStart == 0u || nowQpc < windowStart || (nowQpc - windowStart) > qpcFreq) {
    g_firstChanceWindowStartQpc.store(nowQpc, std::memory_order_relaxed);
    g_firstChanceWindowCount.store(1u, std::memory_order_relaxed);
    return true;
  }

  const auto count = g_firstChanceWindowCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
  return count <= kFirstChancePerSecondLimit;
}

bool ShouldEmitFirstChanceTelemetry(const EXCEPTION_RECORD* record) noexcept
{
  return record != nullptr && !IsBenignFirstChanceException(record->ExceptionCode);
}

bool IsFatalExceptionCode(DWORD code) noexcept
{
  if (IsBenignFirstChanceException(code)) {
    return false;
  }
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_FLT_DENORMAL_OPERAND:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_OVERFLOW:
    case EXCEPTION_FLT_STACK_CHECK:
    case EXCEPTION_FLT_UNDERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
    case EXCEPTION_INVALID_DISPOSITION:
    case 0xC0000374:  // STATUS_HEAP_CORRUPTION
    case 0xC0000409:  // STATUS_STACK_BUFFER_OVERRUN
      return true;
    default:
      return false;
  }
}

// Mode 1 (recommended): fatal exceptions only.
// Mode 2 (unsafe): record all exceptions, including handled first-chance exceptions.
bool ShouldRecordException(DWORD code) noexcept
{
  switch (g_crashHookMode) {
    case 2:  // All exceptions (legacy behavior; can false-trigger).
      return true;
    case 1:  // Fatal exceptions only.
      return IsFatalExceptionCode(code);
    default:  // Off / unknown.
      return false;
  }
}

bool TryPublishCrashRecord(
  skydiag::SharedLayout* shm,
  const EXCEPTION_POINTERS* ep,
  DWORD code) noexcept
{
  if (!shm || !ep || !ep->ExceptionRecord || !ep->ContextRecord) {
    return false;
  }

  // This CAS is the protocol-v4 incident linearization point. Once claimed,
  // every later exception observes Frozen and leaves this record untouched
  // until the helper explicitly ACKs/rearms by clearing Frozen.
  if (!skydiag::TryClaimCrashIncidentOwnership(&shm->header.state_flags)) {
    return false;
  }

  auto* const sequence = reinterpret_cast<volatile LONG*>(&shm->header.crash_seq);
  const LONG observed = InterlockedCompareExchange(sequence, 0, 0);
  if ((observed & 1L) != 0L) {
    // Frozen ownership says no other writer can legitimately be active. Keep
    // the incident fail-closed rather than clearing ownership and allowing an
    // inconsistent CrashInfo record to be replaced.
    return false;
  }

  const auto writingBits = static_cast<std::uint32_t>(observed) + 1u;
  const LONG writing = std::bit_cast<LONG>(writingBits);
  if (InterlockedCompareExchange(sequence, writing, observed) != observed) {
    return false;
  }

  auto& crash = shm->header.crash;
  crash.exception_code = code;
  crash.exception_addr = reinterpret_cast<std::uint64_t>(ep->ExceptionRecord->ExceptionAddress);
  crash.faulting_tid = GetCurrentThreadId();
  std::memcpy(&crash.exception_record, ep->ExceptionRecord, sizeof(EXCEPTION_RECORD));
  std::memcpy(&crash.context, ep->ContextRecord, sizeof(CONTEXT));

  // Publish the complete record before the even sequence and event become
  // observable in the helper process.
  MemoryBarrier();
  const auto committedBits = static_cast<std::uint32_t>(observed) + 2u;
  LONG committed = std::bit_cast<LONG>(committedBits);
  if (committed == 0L) {
    committed = 2L;
  }
  InterlockedExchange(sequence, committed);
  g_lastPublishedCrashQpc.store(QpcNow(), std::memory_order_relaxed);
  return true;
}

std::uint64_t KeptExceptionMaxAgeQpc(const skydiag::SharedLayout* shm) noexcept
{
  const std::uint64_t qpcFreq = (shm && shm->header.qpc_freq != 0u) ? shm->header.qpc_freq : 10000000ull;
  return kKeptExceptionMaxAgeSeconds * qpcFreq;
}

bool IsKeptExceptionRecent(const skydiag::SharedLayout* shm, std::uint64_t keptQpc, std::uint64_t nowQpc) noexcept
{
  return IsKeptExceptionSlotRecent(keptQpc, nowQpc, KeptExceptionMaxAgeQpc(shm));
}

// The slot that holds this thread's kept exception, if any.
KeptException* FindKeptException(DWORD tid) noexcept
{
  for (auto& slot : g_keptExceptions) {
    if (slot.tid.load(std::memory_order_relaxed) == tid) {
      return &slot;
    }
  }
  return nullptr;
}

void KeepUnrecordedException(const skydiag::SharedLayout* shm, const EXCEPTION_POINTERS* ep) noexcept
{
  const auto& record = *ep->ExceptionRecord;
  const auto throwImageBase = record.NumberParameters >= 4u
    ? static_cast<std::uintptr_t>(record.ExceptionInformation[3])
    : std::uintptr_t{0};
  if (!ShouldKeepUnrecordedException(
        record.ExceptionCode,
        record.NumberParameters,
        throwImageBase,
        g_crashLoggerRange.Load(),
        g_crashLoggerSseRange.Load())) {
    return;
  }

  const DWORD tid = GetCurrentThreadId();
  const auto nowQpc = QpcNow();
  KeptExceptionSlotView views[kKeptExceptionSlotCount]{};
  for (std::size_t i = 0; i < kKeptExceptionSlotCount; ++i) {
    views[i].tid = g_keptExceptions[i].tid.load(std::memory_order_relaxed);
    views[i].qpc = g_keptExceptions[i].qpc.load(std::memory_order_relaxed);
  }
  const std::size_t slot = SelectKeptExceptionSlot(
    views, kKeptExceptionSlotCount, tid, nowQpc, KeptExceptionMaxAgeQpc(shm));
  auto& kept = g_keptExceptions[slot];
  if (kept.busy.exchange(1, std::memory_order_acquire) != 0) {
    return;
  }
  const DWORD keptTid = kept.tid.load(std::memory_order_relaxed);
  if (keptTid != views[slot].tid) {
    // Another thread took this slot after it was picked; its exception stays.
    kept.busy.store(0, std::memory_order_release);
    return;
  }
  if (ShouldReplaceKeptException(
        keptTid != 0u,
        keptTid == tid,
        IsKeptExceptionRecent(shm, kept.qpc.load(std::memory_order_relaxed), nowQpc),
        kept.record.ExceptionCode,
        record.ExceptionCode)) {
    kept.tid.store(tid, std::memory_order_relaxed);
    kept.qpc.store(nowQpc, std::memory_order_relaxed);
    std::memcpy(&kept.record, ep->ExceptionRecord, sizeof(EXCEPTION_RECORD));
    std::memcpy(&kept.context, ep->ContextRecord, sizeof(CONTEXT));
    kept.dispatchContextAddress = reinterpret_cast<std::uintptr_t>(ep->ContextRecord);
  }
  kept.busy.store(0, std::memory_order_release);
}

// Kept recently enough and after the last published crash (see
// g_lastPublishedCrashQpc).
bool IsKeptExceptionCurrent(std::uint64_t keptQpc, std::uint64_t maxAgeQpc) noexcept
{
  return IsKeptExceptionSlotRecent(keptQpc, QpcNow(), maxAgeQpc) &&
         keptQpc > g_lastPublishedCrashQpc.load(std::memory_order_relaxed);
}

// Records this thread's kept exception when `accept(kept, keptTid)` agrees,
// then clears the slot. Returns false when there is none, its slot is busy,
// `accept` refuses, or the incident is already owned.
template <class Accept>
bool TryPublishThreadKeptException(
  skydiag::SharedLayout* shm,
  Accept accept,
  DWORD* outCode,
  std::uint64_t* outAddress) noexcept
{
  const DWORD tid = GetCurrentThreadId();
  auto* const slot = FindKeptException(tid);
  if (!slot) {
    return false;
  }
  auto& kept = *slot;
  if (kept.busy.exchange(1, std::memory_order_acquire) != 0) {
    return false;
  }
  bool published = false;
  const DWORD keptTid = kept.tid.load(std::memory_order_relaxed);
  if (accept(kept, keptTid)) {
    EXCEPTION_POINTERS keptPointers{ &kept.record, &kept.context };
    published = TryPublishCrashRecord(shm, &keptPointers, kept.record.ExceptionCode);
    if (published) {
      *outCode = kept.record.ExceptionCode;
      *outAddress = reinterpret_cast<std::uint64_t>(kept.record.ExceptionAddress);
      kept.tid.store(0, std::memory_order_relaxed);
      kept.qpc.store(0, std::memory_order_relaxed);
    }
  }
  kept.busy.store(0, std::memory_order_release);
  return published;
}

// Handles a fatal-code exception inside CrashLogger (see ClassifyCrashLoggerFault):
// records the kept exception CrashLogger is reporting, if any. Returns false
// when the exception should be ignored. On success the incident is frozen.
bool TryPublishKeptExceptionForCrashLoggerFault(
  skydiag::SharedLayout* shm,
  std::uintptr_t exceptionAddress,
  DWORD* outCode,
  std::uint64_t* outAddress) noexcept
{
  const DWORD tid = GetCurrentThreadId();
  return TryPublishThreadKeptException(
    shm,
    [&](const KeptException& kept, DWORD keptTid) noexcept {
      return ClassifyCrashLoggerFault(
               IsCrashCaptureFrozen(shm),
               exceptionAddress,
               g_crashLoggerRange.Load(),
               g_crashLoggerSseRange.Load(),
               keptTid != 0u,
               keptTid == tid,
               IsKeptExceptionCurrent(kept.qpc.load(std::memory_order_relaxed), KeptExceptionMaxAgeQpc(shm))) ==
             CrashLoggerFaultAction::kRecordKeptException;
    },
    outCode,
    outAddress);
}

// Whether the fault is nested in the dispatch of this thread's kept exception
// with CrashLogger between the two (SelectCrashLoggerStackScanWindow): the
// kept exception's CONTEXT is still in place above the fault, and a CrashLogger
// code address is on the stack between them. Reads only the current thread's
// stack above the fault, which is committed, and at most a bounded amount.
bool IsCrashLoggerReportingKeptException(std::uintptr_t faultSp, const KeptException& kept) noexcept
{
  const auto crashLoggerRange = g_crashLoggerRange.Load();
  const auto crashLoggerSseRange = g_crashLoggerSseRange.Load();
  if (crashLoggerRange.begin == 0u && crashLoggerSseRange.begin == 0u) {
    return false;
  }
  const auto* const tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
  const auto stackLow = reinterpret_cast<std::uintptr_t>(tib->StackLimit);
  const auto stackHigh = reinterpret_cast<std::uintptr_t>(tib->StackBase);
  const auto contextAddress = kept.dispatchContextAddress;
  const auto window = SelectCrashLoggerStackScanWindow(
    faultSp, contextAddress, stackLow, stackHigh, kCrashLoggerStackScanMaxBytes);
  if (window.begin == 0u || contextAddress > stackHigh - sizeof(CONTEXT)) {
    return false;
  }
  // Once the exception was handled, its stack was unwound and reused.
  const auto* const dispatchContext = reinterpret_cast<const volatile CONTEXT*>(contextAddress);
  if (dispatchContext->Rip != kept.context.Rip || dispatchContext->Rsp != kept.context.Rsp) {
    return false;
  }
  for (auto at = window.begin; at < window.end; at += sizeof(std::uintptr_t)) {
    const auto value = *reinterpret_cast<const volatile std::uintptr_t*>(at);
    if (IsInCrashLoggerModule(value, crashLoggerRange, crashLoggerSseRange)) {
      return true;
    }
  }
  return false;
}

// Handles a fatal-code exception outside CrashLogger. When CrashLogger is
// reporting this thread's kept exception and its report faults in game or
// runtime code, that fault is a side effect: record the kept exception
// instead. Returns false otherwise, and the fault itself is then recorded as
// before; the scan never makes a fault ignored.
bool TryPublishKeptExceptionForCrashLoggerSideFault(
  skydiag::SharedLayout* shm,
  const CONTEXT* faultContext,
  DWORD* outCode,
  std::uint64_t* outAddress) noexcept
{
  if (g_crashHookMode != 1) {
    return false;
  }
  return TryPublishThreadKeptException(
    shm,
    [&](const KeptException& kept, DWORD keptTid) noexcept {
      return keptTid == GetCurrentThreadId() &&
             IsKeptExceptionCurrent(kept.qpc.load(std::memory_order_relaxed), KeptExceptionMaxAgeQpc(shm)) &&
             IsCrashLoggerReportingKeptException(static_cast<std::uintptr_t>(faultContext->Rsp), kept);
    },
    outCode,
    outAddress);
}

// Signals the helper about a crash record published outside the vectored
// handler. Same fixed-size, allocation-free steps as the VEH fatal path.
void SignalPublishedCrash(DWORD code, std::uint64_t address) noexcept
{
  skydiag::EventPayload p{};
  p.a = code;
  p.b = address;
  PushEventAlways(skydiag::EventType::kCrash, p, sizeof(p));
  if (HANDLE ev = GetCrashEvent()) {
    SetEvent(ev);
  }
}

LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS* ep) noexcept
{
  auto* shm = GetShared();
  if (!shm || !ep || !ep->ExceptionRecord || !ep->ContextRecord) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (ShouldRecordException(code)) {
    const auto exceptionAddress =
      reinterpret_cast<std::uintptr_t>(ep->ExceptionRecord->ExceptionAddress);

    // The fatal path deliberately performs only fixed-size shared-memory
    // writes and kernel signaling. Module/path resolution and std::string /
    // std::filesystem telemetry are unsafe with a corrupt heap or low stack.
    DWORD recordedCode = code;
    auto recordedAddress = static_cast<std::uint64_t>(exceptionAddress);
    if (IsInCrashLoggerModule(exceptionAddress, g_crashLoggerRange.Load(), g_crashLoggerSseRange.Load())) {
      // CrashLogger's own probe faults are never the crash (see
      // ClassifyCrashLoggerFault): record the exception it is reporting if
      // one was kept, and otherwise leave the probe to CrashLogger's handler.
      if (!TryPublishKeptExceptionForCrashLoggerFault(shm, exceptionAddress, &recordedCode, &recordedAddress)) {
        return EXCEPTION_CONTINUE_SEARCH;
      }
    } else if (
      !TryPublishKeptExceptionForCrashLoggerSideFault(shm, ep->ContextRecord, &recordedCode, &recordedAddress) &&
      !TryPublishCrashRecord(shm, ep, code)) {
      return EXCEPTION_CONTINUE_SEARCH;
    }

    skydiag::EventPayload p{};
    p.a = recordedCode;
    p.b = recordedAddress;
    PushEventAlways(skydiag::EventType::kCrash, p, sizeof(p));

    if (HANDLE ev = GetCrashEvent()) {
      SetEvent(ev);
    }
    return EXCEPTION_CONTINUE_SEARCH;
  }

  if (g_crashHookMode == 1) {
    KeepUnrecordedException(shm, ep);
  }

  if (ShouldEmitFirstChanceTelemetry(ep->ExceptionRecord)) {
    const auto qpcNow = QpcNow();
    const auto addressBucket = BucketExceptionAddress(ep->ExceptionRecord->ExceptionAddress);
    const auto signature = HashFirstChanceSignature(code, addressBucket);
    if (ConsumeFirstChanceTelemetryBudget(signature, qpcNow)) {
      // VEH records only numeric telemetry. Loader/module/path lookup is
      // intentionally deferred to helper/analyzer processing outside the
      // faulting process.
      PushFirstChanceExceptionEvent(code, addressBucket, {});
    }
  }

  return EXCEPTION_CONTINUE_SEARCH;
}

// Top-level filter, chained in front of the previous one. Windows calls it
// only for an exception that no frame handler took, right before the process
// ends. In fatal-only mode the vectored handler skips C++ throws, breakpoints
// and other non-fatal codes because they are usually handled; when one gets
// here it was not, so this records it. Without CrashLogger faulting on the
// same thread (TryPublishKeptExceptionForCrashLoggerFault), nothing recorded
// it: the CRT's filter calls terminate() and the process ends by __fastfail.
LONG WINAPI UnhandledExceptionFilterHook(EXCEPTION_POINTERS* ep) noexcept
{
  auto* shm = GetShared();
  if (shm && ep && ep->ExceptionRecord && ep->ContextRecord && g_crashHookMode == 1) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    // Fatal codes were offered to the vectored handler at first chance.
    if (!ShouldRecordException(code) && TryPublishCrashRecord(shm, ep, code)) {
      SignalPublishedCrash(code, reinterpret_cast<std::uint64_t>(ep->ExceptionRecord->ExceptionAddress));
    }
  }
  const auto previous = g_previousUnhandledExceptionFilter.load(std::memory_order_acquire);
  return previous ? previous(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// abort() raises SIGABRT before it ends the process. A throw that leaves a
// noexcept function, or one caught only to call terminate(), never reaches the
// top-level filter: terminate() and abort() run right after it is thrown.
// Record this thread's kept throw then, if it is that recent.
void __cdecl AbortSignalHandler(int signalNumber) noexcept
{
  if (auto* shm = GetShared(); shm && g_crashHookMode == 1) {
    const std::uint64_t qpcFreq = shm->header.qpc_freq != 0u ? shm->header.qpc_freq : 10000000ull;
    const std::uint64_t maxAgeQpc = kAbortKeptExceptionMaxAgeSeconds * qpcFreq;
    DWORD code = 0;
    std::uint64_t address = 0;
    if (TryPublishThreadKeptException(
          shm,
          [&](const KeptException& kept, DWORD keptTid) noexcept {
            return keptTid == GetCurrentThreadId() &&
                   IsKeptExceptionCurrent(kept.qpc.load(std::memory_order_relaxed), maxAgeQpc);
          },
          &code,
          &address)) {
      SignalPublishedCrash(code, address);
    }
  }
  const auto previous = g_previousAbortSignalHandler.load(std::memory_order_acquire);
  if (previous != SIG_DFL && previous != SIG_IGN && previous != SIG_ERR) {
    previous(signalNumber);
  }
}

}  // namespace

void RefreshCrashLoggerModuleRanges() noexcept
{
  // Safe context only. GetModuleHandleW takes the loader lock, so this must
  // never run from VectoredHandler.
  g_crashLoggerRange.PublishOnce(QueryLoadedModuleRange(L"CrashLogger.dll"));
  g_crashLoggerSseRange.PublishOnce(QueryLoadedModuleRange(L"CrashLoggerSSE.dll"));
}

bool InstallCrashHandler(std::uint32_t crashHookMode)
{
  g_crashHookMode = crashHookMode;
  // CrashLogger may not be resident yet; the SKSE lifecycle refreshes these
  // ranges again once the remaining plugins have loaded.
  RefreshCrashLoggerModuleRanges();

  // First=1 to run early, but we never consume the exception.
  PVOID h = AddVectoredExceptionHandler(/*First=*/1, VectoredHandler);

  // Fatal-only mode leaves unhandled C++ throws and breakpoints to these two;
  // mode 2 already records every exception at first chance. Each is installed
  // once and chains the handler it replaces (installing twice would chain to
  // itself).
  static std::atomic<bool> s_lastChanceHandlersInstalled{ false };
  if (h != nullptr && crashHookMode == 1 && !s_lastChanceHandlersInstalled.exchange(true)) {
    g_previousUnhandledExceptionFilter.store(
      SetUnhandledExceptionFilter(UnhandledExceptionFilterHook), std::memory_order_release);
    const AbortSignalHandlerFn previousAbort = std::signal(SIGABRT, AbortSignalHandler);
    g_previousAbortSignalHandler.store(previousAbort == SIG_ERR ? nullptr : previousAbort, std::memory_order_release);
  }
  return h != nullptr;
}

}  // namespace skydiag::plugin
