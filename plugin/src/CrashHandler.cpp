#include "SkyrimDiag/CrashHandler.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

#include "SkyrimDiag/Blackbox.h"
#include "SkyrimDiag/Hash.h"
#include "SkyrimDiag/SharedMemory.h"
#include "SkyrimDiagShared.h"

namespace skydiag::plugin {
namespace {

std::uint32_t g_crashHookMode = 1;
std::atomic<std::uint64_t> g_lastFirstChanceSignature{0};
std::atomic<std::uint64_t> g_lastFirstChanceQpc{0};
std::atomic<std::uint64_t> g_firstChanceWindowStartQpc{0};
std::atomic<std::uint32_t> g_firstChanceWindowCount{0};

constexpr std::uint32_t kFirstChancePerSecondLimit = 8;

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
    case 0xE06D7363:                     // MSVC C++ exception (SEH __CxxThrowException)
    case 0x406D1388:                     // SetThreadName via RaiseException (legacy)
    case EXCEPTION_BREAKPOINT:           // 0x80000003 — debugger breakpoint
    case EXCEPTION_SINGLE_STEP:          // 0x80000004 — single step (debugger)
    case 0x40010006:                     // OutputDebugStringA
    case 0x4001000A:                     // OutputDebugStringW
      return true;
    default:
      return false;
  }
}

std::string WideToUtf8(std::wstring_view text) noexcept
{
  if (text.empty()) {
    return {};
  }
  const int needed = WideCharToMultiByte(
    CP_UTF8,
    0,
    text.data(),
    static_cast<int>(text.size()),
    nullptr,
    0,
    nullptr,
    nullptr);
  if (needed <= 0) {
    return {};
  }
  std::string out(static_cast<std::size_t>(needed), '\0');
  const int written = WideCharToMultiByte(
    CP_UTF8,
    0,
    text.data(),
    static_cast<int>(text.size()),
    out.data(),
    needed,
    nullptr,
    nullptr);
  if (written <= 0) {
    return {};
  }
  out.resize(static_cast<std::size_t>(written));
  return out;
}

std::string ResolveExceptionModuleBasenameUtf8(void* address) noexcept
{
  if (!address) {
    return {};
  }

  HMODULE module{};
  if (!GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        static_cast<LPCWSTR>(address),
        &module) ||
      !module) {
    return {};
  }

  wchar_t pathBuf[MAX_PATH]{};
  const DWORD len = GetModuleFileNameW(module, pathBuf, static_cast<DWORD>(std::size(pathBuf)));
  if (len == 0 || len >= std::size(pathBuf)) {
    return {};
  }

  const std::filesystem::path path(std::wstring_view(pathBuf, len));
  return WideToUtf8(path.filename().wstring());
}

std::uint32_t BucketExceptionAddress(void* address) noexcept
{
  const auto raw = reinterpret_cast<std::uintptr_t>(address);
  return static_cast<std::uint32_t>((raw >> 4) & 0xFFFFFFFFu);
}

std::uint64_t HashFirstChanceSignature(
  DWORD code,
  std::uint32_t addressBucket,
  std::string_view moduleBasenameUtf8) noexcept
{
  std::uint64_t signature = static_cast<std::uint64_t>(code);
  signature ^= (static_cast<std::uint64_t>(addressBucket) << 32);
  signature ^= skydiag::hash::Fnv1a64(moduleBasenameUtf8);
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

LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS* ep) noexcept
{
  auto* shm = GetShared();
  if (!shm || !ep || !ep->ExceptionRecord || !ep->ContextRecord) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (ShouldEmitFirstChanceTelemetry(ep->ExceptionRecord)) {
    const auto qpcNow = QpcNow();
    const auto addressBucket = BucketExceptionAddress(ep->ExceptionRecord->ExceptionAddress);
    const auto moduleBasenameUtf8 = ResolveExceptionModuleBasenameUtf8(ep->ExceptionRecord->ExceptionAddress);
    const auto signature = HashFirstChanceSignature(code, addressBucket, moduleBasenameUtf8);
    if (ConsumeFirstChanceTelemetryBudget(signature, qpcNow)) {
      PushFirstChanceExceptionEvent(code, addressBucket, moduleBasenameUtf8);
    }
  }
  if (!ShouldRecordException(code)) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  shm->header.crash.exception_code = code;
  shm->header.crash.exception_addr = reinterpret_cast<std::uint64_t>(ep->ExceptionRecord->ExceptionAddress);
  shm->header.crash.faulting_tid = GetCurrentThreadId();

  std::memcpy(&shm->header.crash.exception_record, ep->ExceptionRecord, sizeof(EXCEPTION_RECORD));
  std::memcpy(&shm->header.crash.context, ep->ContextRecord, sizeof(CONTEXT));
  (void)InterlockedIncrement(reinterpret_cast<volatile LONG*>(&shm->header.crash_seq));

  skydiag::EventPayload p{};
  p.a = shm->header.crash.exception_code;
  p.b = shm->header.crash.exception_addr;
  PushEventAlways(skydiag::EventType::kCrash, p, sizeof(p));

  InterlockedOr(
    reinterpret_cast<volatile LONG*>(&shm->header.state_flags),
    static_cast<LONG>(skydiag::kState_Frozen));

  if (HANDLE ev = GetCrashEvent()) {
    SetEvent(ev);
  }

  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

bool InstallCrashHandler(std::uint32_t crashHookMode)
{
  g_crashHookMode = crashHookMode;

  // First=1 to run early, but we never consume the exception.
  PVOID h = AddVectoredExceptionHandler(/*First=*/1, VectoredHandler);
  return h != nullptr;
}

}  // namespace skydiag::plugin
