#pragma once

#include <cstddef>
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

// Kept exceptions live in one slot per recently throwing thread. With a
// single slot, a caught throw on another thread (an audio or Papyrus worker)
// replaced the unhandled one CrashLogger was still reporting, and the crash
// was then ignored (review #5).
inline constexpr std::size_t kKeptExceptionSlotCount = 8;

struct KeptExceptionSlotView
{
  std::uint32_t tid = 0;
  std::uint64_t qpc = 0;
};

constexpr bool IsKeptExceptionSlotRecent(
  std::uint64_t keptQpc,
  std::uint64_t nowQpc,
  std::uint64_t maxAgeQpc) noexcept
{
  return keptQpc != 0u && nowQpc >= keptQpc && (nowQpc - keptQpc) <= maxAgeQpc;
}

// The slot a thread keeps its exception in: its own, else an empty or stale
// one, else the oldest.
constexpr std::size_t SelectKeptExceptionSlot(
  const KeptExceptionSlotView* slots,
  std::size_t count,
  std::uint32_t tid,
  std::uint64_t nowQpc,
  std::uint64_t maxAgeQpc) noexcept
{
  for (std::size_t i = 0; i < count; ++i) {
    if (slots[i].tid == tid) {
      return i;
    }
  }
  for (std::size_t i = 0; i < count; ++i) {
    if (slots[i].tid == 0u || !IsKeptExceptionSlotRecent(slots[i].qpc, nowQpc, maxAgeQpc)) {
      return i;
    }
  }
  std::size_t oldest = 0;
  for (std::size_t i = 1; i < count; ++i) {
    if (slots[i].qpc < slots[oldest].qpc) {
      oldest = i;
    }
  }
  return oldest;
}

// What to do with a fatal-code exception raised inside CrashLogger.
//
// CrashLogger catches the access violations its memory probes raise, both
// while it reports a crash and while it writes a thread dump on its hotkey
// (Ctrl+Shift+F12 by default in CrashLogger 1.25, also SkyrimDiag's manual
// capture hotkey up to v0.2.68-rc3). Its own exception is therefore never the
// crash to record. Record the exception it is reporting when one was kept on
// that thread; otherwise ignore it. Recording the probes made every thread-dump hotkey press write
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

// While CrashLogger reports a kept exception, its report can also fault
// outside its own image, in game or runtime code it calls (a stale form's
// name, for example). That fault is nested in the dispatch of the kept
// exception: on the same thread, below the CONTEXT the dispatcher keeps for
// the kept exception (still unchanged there), with CrashLogger's frames
// between the two. The handler scans that part of the stack for a CrashLogger
// address and, if one is there, records the kept exception instead of the
// fault. Any other fault is recorded as before; the scan never makes a fault
// ignored.
struct CrashLoggerStackScanWindow
{
  std::uintptr_t begin = 0;
  std::uintptr_t end = 0;
};

// The pointer-aligned stack range to scan: from the fault's stack pointer up
// to the kept exception's dispatch CONTEXT, at most maxBytes, and only inside
// this thread's stack [stackLow, stackHigh). Empty when the fault is not
// nested below that CONTEXT on that stack.
constexpr CrashLoggerStackScanWindow SelectCrashLoggerStackScanWindow(
  std::uintptr_t faultSp,
  std::uintptr_t keptContextAddress,
  std::uintptr_t stackLow,
  std::uintptr_t stackHigh,
  std::uintptr_t maxBytes) noexcept
{
  constexpr std::uintptr_t kAlign = sizeof(std::uintptr_t);
  if (stackLow >= stackHigh || faultSp < stackLow || keptContextAddress > stackHigh ||
      faultSp >= keptContextAddress) {
    return {};
  }
  const std::uintptr_t begin = (faultSp + (kAlign - 1u)) & ~(kAlign - 1u);
  std::uintptr_t end = keptContextAddress & ~(kAlign - 1u);
  if (begin >= end) {
    return {};
  }
  if (end - begin > maxBytes) {
    end = begin + (maxBytes & ~(kAlign - 1u));
  }
  return { begin, end };
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
//
// Mode 1 also chains a top-level unhandled exception filter and a SIGABRT
// handler. They record an unhandled C++ throw or breakpoint when CrashLogger is
// not there to fault while reporting it: the filter sees one no frame handled,
// and the SIGABRT handler the kept throw that terminate() ends the process for
// (a throw out of a noexcept function never reaches the filter). Neither runs
// for a handled exception, and both use only the fatal path's fixed-size,
// allocation-free steps.
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
