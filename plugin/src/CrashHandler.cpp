#include "SkyrimDiag/CrashHandler.h"

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <atomic>
#include <bit>
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
  return true;
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
    if (ShouldSuppressNestedCrashLoggerException(
          IsCrashCaptureFrozen(shm),
          exceptionAddress,
          g_crashLoggerRange.Load(),
          g_crashLoggerSseRange.Load())) {
      // CrashLogger probes objects with handled exceptions while producing its
      // report. Once a crash candidate is already frozen, those probes must not
      // replace the original crash context.
      return EXCEPTION_CONTINUE_SEARCH;
    }

    // The fatal path deliberately performs only fixed-size shared-memory
    // writes and kernel signaling. Module/path resolution and std::string /
    // std::filesystem telemetry are unsafe with a corrupt heap or low stack.
    if (!TryPublishCrashRecord(shm, ep, code)) {
      return EXCEPTION_CONTINUE_SEARCH;
    }

    skydiag::EventPayload p{};
    p.a = code;
    p.b = reinterpret_cast<std::uint64_t>(ep->ExceptionRecord->ExceptionAddress);
    PushEventAlways(skydiag::EventType::kCrash, p, sizeof(p));

    if (HANDLE ev = GetCrashEvent()) {
      SetEvent(ev);
    }
    return EXCEPTION_CONTINUE_SEARCH;
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
  return h != nullptr;
}

}  // namespace skydiag::plugin
