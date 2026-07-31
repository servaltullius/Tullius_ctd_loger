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

// Protocol v4 preserves the first selected exception for one incident.
//
// kState_Frozen is the cross-process incident ownership/ACK bit. A fatal writer
// must CAS-claim it before changing crash_seq or CrashInfo. Later writers lose
// that same atomic claim and cannot overwrite the first committed record. If
// the helper proves the record was recovered or cannot retain its dump, its
// atomic clear of kState_Frozen acknowledges the old generation and rearms the
// slot for the next incident. crash_seq remains the CrashInfo seqlock and
// generation used for stable helper snapshots.

// CrashHookMode:
//   0 = Off
//   1 = Fatal exceptions only (recommended; avoids many false positives)
//   2 = All exceptions (can false-trigger on handled exceptions)
bool InstallCrashHandler(std::uint32_t crashHookMode);

// Caches the CrashLogger module image ranges used by nested-exception
// suppression. InstallCrashHandler runs during SKSE plugin load, which is too
// early to observe a CrashLogger build that loads after us, so the SKSE
// lifecycle calls this again once more plugins are resident.
//
// Each range is published exactly once and never rewritten, so the crash
// handler only ever observes an empty range or a fully published one.
//
// MUST NOT be called from the crash handler: GetModuleHandleW acquires the
// loader lock, which may already be held by the faulting thread.
void RefreshCrashLoggerModuleRanges() noexcept;

}  // namespace skydiag::plugin
