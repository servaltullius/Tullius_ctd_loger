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

bool IsIgnorableException(DWORD code) noexcept
{
  // Benign exceptions: first-chance C++ SEH, OutputDebugString, thread naming,
  // breakpoints in debuggers, etc.
  switch (code) {
    case 0xE06D7363:                     // MSVC C++ exception (SEH __CxxThrowException)
    case 0x406D1388:                     // SetThreadName via RaiseException (legacy)
    case EXCEPTION_BREAKPOINT:           // 0x80000003 — debugger breakpoint
    case EXCEPTION_SINGLE_STEP:          // 0x80000004 — single step (debugger)
    case 0x4001000A:                     // OutputDebugStringW
      return true;
    default:
      return false;
  }
}

// Mode 1 (recommended): record all exceptions EXCEPT known-benign ones.
// This catches real crashes regardless of exception code, while filtering out
// routine first-chance exceptions (C++ SEH, debugger breakpoints, thread naming).
//
// Mode 2 (legacy/unsafe): record everything including known-benign exceptions.
bool ShouldRecordException(DWORD code) noexcept
{
  switch (g_crashHookMode) {
    case 2:  // All exceptions (legacy behavior; can false-trigger).
      return true;
    case 1:  // Blacklist: record everything except known-benign.
      return !IsIgnorableException(code);
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
