#pragma once

#ifdef _WIN32
#include <Windows.h>
#endif

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "CrashCapture.h"

namespace skydiag::helper::internal {

// ADR-0038: Windows Error Reporting Application Error (event 1000) lookup for
// an abnormal game exit without a crash SkyrimDiag recorded.
inline constexpr std::uint64_t kWerExitEventLookbackMs = 600000u;  // 10 minutes
inline constexpr std::size_t kWerExitEventMaxEvents = 200u;
inline constexpr std::uint64_t kWerExitEventReadBudgetMs = 500u;
inline constexpr std::uint32_t kWerExitEventNextTimeoutMs = 500u;
inline constexpr std::uint32_t kWerExitEventRetryDelayMs = 1000u;
inline constexpr std::uint32_t kWerExitEventRetryMinExitCode = 0xC0000000u;

struct WerExitEvent
{
  std::wstring moduleName;
  std::wstring moduleVersion;
  std::uint32_t exceptionCode = 0;
  std::uint64_t faultingOffset = 0;
  std::uint32_t processId = 0;
  std::uint64_t processCreationTime = 0;
};

enum class WerExitQueryStatus
{
  kFound,
  kNotFound,
  kQueryFailed,
};

struct WerExitQueryResult
{
  WerExitQueryStatus status = WerExitQueryStatus::kNotFound;
  std::optional<WerExitEvent> event;
};

// ADR-0038 Decision 1: target exit branches for WER event 1000 lookup.
// Applies to unrecorded non-zero exits (closing-hang, unresponsive close other
// than Windows' not-responding close 0xCFFFFFFF, and abnormal exit without a
// recorded crash), regardless of EnableWerDumpFallbackHint. Skips normal exit,
// captured crash, dump-write-failed crash, post-exit crash on record, and
// 0xCFFFFFFF (event 1002).
constexpr bool ShouldQueryWerExitEvent(
  std::uint32_t exitCode,
  bool crashCapturedLatched,
  std::uint32_t dumpWriteFailedCrashSeq,
  bool crashOnRecord) noexcept
{
  return exitCode != 0u && !crashCapturedLatched && dumpWriteFailedCrashSeq == 0u && !crashOnRecord &&
         exitCode != kExitCodeClosedAsNotResponding;
}

// ADR-0038 Decision 3: retry once after 1 s only when the first query succeeded
// without finding an event and the exit code is an NTSTATUS error (>= 0xC0000000).
constexpr bool ShouldRetryWerExitEventQuery(std::uint32_t exitCode, WerExitQueryStatus firstStatus) noexcept
{
  return firstStatus == WerExitQueryStatus::kNotFound && exitCode >= kWerExitEventRetryMinExitCode;
}

// XPath query for Application Error event 1000 within the last `lookbackMs`.
std::wstring WerExitEventQuery(std::uint64_t lookbackMs = kWerExitEventLookbackMs);

// Parses a hexadecimal integer (optional 0x/0X prefix, trimmed whitespace,
// arbitrary leading zeros, 64-bit unsigned). Never interprets as decimal.
bool ParseWerHexUint64(std::wstring_view text, std::uint64_t* out) noexcept;

// Parses one rendered Application Error (event 1000) XML. Reads <EventData>
// <Data> elements by Name attribute when present, else by Windows' positional
// schema order. Returns nullopt if required numeric fields are missing or invalid.
std::optional<WerExitEvent> ParseWerExitEventXml(std::wstring_view xml);

// True when the event matches the game process by both PID and 64-bit FILETIME
// creation time. Never matches when creation time (or PID) is 0.
bool MatchesWerExitEventProcess(
  const WerExitEvent& event,
  std::uint32_t processId,
  std::uint64_t processCreationTime) noexcept;

// Formats the single English Helper log line for a matched event (ADR-0038 Decision 4).
std::wstring FormatWerExitEventLine(const WerExitEvent& event);

// Pure end-to-end helper: parses `xml`, matches (processId, processCreationTime),
// and formats the log line without calling any Windows API.
std::optional<std::wstring> FormatMatchingWerExitEventLine(
  std::wstring_view xml,
  std::uint32_t processId,
  std::uint64_t processCreationTime);

// Orchestrates initial lookup + optional single retry (ADR-0038 Decision 3),
// parameterized by query/sleep functions so failure and retry paths can be tested
// without touching the real Windows Event Log or sleeping.
template <typename QueryFn, typename SleepFn>
std::optional<std::wstring> FindWerExitEventLineWithRetry(
  std::uint32_t exitCode,
  std::uint32_t processId,
  std::uint64_t processCreationTime,
  QueryFn&& queryOnce,
  SleepFn&& sleepMs)
{
  if (processId == 0u || processCreationTime == 0u) {
    return std::nullopt;
  }
  const WerExitQueryResult first = queryOnce(processId, processCreationTime);
  if (first.status == WerExitQueryStatus::kFound && first.event) {
    return FormatWerExitEventLine(*first.event);
  }
  if (!ShouldRetryWerExitEventQuery(exitCode, first.status)) {
    return std::nullopt;
  }
  sleepMs(kWerExitEventRetryDelayMs);
  const WerExitQueryResult second = queryOnce(processId, processCreationTime);
  if (second.status == WerExitQueryStatus::kFound && second.event) {
    return FormatWerExitEventLine(*second.event);
  }
  return std::nullopt;
}

#ifdef _WIN32
using WerExitEventLineLookupFn = std::optional<std::wstring> (*)(
  HANDLE process,
  std::uint32_t processId,
  std::uint32_t exitCode);

// Replaces the production Windows Event Log lookup used by
// QueryWerExitEventLineForProcess during tests (nullptr restores production).
// Returns the previous override.
WerExitEventLineLookupFn SetWerExitEventLineLookupOverrideForTests(WerExitEventLineLookupFn fn) noexcept;

// 64-bit FILETIME creation time of `process` via GetProcessTimes, or 0 on failure.
std::uint64_t GetProcessCreationTimeFileTime(HANDLE process) noexcept;

// Queries the Windows Application log once for an Application Error event 1000
// matching (processId, processCreationTime) within `lookbackMs`.
WerExitQueryResult QueryWerExitEventOnce(
  std::uint32_t processId,
  std::uint64_t processCreationTime,
  std::uint64_t lookbackMs = kWerExitEventLookbackMs);

// Full production lookup called at process exit: reads creation time from
// `process`, queries once, and retries once after 1 s for NTSTATUS exit codes.
// Never throws: any exception leaves the exit path uninterrupted with nullopt.
std::optional<std::wstring> QueryWerExitEventLineForProcess(
  HANDLE process,
  std::uint32_t processId,
  std::uint32_t exitCode) noexcept;
#endif

}  // namespace skydiag::helper::internal
