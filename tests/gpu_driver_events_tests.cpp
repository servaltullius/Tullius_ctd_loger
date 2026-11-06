// ADR-0026: the helper reads the display drivers' events from the Windows
// System log for a freeze or manual capture.

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <string>

#include <nlohmann/json.hpp>

#include "GpuDriverEvents.h"
#include "HelperRuntimeTestUtils.h"

using skydiag::helper::internal::GpuDriverEventLog;
using skydiag::helper::internal::GpuDriverEventQuery;
using skydiag::helper::internal::GpuDriverEventsJson;
using skydiag::helper::internal::ParseGpuDriverEventXml;
using skydiag::helper::internal::ProcessStartUnix;
using skydiag::helper::internal::QueryGpuDriverEvents;
using skydiag::tests::runtime::Require;

namespace {

void TestParsesTheDriversEvents()
{
  // As this PC logged it on 2026-11-03 (nvlddmkm ships no message table).
  const auto nvidia = ParseGpuDriverEventXml(
    L"<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System><Provider Name='nvlddmkm'/>"
    L"<EventID Qualifiers='0'>153</EventID><Version>0</Version><Level>2</Level><Task>0</Task><Opcode>0</Opcode>"
    L"<Keywords>0x80000000000000</Keywords><TimeCreated SystemTime='2026-11-03T02:28:26.5067795Z'/>"
    L"<EventRecordID>85605</EventRecordID><Correlation/><Execution ProcessID='4' ThreadID='19620'/>"
    L"<Channel>System</Channel><Computer>PC</Computer><Security/></System><EventData>"
    L"<Data>\\Device\\0000016c</Data><Data>Error occurred on GPUID: 100</Data>"
    L"<Binary>0000000002003000</Binary></EventData></Event>");
  Require(nvidia && nvidia->provider == L"nvlddmkm" && nvidia->id == 153u && nvidia->level == 2u &&
            nvidia->unix == 1793672906u && nvidia->message == L"Error occurred on GPUID: 100",
    "an NVIDIA driver error, its device path left out");

  // Windows' TDR record: named data, an empty one, an escaped character, and
  // EventSourceName= after Name=.
  const auto tdr = ParseGpuDriverEventXml(
    L"<Event><System><Provider Name='Display' Guid='{00000000-0000-0000-0000-000000000000}' "
    L"EventSourceName='Display'/><EventID>4101</EventID><Level>3</Level>"
    L"<TimeCreated SystemTime=\"2026-11-06T08:00:12.0000000Z\"/></System><EventData>"
    L"<Data Name='Driver'>nvlddmkm</Data><Data Name='Detail'/><Data Name='Note'>a &amp; b</Data></EventData></Event>");
  Require(tdr && tdr->provider == L"Display" && tdr->id == 4101u && tdr->level == 3u && tdr->unix == 1793952012u &&
            tdr->message == L"nvlddmkm; a & b",
    "a driver reset");

  // Numeric character references: a control character folds into a space.
  const auto escaped = ParseGpuDriverEventXml(
    L"<Event><System><Provider Name='nvlddmkm'/><EventID>153</EventID><Level>2</Level>"
    L"<TimeCreated SystemTime='2026-11-03T02:28:26Z'/></System><EventData>"
    L"<Data>Error&#13;&#10;on GPU &#x41;&#66;</Data><Data>&#xZZ; &#;</Data></EventData></Event>");
  Require(escaped && escaped->message == L"Error on GPU AB; &#xZZ; &#;", "numeric character references");

  Require(!ParseGpuDriverEventXml(L"") && !ParseGpuDriverEventXml(L"<Event><System><Provider Name='x'/></System></Event>"),
    "not an event");

  const auto query = GpuDriverEventQuery(1793672906u, 1793952012u);
  Require(query.find(L"Provider[@Name='nvlddmkm']") != std::wstring::npos &&
            query.find(L"Provider[@Name='Display']") != std::wstring::npos &&
            query.find(L"@SystemTime>='2026-11-03T02:28:26.000Z'") != std::wstring::npos &&
            query.find(L"@SystemTime<='2026-11-06T08:00:12.000Z'") != std::wstring::npos,
    "the query names the drivers and the window");
  // Information events would push the errors out of the newest kept.
  Require(query.find(L"(Level=1 or Level=2 or Level=3)") != std::wstring::npos, "errors and warnings only");
}

void TestJsonForTheDump()
{
  GpuDriverEventLog unread{};
  Require(GpuDriverEventsJson(unread).empty(), "no stream when the log was not read");

  GpuDriverEventLog log{};
  log.read = true;
  log.since_unix = 100u;
  log.until_unix = 200u;
  log.total = 40u;
  log.events.push_back({ L"nvlddmkm", 153u, 2u, 150u, L"Error occurred on GPUID: 100" });
  const auto j = nlohmann::json::parse(GpuDriverEventsJson(log));
  Require(j["version"] == 1 && j["since_unix"] == 100 && j["until_unix"] == 200 && j["total"] == 40 &&
            j["events"].size() == 1u && j["events"][0]["provider"] == "nvlddmkm" && j["events"][0]["id"] == 153 &&
            j["events"][0]["level"] == 2 && j["events"][0]["unix"] == 150 &&
            j["events"][0]["message"] == "Error occurred on GPUID: 100" && j["complete"] == true,
    "every field");
  // A read that timed out or failed partway: none found is not none.
  log.complete = false;
  Require(nlohmann::json::parse(GpuDriverEventsJson(log))["complete"] == false, "an unfinished read says so");
}

void TestReadsThisPcsLog()
{
  const auto now = static_cast<std::uint64_t>(std::time(nullptr));
  const auto started = ProcessStartUnix(GetCurrentProcessId());
  Require(started != 0u && started <= now && now - started < 3600u, "this process started within the hour");
  // Any user may read the System log: the query works whether or not the
  // drivers logged anything.
  const auto log = QueryGpuDriverEvents(now - std::uint64_t{ 30 } * 86400u, now);
  Require(log.read && log.total >= log.events.size() &&
            log.events.size() <= skydiag::helper::internal::kGpuDriverEventsKept,
    "the System log is read");
  for (std::size_t i = 1; i < log.events.size(); ++i) {
    Require(log.events[i - 1u].unix <= log.events[i].unix, "oldest first");
  }
  for (const auto& event : log.events) {
    Require(event.level >= 1u && event.level <= 3u, "the System log returns errors and warnings only");
  }
  std::printf("GPU driver events in 30 days: %zu\n", log.total);
  Require(!QueryGpuDriverEvents(0u, now).read && !QueryGpuDriverEvents(now, now - 1u).read,
    "no window, no query");
}

}  // namespace

int main()
{
  try {
    SKYDIAG_RUN_TEST(TestParsesTheDriversEvents);
    SKYDIAG_RUN_TEST(TestJsonForTheDump);
    SKYDIAG_RUN_TEST(TestReadsThisPcsLog);
    std::puts("gpu driver events tests passed");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
