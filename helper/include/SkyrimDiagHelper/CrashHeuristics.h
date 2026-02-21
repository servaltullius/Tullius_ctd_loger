#pragma once

#include <Windows.h>

#include <cstdint>

namespace skydiag::helper {

// Best-effort heuristic for distinguishing "real crash" exception codes from
// shutdown-boundary noise where exit_code can be 0.
//
// Policy:
// - Treat STATUS_INVALID_HANDLE as weak (can surface during shutdown / cleanup).
// - Treat everything else as strong by default so we don't suppress CTD UX when
//   the process exit code is misleading.
inline bool IsStrongCrashException(std::uint32_t code) noexcept
{
  if (code == 0) {
    return false;
  }
  return code != static_cast<std::uint32_t>(STATUS_INVALID_HANDLE);
}

}  // namespace skydiag::helper
