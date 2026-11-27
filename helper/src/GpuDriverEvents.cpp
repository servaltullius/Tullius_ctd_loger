#include "GpuDriverEvents.h"

#include <Windows.h>

#include <winevt.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <ctime>
#include <iterator>
#include <map>

#include <nlohmann/json.hpp>

#include "EventLogCommon.h"
#include "HelperCommon.h"

namespace skydiag::helper::internal {

namespace {

// The display drivers of the three GPU vendors, and Windows' own TDR record
// (Display 4101, "display driver stopped responding and has recovered").
constexpr const wchar_t* kProviders[] = { L"nvlddmkm", L"amdkmdag", L"amdwddmg", L"igfx", L"igfxn", L"Display" };

// Bounds the work when a broken driver floods the log. The read comes before
// the dump is written, so it gets a time budget of its own as well.
constexpr std::size_t kMaxEventsCounted = 10000;
constexpr std::size_t kMaxMessageChars = 300;
constexpr ULONGLONG kReadBudgetMs = 1500;
constexpr DWORD kNextTimeoutMs = 500;

std::wstring IsoUtc(std::uint64_t unix, const wchar_t* fraction = L".000Z")
{
  const auto seconds = static_cast<std::time_t>(unix);
  std::tm utc{};
  wchar_t text[32]{};
  if (gmtime_s(&utc, &seconds) != 0 || std::wcsftime(text, std::size(text), L"%Y-%m-%dT%H:%M:%S", &utc) == 0u) {
    return std::wstring(L"1970-01-01T00:00:00") + fraction;
  }
  return std::wstring(text) + fraction;
}

// Days since 1970-01-01 of a civil date (proleptic Gregorian).
std::int64_t DaysFromCivil(std::int64_t y, unsigned m, unsigned d)
{
  y -= m <= 2u ? 1 : 0;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned mp = m > 2u ? m - 3u : m + 9u;
  const unsigned doy = (153u * mp + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// "2026-11-03T02:28:26.5067795Z" -> seconds since 1970.
std::uint64_t ParseSystemTime(std::wstring_view text)
{
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
  const std::wstring copy(text.substr(0, 19));
  if (swscanf_s(copy.c_str(), L"%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6 || mo < 1 || mo > 12 ||
      d < 1 || d > 31) {
    return 0u;
  }
  const std::int64_t days = DaysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
  const std::int64_t unix = days * 86400 + std::int64_t{ h } * 3600 + std::int64_t{ mi } * 60 + s;
  return unix > 0 ? static_cast<std::uint64_t>(unix) : 0u;
}

// Whitespace runs to one space, trimmed, capped.
std::wstring Tidy(std::wstring_view text)
{
  std::wstring out;
  for (const wchar_t c : text) {
    if (std::iswspace(c) != 0) {
      if (!out.empty() && out.back() != L' ') {
        out.push_back(L' ');
      }
    } else {
      out.push_back(c);
    }
  }
  while (!out.empty() && out.back() == L' ') {
    out.pop_back();
  }
  if (out.size() > kMaxMessageChars) {
    out.resize(kMaxMessageChars);
  }
  return out;
}

std::wstring FormatEventMessage(EVT_HANDLE event, const std::wstring& provider, std::map<std::wstring, EvtHandle>& publishers)
{
  auto it = publishers.find(provider);
  if (it == publishers.end()) {
    // A driver that ships no message table gives none: its data strings then.
    it = publishers.emplace(provider, EvtHandle(EvtOpenPublisherMetadata(nullptr, provider.c_str(), nullptr, 0, 0))).first;
  }
  if (!it->second) {
    return {};
  }
  DWORD used = 0;
  if (EvtFormatMessage(it->second.get(), event, 0, 0, nullptr, EvtFormatMessageEvent, 0, nullptr, &used) ||
      GetLastError() != ERROR_INSUFFICIENT_BUFFER || used == 0u) {
    return {};
  }
  std::wstring text(used, L'\0');
  if (!EvtFormatMessage(it->second.get(), event, 0, 0, nullptr, EvtFormatMessageEvent, used, text.data(), &used)) {
    return {};
  }
  text.resize(wcsnlen(text.c_str(), text.size()));
  return Tidy(text);
}

}  // namespace

std::wstring GpuDriverEventQuery(std::uint64_t sinceUnix, std::uint64_t untilUnix)
{
  std::wstring providers;
  for (const auto* provider : kProviders) {
    providers += (providers.empty() ? L"" : L" or ") + std::wstring(L"Provider[@Name='") + provider + L"']";
  }
  // untilUnix comes from std::time(nullptr) (whole seconds); extend the upper
  // bound through .9999999Z so a driver TDR/reset in the same second as the
  // crash or freeze is not dropped by 100 ns SystemTime comparison.
  return L"*[System[(" + providers + L") and (Level=1 or Level=2 or Level=3) and TimeCreated[@SystemTime>='" +
    IsoUtc(sinceUnix) +
    L"' and @SystemTime<='" + IsoUtc(untilUnix, L".9999999Z") + L"']]]";
}

std::optional<GpuDriverEvent> ParseGpuDriverEventXml(std::wstring_view xml)
{
  GpuDriverEvent event{};
  event.provider = XmlAttributeOf(xml, L"<Provider", L"Name");
  const auto id = XmlTextOf(xml, L"EventID");
  const auto time = XmlAttributeOf(xml, L"<TimeCreated", L"SystemTime");
  if (event.provider.empty() || id.empty() || time.empty()) {
    return std::nullopt;
  }
  event.id = static_cast<std::uint32_t>(std::wcstoul(id.c_str(), nullptr, 10));
  event.level = static_cast<std::uint32_t>(std::wcstoul(XmlTextOf(xml, L"Level").c_str(), nullptr, 10));
  event.unix = ParseSystemTime(time);

  // The data strings, the device path (\Device\Video3) left out.
  std::wstring message;
  const auto data = xml.find(L"<EventData");
  for (auto at = data == std::wstring_view::npos ? data : xml.find(L"<Data", data); at != std::wstring_view::npos;
       at = xml.find(L"<Data", at + 5u)) {
    const auto open = xml.find(L'>', at);
    if (open == std::wstring_view::npos) {
      break;
    }
    if (xml[open - 1u] == L'/') {
      continue;  // <Data/>: empty
    }
    const auto end = xml.find(L"</Data>", open);
    if (end == std::wstring_view::npos) {
      break;
    }
    const auto value = Tidy(UnescapeXml(xml.substr(open + 1u, end - open - 1u)));
    if (!value.empty() && value.rfind(L"\\Device\\", 0) != 0) {
      message += (message.empty() ? L"" : L"; ") + value;
    }
  }
  event.message = Tidy(message);
  return event;
}

GpuDriverEventLog QueryGpuDriverEvents(std::uint64_t sinceUnix, std::uint64_t untilUnix)
{
  GpuDriverEventLog log{};
  log.since_unix = sinceUnix;
  log.until_unix = untilUnix;
  if (sinceUnix == 0u || untilUnix < sinceUnix) {
    return log;
  }
  const auto query = GpuDriverEventQuery(sinceUnix, untilUnix);
  // Newest first, so the ones kept are the latest.
  EvtHandle results(EvtQuery(nullptr, L"System", query.c_str(), EvtQueryChannelPath | EvtQueryReverseDirection));
  if (!results) {
    return log;
  }
  log.read = true;
  std::map<std::wstring, EvtHandle> publishers;
  EVT_HANDLE batch[16]{};
  DWORD returned = 0;
  const ULONGLONG started = GetTickCount64();
  for (;;) {
    if (log.total >= kMaxEventsCounted || GetTickCount64() - started > kReadBudgetMs) {
      log.complete = false;  // more may follow
      break;
    }
    if (!EvtNext(results.get(), static_cast<DWORD>(std::size(batch)), batch, kNextTimeoutMs, 0, &returned)) {
      // Only "no more" is the end; a timeout or an error leaves the count short.
      log.complete = GetLastError() == ERROR_NO_MORE_ITEMS;
      break;
    }
    for (DWORD i = 0; i < returned; ++i) {
      EvtHandle event(batch[i]);
      ++log.total;
      if (log.events.size() >= kGpuDriverEventsKept) {
        continue;
      }
      if (auto parsed = ParseGpuDriverEventXml(RenderEventXml(event.get()))) {
        if (auto text = FormatEventMessage(event.get(), parsed->provider, publishers); !text.empty()) {
          parsed->message = std::move(text);
        }
        log.events.push_back(std::move(*parsed));
      }
    }
  }
  std::reverse(log.events.begin(), log.events.end());
  return log;
}

std::string GpuDriverEventsJson(const GpuDriverEventLog& log)
{
  if (!log.read) {
    return {};
  }
  nlohmann::json events = nlohmann::json::array();
  for (const auto& event : log.events) {
    events.push_back({
      { "provider", WideToUtf8(event.provider) },
      { "id", event.id },
      { "level", event.level },
      { "unix", event.unix },
      { "message", WideToUtf8(event.message) },
    });
  }
  return nlohmann::json{
    { "version", 1 },
    { "since_unix", log.since_unix },
    { "until_unix", log.until_unix },
    { "total", log.total },
    { "complete", log.complete },
    { "events", events },
  }.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::uint64_t ProcessStartUnix(std::uint32_t pid)
{
  const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) {
    return 0u;
  }
  FILETIME created{}, exited{}, kernel{}, user{};
  const bool ok = GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE;
  CloseHandle(process);
  if (!ok) {
    return 0u;
  }
  const std::uint64_t ticks = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
  constexpr std::uint64_t kTicksTo1970 = 116444736000000000ull;  // 100 ns units from 1601
  return ticks > kTicksTo1970 ? (ticks - kTicksTo1970) / 10000000ull : 0u;
}

}  // namespace skydiag::helper::internal
