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

// NOTE: first-fault preservation is deliberately absent.
//
// Once a record is committed, a later fatal exception can still overwrite it,
// and on a heavily modded setup a single corruption often cascades into further
// faults on other threads — so the record that survives is not always the one
// nearest the root cause.
//
// An attempt to preserve the first strong fault was reverted before release. It
// required the plugin to suppress a later fault without moving crash_seq, which
// left the suppression invisible to the helper: crash_seq is this protocol's
// only generation counter, and a separate unversioned state flag could not give
// the two processes an atomic view of one incident. Every added check narrowed
// the race and left another interleaving that could discard evidence.
//
// Implementing it safely needs a protocol with a single atomic linearization
// point — a versioned incident state, or a combined generation/state word
// updated with CAS — which means a SharedLayout version bump and an ADR-0004
// compatibility review, not more observations around two independent values.

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
