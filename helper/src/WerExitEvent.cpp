#include "WerExitEvent.h"

#ifdef _WIN32
#include <Windows.h>

#include <winevt.h>
#endif

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <limits>
#include <utility>

#include "EventLogCommon.h"

namespace skydiag::helper::internal {

namespace {

#ifdef _WIN32
std::atomic<WerExitEventLineLookupFn> g_werExitEventLineLookupOverride{ nullptr };
#endif

std::wstring_view TrimWhitespace(std::wstring_view text) noexcept
{
  while (!text.empty() && std::iswspace(text.front()) != 0) {
    text.remove_prefix(1u);
  }
  while (!text.empty() && std::iswspace(text.back()) != 0) {
    text.remove_suffix(1u);
  }
  return text;
}

std::wstring StripControlAndTrimSpaces(std::wstring_view text)
{
  std::wstring out;
  out.reserve(text.size());
  for (const wchar_t c : text) {
    if (c <= 0x1Fu || c == 0x7Fu) {
      continue;
    }
    out.push_back(c);
  }
  const auto first = out.find_first_not_of(L' ');
  if (first == std::wstring::npos) {
    return {};
  }
  const auto last = out.find_last_not_of(L' ');
  return out.substr(first, last - first + 1u);
}

std::wstring SanitizeModuleName(std::wstring_view raw)
{
  const auto sep = raw.find_last_of(L"\\/");
  if (sep != std::wstring_view::npos) {
    raw = raw.substr(sep + 1u);
  }
  std::wstring cleaned = StripControlAndTrimSpaces(raw);
  if (cleaned.empty()) {
    return L"unknown module";
  }
  return cleaned;
}

std::wstring SanitizeModuleVersion(std::wstring_view raw)
{
  std::wstring cleaned = StripControlAndTrimSpaces(raw);
  if (cleaned == L"0.0.0.0") {
    return {};
  }
  return cleaned;
}

std::wstring FormatHex32Lower(std::uint32_t value)
{
  static constexpr wchar_t kHex[] = L"0123456789abcdef";
  std::wstring out(10u, L'0');
  out[0] = L'0';
  out[1] = L'x';
  for (int i = 0; i < 8; ++i) {
    out[9u - static_cast<std::size_t>(i)] = kHex[(value >> (i * 4)) & 0xFu];
  }
  return out;
}

std::wstring FormatHex64TrimmedLower(std::uint64_t value)
{
  static constexpr wchar_t kHex[] = L"0123456789abcdef";
  if (value == 0u) {
    return L"0x0";
  }
  wchar_t buf[16]{};
  std::size_t n = 0;
  while (value != 0u) {
    buf[n++] = kHex[value & 0xFu];
    value >>= 4;
  }
  std::wstring out;
  out.reserve(2u + n);
  out += L"0x";
  while (n > 0u) {
    out.push_back(buf[--n]);
  }
  return out;
}

}  // namespace

std::wstring WerExitEventQuery(std::uint64_t lookbackMs)
{
  return L"*[System[Provider[@Name='Application Error'] and (EventID=1000) and TimeCreated[timediff(@SystemTime) <= " +
         std::to_wstring(lookbackMs) + L"]]]";
}

bool ParseWerHexUint64(std::wstring_view text, std::uint64_t* out) noexcept
{
  if (!out) {
    return false;
  }
  std::wstring_view sv = TrimWhitespace(text);
  if (sv.empty()) {
    return false;
  }
  if (sv.size() >= 2u && sv[0] == L'0' && (sv[1] == L'x' || sv[1] == L'X')) {
    sv.remove_prefix(2u);
  }
  if (sv.empty()) {
    return false;
  }
  std::uint64_t value = 0;
  constexpr std::uint64_t kMaxBeforeShift = (std::numeric_limits<std::uint64_t>::max)() >> 4;
  for (const wchar_t c : sv) {
    std::uint64_t digit = 0;
    if (c >= L'0' && c <= L'9') {
      digit = static_cast<std::uint64_t>(c - L'0');
    } else if (c >= L'a' && c <= L'f') {
      digit = static_cast<std::uint64_t>(c - L'a' + 10);
    } else if (c >= L'A' && c <= L'F') {
      digit = static_cast<std::uint64_t>(c - L'A' + 10);
    } else {
      return false;
    }
    if (value > kMaxBeforeShift) {
      return false;
    }
    value = (value << 4) | digit;
  }
  *out = value;
  return true;
}

std::optional<WerExitEvent> ParseWerExitEventXml(std::wstring_view xml)
{
  const auto eventDataStart = xml.find(L"<EventData");
  if (eventDataStart == std::wstring_view::npos) {
    return std::nullopt;
  }
  const auto eventDataOpen = xml.find(L'>', eventDataStart);
  if (eventDataOpen == std::wstring_view::npos || xml[eventDataOpen - 1u] == L'/') {
    return std::nullopt;
  }
  const auto eventDataClose = xml.find(L"</EventData>", eventDataOpen + 1u);
  const std::wstring_view body = eventDataClose == std::wstring_view::npos
    ? xml.substr(eventDataOpen + 1u)
    : xml.substr(eventDataOpen + 1u, eventDataClose - (eventDataOpen + 1u));

  std::wstring moduleName;
  std::wstring moduleVersion;
  std::wstring exceptionCodeRaw;
  std::wstring faultingOffsetRaw;
  std::wstring processIdRaw;
  std::wstring processCreationTimeRaw;
  bool hasExceptionCode = false;
  bool hasFaultingOffset = false;
  bool hasProcessId = false;
  bool hasProcessCreationTime = false;

  std::size_t cursor = 0;
  std::size_t index = 0;
  while (cursor < body.size()) {
    const auto at = body.find(L"<Data", cursor);
    if (at == std::wstring_view::npos) {
      break;
    }
    const auto afterTagName = at + 5u;
    if (afterTagName >= body.size()) {
      break;
    }
    const wchar_t nextChar = body[afterTagName];
    if (nextChar != L'>' && nextChar != L'/' && nextChar != L' ' && nextChar != L'\t' && nextChar != L'\r' &&
        nextChar != L'\n') {
      cursor = afterTagName;
      continue;
    }
    const auto open = body.find(L'>', afterTagName);
    if (open == std::wstring_view::npos) {
      break;
    }

    const std::wstring name = XmlAttributeOf(body.substr(at, open - at + 1u), L"<Data", L"Name");
    std::wstring value;
    if (body[open - 1u] == L'/') {
      cursor = open + 1u;
    } else {
      const auto end = body.find(L"</Data>", open + 1u);
      if (end == std::wstring_view::npos) {
        break;
      }
      value = UnescapeXml(body.substr(open + 1u, end - (open + 1u)), XmlControlCharMode::kStrip);
      cursor = end + 7u;  // length of "</Data>"
    }

    // Event 1000 <EventData> positional order (when Name attribute is absent):
    // 0 AppName, 1 AppVersion, 2 AppTimeStamp, 3 ModuleName, 4 ModuleVersion,
    // 5 ModuleTimeStamp, 6 ExceptionCode, 7 FaultingOffset, 8 ProcessId,
    // 9 ProcessCreationTime, 10 AppPath, 11 ModulePath, 12 IntegratorReportId,
    // 13 PackageFullName, 14 PackageRelativeAppId.
    if (!name.empty()) {
      if (name == L"ModuleName") {
        moduleName = std::move(value);
      } else if (name == L"ModuleVersion") {
        moduleVersion = std::move(value);
      } else if (name == L"ExceptionCode") {
        exceptionCodeRaw = std::move(value);
        hasExceptionCode = true;
      } else if (name == L"FaultingOffset") {
        faultingOffsetRaw = std::move(value);
        hasFaultingOffset = true;
      } else if (name == L"ProcessId") {
        processIdRaw = std::move(value);
        hasProcessId = true;
      } else if (name == L"ProcessCreationTime") {
        processCreationTimeRaw = std::move(value);
        hasProcessCreationTime = true;
      }
    } else {
      switch (index) {
        case 3u:
          moduleName = std::move(value);
          break;
        case 4u:
          moduleVersion = std::move(value);
          break;
        case 6u:
          exceptionCodeRaw = std::move(value);
          hasExceptionCode = true;
          break;
        case 7u:
          faultingOffsetRaw = std::move(value);
          hasFaultingOffset = true;
          break;
        case 8u:
          processIdRaw = std::move(value);
          hasProcessId = true;
          break;
        case 9u:
          processCreationTimeRaw = std::move(value);
          hasProcessCreationTime = true;
          break;
        default:
          break;
      }
    }
    ++index;
  }

  if (!hasExceptionCode || !hasFaultingOffset || !hasProcessId || !hasProcessCreationTime) {
    return std::nullopt;
  }

  std::uint64_t exceptionCode64 = 0;
  std::uint64_t faultingOffset = 0;
  std::uint64_t processId64 = 0;
  std::uint64_t processCreationTime = 0;
  if (!ParseWerHexUint64(exceptionCodeRaw, &exceptionCode64) ||
      !ParseWerHexUint64(faultingOffsetRaw, &faultingOffset) ||
      !ParseWerHexUint64(processIdRaw, &processId64) ||
      !ParseWerHexUint64(processCreationTimeRaw, &processCreationTime)) {
    return std::nullopt;
  }
  if (exceptionCode64 > 0xFFFFFFFFull || processId64 > 0xFFFFFFFFull) {
    return std::nullopt;
  }

  WerExitEvent event{};
  event.moduleName = std::move(moduleName);
  event.moduleVersion = std::move(moduleVersion);
  event.exceptionCode = static_cast<std::uint32_t>(exceptionCode64);
  event.faultingOffset = faultingOffset;
  event.processId = static_cast<std::uint32_t>(processId64);
  event.processCreationTime = processCreationTime;
  return event;
}

bool MatchesWerExitEventProcess(
  const WerExitEvent& event,
  std::uint32_t processId,
  std::uint64_t processCreationTime) noexcept
{
  return processId != 0u && processCreationTime != 0u && event.processId == processId &&
         event.processCreationTime == processCreationTime;
}

std::wstring FormatWerExitEventLine(const WerExitEvent& event)
{
  const std::wstring moduleName = SanitizeModuleName(event.moduleName);
  const std::wstring moduleVersion = SanitizeModuleVersion(event.moduleVersion);
  std::wstring line = L"Windows Error Reporting recorded this exit (Application log, event 1000): exception ";
  line += FormatHex32Lower(event.exceptionCode);
  line += L" in ";
  line += moduleName;
  if (!moduleVersion.empty()) {
    line += L' ';
    line += moduleVersion;
  }
  line += L" at offset ";
  line += FormatHex64TrimmedLower(event.faultingOffset);
  line += L". That is where the process ended, not necessarily where the problem began.";
  return line;
}

std::optional<std::wstring> FormatMatchingWerExitEventLine(
  std::wstring_view xml,
  std::uint32_t processId,
  std::uint64_t processCreationTime)
{
  if (processId == 0u || processCreationTime == 0u) {
    return std::nullopt;
  }
  const auto parsed = ParseWerExitEventXml(xml);
  if (!parsed || !MatchesWerExitEventProcess(*parsed, processId, processCreationTime)) {
    return std::nullopt;
  }
  return FormatWerExitEventLine(*parsed);
}

#ifdef _WIN32

WerExitEventLineLookupFn SetWerExitEventLineLookupOverrideForTests(WerExitEventLineLookupFn fn) noexcept
{
  return g_werExitEventLineLookupOverride.exchange(fn, std::memory_order_acq_rel);
}

std::uint64_t GetProcessCreationTimeFileTime(HANDLE process) noexcept
{
  if (!process) {
    return 0u;
  }
  FILETIME created{};
  FILETIME unusedExit{};
  FILETIME unusedKernel{};
  FILETIME unusedUser{};
  if (!GetProcessTimes(process, &created, &unusedExit, &unusedKernel, &unusedUser)) {
    return 0u;
  }
  return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

WerExitQueryResult QueryWerExitEventOnce(
  std::uint32_t processId,
  std::uint64_t processCreationTime,
  std::uint64_t lookbackMs)
{
  if (processId == 0u || processCreationTime == 0u) {
    return { WerExitQueryStatus::kNotFound, std::nullopt };
  }
  const std::wstring query = WerExitEventQuery(lookbackMs);
  EvtHandle results(
    EvtQuery(nullptr, L"Application", query.c_str(), EvtQueryChannelPath | EvtQueryReverseDirection));
  if (!results) {
    return { WerExitQueryStatus::kQueryFailed, std::nullopt };
  }

  std::size_t scanned = 0;
  const ULONGLONG started = GetTickCount64();
  for (;;) {
    if (scanned >= kWerExitEventMaxEvents) {
      break;
    }
    const ULONGLONG elapsed = GetTickCount64() - started;
    if (elapsed >= kWerExitEventReadBudgetMs) {
      break;
    }
    const ULONGLONG remaining = kWerExitEventReadBudgetMs - elapsed;
    const DWORD nextTimeout = static_cast<DWORD>(
      (std::max)(ULONGLONG{ 1u }, (std::min)(static_cast<ULONGLONG>(kWerExitEventNextTimeoutMs), remaining)));
    // Request one event at a time in reverse order so the event log service
    // does not scan further back in history trying to fill a multi-event batch
    // when the newest matching event is already at the head.
    EVT_HANDLE rawEvent = nullptr;
    DWORD returned = 0;
    if (!EvtNext(results.get(), 1u, &rawEvent, nextTimeout, 0, &returned)) {
      const DWORD err = GetLastError();
      if (err == ERROR_NO_MORE_ITEMS || err == ERROR_TIMEOUT) {
        break;
      }
      return { WerExitQueryStatus::kQueryFailed, std::nullopt };
    }
    if (returned == 0u || !rawEvent) {
      continue;
    }
    EvtHandle event(rawEvent);
    ++scanned;
    if (auto parsed = ParseWerExitEventXml(RenderEventXml(event.get()))) {
      if (MatchesWerExitEventProcess(*parsed, processId, processCreationTime)) {
        return { WerExitQueryStatus::kFound, std::move(*parsed) };
      }
    }
  }
  return { WerExitQueryStatus::kNotFound, std::nullopt };
}

std::optional<std::wstring> QueryWerExitEventLineForProcess(
  HANDLE process,
  std::uint32_t processId,
  std::uint32_t exitCode) noexcept
{
  try {
    if (const auto overrideFn = g_werExitEventLineLookupOverride.load(std::memory_order_acquire)) {
      return overrideFn(process, processId, exitCode);
    }
    const std::uint64_t creationTime = GetProcessCreationTimeFileTime(process);
    if (creationTime == 0u || processId == 0u) {
      return std::nullopt;
    }
    return FindWerExitEventLineWithRetry(
      exitCode,
      processId,
      creationTime,
      [](std::uint32_t pid, std::uint64_t created) {
        return QueryWerExitEventOnce(pid, created, kWerExitEventLookbackMs);
      },
      [](std::uint32_t delayMs) { Sleep(delayMs); });
  } catch (...) {
    return std::nullopt;
  }
}

#endif

}  // namespace skydiag::helper::internal
