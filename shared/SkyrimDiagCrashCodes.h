#pragma once

// Exception-code classification shared by the in-process plugin and the
// out-of-process helper.
//
// Deliberately free of <Windows.h> so both the Windows targets and the
// host-independent unit tests can use the same definitions instead of keeping
// parallel copies that could drift apart.

#include <cstdint>

namespace skydiag {

inline constexpr std::uint32_t kStatusInvalidHandle = 0xC0000008u;
inline constexpr std::uint32_t kStatusCppException = 0xE06D7363u;
inline constexpr std::uint32_t kStatusClrException = 0xE0434F4Du;
inline constexpr std::uint32_t kStatusBreakpoint = 0x80000003u;
inline constexpr std::uint32_t kStatusControlCExit = 0xC000013Au;

// Debugger and instrumentation notifications. These are raised constantly
// during normal play and are always continued, so they must agree with the
// plugin's IsBenignFirstChanceException classification: CrashHookMode=2 records
// every exception, and a benign code that counted as a strong fault there would
// anchor an incident and block the real crash that follows it.
inline constexpr std::uint32_t kStatusSingleStep = 0x80000004u;
inline constexpr std::uint32_t kStatusThreadNameLegacy = 0x406D1388u;
inline constexpr std::uint32_t kStatusOutputDebugStringA = 0x40010006u;
inline constexpr std::uint32_t kStatusOutputDebugStringW = 0x4001000Au;

// True when a code denotes a fault that is very unlikely to be handled and
// recovered from — an access violation, illegal instruction, stack overflow and
// similar.
//
// The excluded codes are routinely raised and handled during normal play:
// C++ and CLR exceptions are ordinary control flow, invalid-handle and
// breakpoint exceptions are frequently swallowed, Ctrl-C exit is a shutdown
// signal rather than a fault, and the debugger notifications above are not
// faults at all.
constexpr bool IsStrongCrashExceptionCode(std::uint32_t code) noexcept
{
  if (code == 0u) {
    return false;
  }
  if (code == kStatusInvalidHandle ||
      code == kStatusCppException ||
      code == kStatusClrException ||
      code == kStatusBreakpoint ||
      code == kStatusControlCExit ||
      code == kStatusSingleStep ||
      code == kStatusThreadNameLegacy ||
      code == kStatusOutputDebugStringA ||
      code == kStatusOutputDebugStringW) {
    return false;
  }
  return true;
}

}  // namespace skydiag
