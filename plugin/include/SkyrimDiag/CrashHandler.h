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

constexpr bool IsInCrashLoggerModule(
  std::uintptr_t address,
  CrashHandlerModuleRange crashLoggerRange,
  CrashHandlerModuleRange crashLoggerSseRange) noexcept
{
  return CrashHandlerModuleRangeContains(crashLoggerRange, address) ||
         CrashHandlerModuleRangeContains(crashLoggerSseRange, address);
}

// In fatal-only mode an assert/abort breakpoint and a C++ throw are not
// recorded, because they are usually handled. When one is not handled,
// CrashLogger reports it, and while doing so it raises access violations of its
// own that it catches internally. Those probes are fatal codes, so without this
// the first thing recorded for the crash would be CrashLogger's probe.
//
// The handler therefore keeps the latest such exception, tagged with its
// thread, and records it instead when CrashLogger faults on that thread before
// anything else has been recorded.
inline constexpr std::uint32_t kCrashHandlerBreakpointCode = 0x80000003u;
inline constexpr std::uint32_t kCrashHandlerCppExceptionCode = 0xE06D7363u;

// throwImageBase is ExceptionInformation[3] of a C++ exception: the image that
// threw it. CrashLogger's own throws while writing its report are not the crash.
constexpr bool ShouldKeepUnrecordedException(
  std::uint32_t code,
  std::uint32_t numberParameters,
  std::uintptr_t throwImageBase,
  CrashHandlerModuleRange crashLoggerRange,
  CrashHandlerModuleRange crashLoggerSseRange) noexcept
{
  if (code == kCrashHandlerBreakpointCode) {
    return true;
  }
  if (code == kCrashHandlerCppExceptionCode) {
    return !(numberParameters >= 4u &&
             IsInCrashLoggerModule(throwImageBase, crashLoggerRange, crashLoggerSseRange));
  }
  return false;
}

// A breakpoint is the more specific signal (assert/abort), so a later C++
// throw on the same thread does not displace a recent one.
constexpr bool ShouldReplaceKeptException(
  bool haveKept,
  bool keptOnSameThread,
  bool keptIsRecent,
  std::uint32_t keptCode,
  std::uint32_t newCode) noexcept
{
  if (!haveKept || !keptOnSameThread || !keptIsRecent) {
    return true;
  }
  return !(keptCode == kCrashHandlerBreakpointCode && newCode != kCrashHandlerBreakpointCode);
}

// What to do with a fatal-code exception raised inside CrashLogger.
//
// CrashLogger catches the access violations its memory probes raise, both
// while it reports a crash and while it writes a thread dump on its hotkey
// (Ctrl+Shift+F12 in CrashLogger 1.25, the same keys as SkyrimDiag's manual
// capture). Its own exception is therefore never the crash to record. Record
// the exception it is reporting when one was kept on that thread; otherwise
// ignore it. Recording the probes made every thread-dump hotkey press write
// and then discard full crash dumps while the game stalled.
enum class CrashLoggerFaultAction
{
  kNotInCrashLogger,
  kRecordKeptException,
  kIgnore,
};

constexpr CrashLoggerFaultAction ClassifyCrashLoggerFault(
  bool crashAlreadyFrozen,
  std::uintptr_t exceptionAddress,
  CrashHandlerModuleRange crashLoggerRange,
  CrashHandlerModuleRange crashLoggerSseRange,
  bool haveKept,
  bool keptOnSameThread,
  bool keptIsRecent) noexcept
{
  if (!IsInCrashLoggerModule(exceptionAddress, crashLoggerRange, crashLoggerSseRange)) {
    return CrashLoggerFaultAction::kNotInCrashLogger;
  }
  if (!crashAlreadyFrozen && haveKept && keptOnSameThread && keptIsRecent) {
    return CrashLoggerFaultAction::kRecordKeptException;
  }
  return CrashLoggerFaultAction::kIgnore;
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
