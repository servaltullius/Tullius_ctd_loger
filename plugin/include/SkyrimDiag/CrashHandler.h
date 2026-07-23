#pragma once

#include <cstdint>

namespace skydiag::plugin {

struct CrashHandlerModuleRange
{
  std::uintptr_t begin = 0;
  std::uintptr_t end = 0;
};

constexpr bool CrashHandlerModuleRangeContains(
  CrashHandlerModuleRange range,
  std::uintptr_t address) noexcept
{
  return range.begin != 0u && range.end > range.begin &&
         address >= range.begin && address < range.end;
}

constexpr bool ShouldSuppressNestedCrashLoggerException(
  bool crashAlreadyFrozen,
  std::uintptr_t exceptionAddress,
  CrashHandlerModuleRange crashLoggerRange,
  CrashHandlerModuleRange crashLoggerSseRange) noexcept
{
  return crashAlreadyFrozen &&
         (CrashHandlerModuleRangeContains(crashLoggerRange, exceptionAddress) ||
          CrashHandlerModuleRangeContains(crashLoggerSseRange, exceptionAddress));
}

// CrashHookMode:
//   0 = Off
//   1 = Fatal exceptions only (recommended; avoids many false positives)
//   2 = All exceptions (can false-trigger on handled exceptions)
bool InstallCrashHandler(std::uint32_t crashHookMode);

}  // namespace skydiag::plugin
