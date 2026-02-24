#pragma once

#include <Windows.h>

#include <cstdint>

namespace skydiag::helper {

// Best-effort heuristic for distinguishing "real crash" exception codes from
// shutdown-boundary noise where exit_code can be 0.
//
// Policy:
// - Treat everything else as strong by default so we don't suppress CTD UX when
//   the process exit code is misleading.
inline bool IsStrongCrashException(std::uint32_t code) noexcept
{
  if (code == 0) {
    return false;
  }
  constexpr std::uint32_t kStatusInvalidHandle = 0xC0000008u;
  constexpr std::uint32_t kStatusCppException = 0xE06D7363u;
  constexpr std::uint32_t kStatusClrException = 0xE0434F4Du;
  constexpr std::uint32_t kStatusBreakpoint = 0x80000003u;
  constexpr std::uint32_t kStatusControlCExit = 0xC000013Au;

  if (code == kStatusInvalidHandle ||
      code == kStatusCppException ||
      code == kStatusClrException ||
      code == kStatusBreakpoint ||
      code == kStatusControlCExit) {
    return false;
  }
  return true;
}

}  // namespace skydiag::helper
