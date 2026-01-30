#include "SkyrimDiag/CrashHandler.h"

#include <Windows.h>

#include <cstdint>
#include <cstring>

#include "SkyrimDiag/Blackbox.h"
#include "SkyrimDiag/SharedMemory.h"
#include "SkyrimDiagShared.h"

namespace skydiag::plugin {
namespace {

std::uint32_t g_crashHookMode = 1;

bool IsProbablyFatalException(DWORD code) noexcept
{
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:        // 0xC0000005
    case EXCEPTION_IN_PAGE_ERROR:           // 0xC0000006
    case EXCEPTION_ILLEGAL_INSTRUCTION:     // 0xC000001D
    case EXCEPTION_PRIV_INSTRUCTION:        // 0xC0000096
    case EXCEPTION_STACK_OVERFLOW:          // 0xC00000FD
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:   // 0xC000008C
    case EXCEPTION_INT_DIVIDE_BY_ZERO:      // 0xC0000094
    case EXCEPTION_INT_OVERFLOW:            // 0xC0000095
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:      // 0xC000008E
    case EXCEPTION_FLT_INVALID_OPERATION:   // 0xC0000090
    case EXCEPTION_FLT_OVERFLOW:            // 0xC0000091
    case EXCEPTION_FLT_UNDERFLOW:           // 0xC0000093
    case STATUS_HEAP_CORRUPTION:            // 0xC0000374
    case STATUS_STACK_BUFFER_OVERRUN:       // 0xC0000409
    case STATUS_INVALID_HANDLE:             // 0xC0000008
      return true;
    default:
      return false;
  }
}

bool ShouldRecordException(DWORD code) noexcept
{
  switch (g_crashHookMode) {
    case 2:  // All exceptions (legacy behavior; can false-trigger).
      return true;
    case 1:  // Fatal only (recommended).
      return IsProbablyFatalException(code);
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
  if (!ShouldRecordException(code)) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  const LONG prev = InterlockedCompareExchange(
    reinterpret_cast<volatile LONG*>(&shm->header.crash_seq),
    1,
    0);
  if (prev != 0) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  shm->header.crash.exception_code = code;
  shm->header.crash.exception_addr = reinterpret_cast<std::uint64_t>(ep->ExceptionRecord->ExceptionAddress);
  shm->header.crash.faulting_tid = GetCurrentThreadId();

  std::memcpy(&shm->header.crash.exception_record, ep->ExceptionRecord, sizeof(EXCEPTION_RECORD));
  std::memcpy(&shm->header.crash.context, ep->ContextRecord, sizeof(CONTEXT));

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
