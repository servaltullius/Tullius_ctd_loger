// ADR-0038: Application Error (event 1000) XML parsing, process matching,
// log-line formatting, retry/failure behavior, and exit-branch gating.

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "WerExitEvent.h"

#ifdef _WIN32
#include <Windows.h>
#endif

using skydiag::helper::internal::FindWerExitEventLineWithRetry;
using skydiag::helper::internal::FormatMatchingWerExitEventLine;
using skydiag::helper::internal::kExitCodeClosedAsNotResponding;
using skydiag::helper::internal::kWerExitEventRetryDelayMs;
using skydiag::helper::internal::ParseWerExitEventXml;
using skydiag::helper::internal::ParseWerHexUint64;
using skydiag::helper::internal::ShouldQueryWerExitEvent;
using skydiag::helper::internal::ShouldRetryWerExitEventQuery;
using skydiag::helper::internal::WerExitEventQuery;
using skydiag::helper::internal::WerExitQueryResult;
using skydiag::helper::internal::WerExitQueryStatus;

namespace {

constexpr std::uint32_t kSamplePid = 0x8074u;
constexpr std::uint64_t kSampleCreationTime = 0x1dd7e4ce612c54aull;

constexpr const wchar_t* kSampleNamedXml =
  L"<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'>"
  L"<System>"
  L"<Provider Name='Application Error' Guid='{a0e9b465-b939-57d7-b27d-95d8e925ff57}'/>"
  L"<EventID>1000</EventID><Version>0</Version><Level>2</Level><Task>100</Task><Opcode>0</Opcode>"
  L"<Keywords>0x8000000000000000</Keywords>"
  L"<TimeCreated SystemTime='2026-11-27T09:00:15.7346966Z'/>"
  L"<EventRecordID>152859</EventRecordID><Correlation/>"
  L"<Execution ProcessID='31660' ThreadID='59860'/>"
  L"<Channel>Application</Channel><Computer>TESTPC</Computer>"
  L"<Security UserID='S-1-5-21-0-0-0-1001'/>"
  L"</System>"
  L"<EventData>"
  L"<Data Name='AppName'>SkyrimSE.exe</Data>"
  L"<Data Name='AppVersion'>1.6.1170.0</Data>"
  L"<Data Name='AppTimeStamp'>65a5f4d5</Data>"
  L"<Data Name='ModuleName'>ntdll.dll</Data>"
  L"<Data Name='ModuleVersion'>10.0.26100.9974</Data>"
  L"<Data Name='ModuleTimeStamp'>a2b84216</Data>"
  L"<Data Name='ExceptionCode'>c0000005</Data>"
  L"<Data Name='FaultingOffset'>0000000000055ae0</Data>"
  L"<Data Name='ProcessId'>0x8074</Data>"
  L"<Data Name='ProcessCreationTime'>0x1dd7e4ce612c54a</Data>"
  L"<Data Name='AppPath'>G:\\Games\\Skyrim\\SkyrimSE.exe</Data>"
  L"<Data Name='ModulePath'>C:\\WINDOWS\\SYSTEM32\\ntdll.dll</Data>"
  L"<Data Name='IntegratorReportId'>873111d5-2b72-421f-8010-e28a292f1e8c</Data>"
  L"<Data Name='PackageFullName'></Data>"
  L"<Data Name='PackageRelativeAppId'></Data>"
  L"</EventData>"
  L"</Event>";

constexpr const wchar_t* kSamplePositionalXml =
  L"<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'>"
  L"<System>"
  L"<Provider Name='Application Error'/>"
  L"<EventID>1000</EventID><Level>2</Level>"
  L"<TimeCreated SystemTime='2026-11-27T09:00:15.7346966Z'/>"
  L"<Channel>Application</Channel><Computer>TESTPC</Computer>"
  L"<Security UserID='S-1-5-21-0-0-0-1001'/>"
  L"</System>"
  L"<EventData>"
  L"<Data>SkyrimSE.exe</Data>"
  L"<Data>1.6.1170.0</Data>"
  L"<Data>65a5f4d5</Data>"
  L"<Data>ntdll.dll</Data>"
  L"<Data>10.0.26100.9974</Data>"
  L"<Data>a2b84216</Data>"
  L"<Data>c0000005</Data>"
  L"<Data>0000000000055ae0</Data>"
  L"<Data>0x8074</Data>"
  L"<Data>0x1dd7e4ce612c54a</Data>"
  L"<Data>G:\\Games\\Skyrim\\SkyrimSE.exe</Data>"
  L"<Data>C:\\WINDOWS\\SYSTEM32\\ntdll.dll</Data>"
  L"<Data>873111d5-2b72-421f-8010-e28a292f1e8c</Data>"
  L"<Data></Data>"
  L"<Data></Data>"
  L"</EventData>"
  L"</Event>";

constexpr const wchar_t* kExpectedSampleLine =
  L"Windows Error Reporting recorded this exit (Application log, event 1000): "
  L"exception 0xc0000005 in ntdll.dll 10.0.26100.9974 at offset 0x55ae0. "
  L"That is where the process ended, not necessarily where the problem began.";

std::wstring MakeNamedXml(
  std::wstring_view moduleName,
  std::wstring_view moduleVersion,
  std::wstring_view exceptionCode,
  std::wstring_view faultingOffset,
  std::wstring_view processId,
  std::wstring_view processCreationTime)
{
  std::wstring xml =
    L"<Event><System><Provider Name='Application Error'/><EventID>1000</EventID>"
    L"<Computer>TESTPC</Computer><Security UserID='S-1-5-21-0-0-0-1001'/></System><EventData>"
    L"<Data Name='AppName'>SkyrimSE.exe</Data>"
    L"<Data Name='ModuleName'>";
  xml += moduleName;
  xml += L"</Data><Data Name='ModuleVersion'>";
  xml += moduleVersion;
  xml += L"</Data><Data Name='ExceptionCode'>";
  xml += exceptionCode;
  xml += L"</Data><Data Name='FaultingOffset'>";
  xml += faultingOffset;
  xml += L"</Data><Data Name='ProcessId'>";
  xml += processId;
  xml += L"</Data><Data Name='ProcessCreationTime'>";
  xml += processCreationTime;
  xml += L"</Data><Data Name='AppPath'>G:\\Games\\Skyrim\\SkyrimSE.exe</Data>"
         L"<Data Name='ModulePath'>C:\\Windows\\System32\\ntdll.dll</Data>"
         L"</EventData></Event>";
  return xml;
}

void TestSampleNamedAndPositionalXml()
{
  const auto namedLine = FormatMatchingWerExitEventLine(kSampleNamedXml, kSamplePid, kSampleCreationTime);
  assert(namedLine.has_value() && "Sample named XML must match and produce a line");
  assert(*namedLine == kExpectedSampleLine && "Named XML line must match ADR-0038 exact format");
  assert(namedLine->find(L"TESTPC") == std::wstring::npos && "Computer name must not appear");
  assert(namedLine->find(L"S-1-5-21") == std::wstring::npos && "User SID must not appear");
  assert(namedLine->find(L"SkyrimSE.exe") == std::wstring::npos && "App path/name must not appear");
  assert(namedLine->find(L"SYSTEM32") == std::wstring::npos && "Module path must not appear");

  const auto positionalLine = FormatMatchingWerExitEventLine(kSamplePositionalXml, kSamplePid, kSampleCreationTime);
  assert(positionalLine.has_value() && "Sample positional XML must match and produce a line");
  assert(*positionalLine == kExpectedSampleLine && "Positional XML must produce the exact same line");

  // Named <Data> in a different order than the positional schema, with double quotes.
  const wchar_t* reorderedDoubleQuotedXml =
    L"<Event><EventData>"
    L"<Data Name=\"ProcessCreationTime\">0x1dd7e4ce612c54a</Data>"
    L"<Data Name=\"ProcessId\">0x8074</Data>"
    L"<Data Name=\"FaultingOffset\">0000000000055ae0</Data>"
    L"<Data Name=\"ExceptionCode\">c0000005</Data>"
    L"<Data Name=\"ModuleVersion\">10.0.26100.9974</Data>"
    L"<Data Name=\"ModuleName\">ntdll.dll</Data>"
    L"</EventData></Event>";
  assert(
    FormatMatchingWerExitEventLine(reorderedDoubleQuotedXml, kSamplePid, kSampleCreationTime) == kExpectedSampleLine &&
    "Reordered double-quoted named <Data> elements must produce the exact same line");

  // Positional XML with self-closing empty <Data/> elements keeps later indices aligned.
  const wchar_t* selfClosingPositional =
    L"<Event><EventData>"
    L"<Data>SkyrimSE.exe</Data><Data>1.6.1170.0</Data><Data/>"
    L"<Data>ntdll.dll</Data><Data/><Data/>"
    L"<Data>c0000005</Data><Data>0000000000055ae0</Data>"
    L"<Data>8074</Data><Data>1dd7e4ce612c54a</Data>"
    L"</EventData></Event>";
  const auto selfClosingLine = FormatMatchingWerExitEventLine(selfClosingPositional, kSamplePid, kSampleCreationTime);
  assert(selfClosingLine.has_value() && "Self-closing positional <Data/> must keep indices aligned");
  assert(
    *selfClosingLine ==
      L"Windows Error Reporting recorded this exit (Application log, event 1000): "
      L"exception 0xc0000005 in ntdll.dll at offset 0x55ae0. "
      L"That is where the process ended, not necessarily where the problem began." &&
    "Empty self-closing ModuleVersion must be omitted cleanly");

  // Positional XML with fewer than 9 <Data> elements (missing ProcessId & ProcessCreationTime),
  // and with 9 <Data> elements (missing ProcessCreationTime) -> rejected.
  const wchar_t* positionalTooShort8 =
    L"<Event><EventData>"
    L"<Data>SkyrimSE.exe</Data><Data>1.6.1170.0</Data><Data>65a5f4d5</Data>"
    L"<Data>ntdll.dll</Data><Data>10.0.26100.9974</Data><Data>a2b84216</Data>"
    L"<Data>c0000005</Data><Data>0000000000055ae0</Data>"
    L"</EventData></Event>";
  assert(!ParseWerExitEventXml(positionalTooShort8).has_value() && "Positional XML with < 9 items must fail");

  const wchar_t* positionalTooShort9 =
    L"<Event><EventData>"
    L"<Data>SkyrimSE.exe</Data><Data>1.6.1170.0</Data><Data>65a5f4d5</Data>"
    L"<Data>ntdll.dll</Data><Data>10.0.26100.9974</Data><Data>a2b84216</Data>"
    L"<Data>c0000005</Data><Data>0000000000055ae0</Data><Data>0x8074</Data>"
    L"</EventData></Event>";
  assert(!ParseWerExitEventXml(positionalTooShort9).has_value() && "Positional XML missing index 9 must fail");
}

void TestProcessMatchingAndHexParsing()
{
  // Same PID, different creation time -> must not match.
  assert(
    !FormatMatchingWerExitEventLine(kSampleNamedXml, kSamplePid, kSampleCreationTime + 1ull).has_value() &&
    "Same PID with different creation time must not match");

  // Different PID, same creation time -> must not match.
  assert(
    !FormatMatchingWerExitEventLine(kSampleNamedXml, kSamplePid + 1u, kSampleCreationTime).has_value() &&
    "Different PID with same creation time must not match");

  // Unprefixed hex PID "8074" must parse as 0x8074 (32884), NOT decimal 8074.
  const std::wstring unprefixedHexXml = MakeNamedXml(
    L"ntdll.dll", L"10.0.26100.9974", L"0Xc0000005", L"  0x0000000000055ae0  ", L"  8074  ", L"1dd7e4ce612c54a");
  assert(
    FormatMatchingWerExitEventLine(unprefixedHexXml, 0x8074u, kSampleCreationTime) == kExpectedSampleLine &&
    "Unprefixed hex PID '8074' must match 0x8074");
  assert(
    !FormatMatchingWerExitEventLine(unprefixedHexXml, 8074u, kSampleCreationTime).has_value() &&
    "Unprefixed hex PID '8074' must NOT match decimal 8074");

  // Creation time 0 -> never matches, even if XML has ProcessCreationTime=0.
  const std::wstring zeroCreationXml =
    MakeNamedXml(L"ntdll.dll", L"1.0.0.0", L"c0000005", L"10", L"0x8074", L"0x0");
  assert(
    !FormatMatchingWerExitEventLine(kSampleNamedXml, kSamplePid, 0ull).has_value() &&
    "Zero expected creation time must not match");
  assert(
    !FormatMatchingWerExitEventLine(zeroCreationXml, kSamplePid, 0ull).has_value() &&
    "Zero creation time in both event and target must not match");
  assert(
    !FormatMatchingWerExitEventLine(kSampleNamedXml, 0u, kSampleCreationTime).has_value() &&
    "Zero expected PID must not match");

  // Direct hex parser edge cases.
  std::uint64_t val = 0;
  assert(ParseWerHexUint64(L"8074", &val) && val == 0x8074ull);
  assert(ParseWerHexUint64(L"0x8074", &val) && val == 0x8074ull);
  assert(ParseWerHexUint64(L"0X00008074", &val) && val == 0x8074ull);
  assert(ParseWerHexUint64(L"0000000000000000000000000055ae0", &val) && val == 0x55ae0ull);
  assert(ParseWerHexUint64(L"ffffffffffffffff", &val) && val == 0xFFFFFFFFFFFFFFFFull);
  assert(!ParseWerHexUint64(L"", &val));
  assert(!ParseWerHexUint64(L"   ", &val));
  assert(!ParseWerHexUint64(L"0x", &val));
  assert(!ParseWerHexUint64(L"-1", &val));
  assert(!ParseWerHexUint64(L"+1", &val));
  assert(!ParseWerHexUint64(L"123g", &val));
  assert(!ParseWerHexUint64(L"10000000000000000", &val));  // > 64-bit overflow
}

void TestFormattingEdgeCasesAndSanitization()
{
  // 64-bit offset, small exception code (8-digit zero-padded), empty version.
  const std::wstring bigOffsetSmallCode =
    MakeNamedXml(L"mod.dll", L"", L"00000001", L"00007ff612345678", L"0x8074", L"0x1dd7e4ce612c54a");
  assert(
    FormatMatchingWerExitEventLine(bigOffsetSmallCode, kSamplePid, kSampleCreationTime) ==
      L"Windows Error Reporting recorded this exit (Application log, event 1000): "
      L"exception 0x00000001 in mod.dll at offset 0x7ff612345678. "
      L"That is where the process ended, not necessarily where the problem began." &&
    "Small exception code must be 8-digit lowercase hex, 64-bit offset trimmed, empty version omitted");

  // 0.0.0.0 version and zero offset.
  const std::wstring zeroVersionZeroOffset =
    MakeNamedXml(L"mod.dll", L"0.0.0.0", L"C0000409", L"0000000000000000", L"0x8074", L"0x1dd7e4ce612c54a");
  assert(
    FormatMatchingWerExitEventLine(zeroVersionZeroOffset, kSamplePid, kSampleCreationTime) ==
      L"Windows Error Reporting recorded this exit (Application log, event 1000): "
      L"exception 0xc0000409 in mod.dll at offset 0x0. "
      L"That is where the process ended, not necessarily where the problem began." &&
    "Version 0.0.0.0 must be omitted and offset 0 formatted as 0x0");

  // Path separators (both \ and /), raw control chars (0x00..0x1f, 0x7f), XML entities (&amp;).
  const std::wstring messyModule = MakeNamedXml(
    L"C:\\Users\\secret/mods\\Foo &amp; Bar\r\n\x01\x1f\x7f.dll",
    L"1.2\r\n.3\x7f",
    L"c0000005",
    L"0x10",
    L"0x8074",
    L"0x1dd7e4ce612c54a");
  const auto messyLine = FormatMatchingWerExitEventLine(messyModule, kSamplePid, kSampleCreationTime);
  assert(messyLine.has_value());
  assert(
    *messyLine ==
      L"Windows Error Reporting recorded this exit (Application log, event 1000): "
      L"exception 0xc0000005 in Foo & Bar.dll 1.2.3 at offset 0x10. "
      L"That is where the process ended, not necessarily where the problem began." &&
    "Path prefix and raw control chars must be stripped, &amp; unescaped");

  // Numeric character references for control characters (< 0x20 and 0x7f) must be stripped,
  // NOT converted to spaces: Foo&#13;&#10;Bar.dll -> FooBar.dll, &#x7f; removed.
  const std::wstring numericRefControlModule = MakeNamedXml(
    L"Foo&#13;&#10;Bar&#x7f;.dll",
    L"1&#9;.0&#x7f;",
    L"c0000005",
    L"0x10",
    L"0x8074",
    L"0x1dd7e4ce612c54a");
  assert(
    FormatMatchingWerExitEventLine(numericRefControlModule, kSamplePid, kSampleCreationTime) ==
      L"Windows Error Reporting recorded this exit (Application log, event 1000): "
      L"exception 0xc0000005 in FooBar.dll 1.0 at offset 0x10. "
      L"That is where the process ended, not necessarily where the problem began." &&
    "Numeric control-char references (&#13;&#10;, &#9;, &#x7f;) must be stripped without turning into spaces");

  // Empty module name, trailing-slash-only module name, control-chars-only -> "unknown module".
  for (const wchar_t* emptyMod : { L"", L"   ", L"C:\\Windows\\System32\\", L"\x01\x1f\x7f", L"&#13;&#10;&#x7f;" }) {
    const std::wstring xml =
      MakeNamedXml(emptyMod, L"0.0.0.0", L"c0000005", L"0x20", L"0x8074", L"0x1dd7e4ce612c54a");
    assert(
      FormatMatchingWerExitEventLine(xml, kSamplePid, kSampleCreationTime) ==
        L"Windows Error Reporting recorded this exit (Application log, event 1000): "
        L"exception 0xc0000005 in unknown module at offset 0x20. "
        L"That is where the process ended, not necessarily where the problem began." &&
      "Empty sanitized module name must fall back to 'unknown module'");
  }

  // Missing or unparseable required fields -> nullopt.
  assert(!ParseWerExitEventXml(L"").has_value());
  assert(!ParseWerExitEventXml(L"<Event><EventData/></Event>").has_value());
  assert(
    !ParseWerExitEventXml(MakeNamedXml(L"a.dll", L"1.0", L"", L"10", L"0x8074", L"0x1dd7e4ce612c54a")).has_value() &&
    "Missing ExceptionCode must fail");
  assert(
    !ParseWerExitEventXml(MakeNamedXml(L"a.dll", L"1.0", L"c0000005", L"", L"0x8074", L"0x1dd7e4ce612c54a"))
       .has_value() &&
    "Missing FaultingOffset must fail");
  // Missing ONLY ProcessId:
  assert(
    !ParseWerExitEventXml(
       L"<Event><EventData>"
       L"<Data Name='ModuleName'>ntdll.dll</Data>"
       L"<Data Name='ModuleVersion'>1.0.0.0</Data>"
       L"<Data Name='ExceptionCode'>c0000005</Data>"
       L"<Data Name='FaultingOffset'>55ae0</Data>"
       L"<Data Name='ProcessCreationTime'>0x1dd7e4ce612c54a</Data>"
       L"</EventData></Event>")
       .has_value() &&
    "Missing only ProcessId must fail");
  // Missing ONLY ProcessCreationTime:
  assert(
    !ParseWerExitEventXml(
       L"<Event><EventData>"
       L"<Data Name='ModuleName'>ntdll.dll</Data>"
       L"<Data Name='ModuleVersion'>1.0.0.0</Data>"
       L"<Data Name='ExceptionCode'>c0000005</Data>"
       L"<Data Name='FaultingOffset'>55ae0</Data>"
       L"<Data Name='ProcessId'>0x8074</Data>"
       L"</EventData></Event>")
       .has_value() &&
    "Missing only ProcessCreationTime must fail");
  assert(
    !ParseWerExitEventXml(MakeNamedXml(L"a.dll", L"1.0", L"nothex", L"10", L"0x8074", L"0x1dd7e4ce612c54a"))
       .has_value() &&
    "Invalid hex ExceptionCode must fail");
  assert(
    !ParseWerExitEventXml(MakeNamedXml(L"a.dll", L"1.0", L"c0000005", L"xyz", L"0x8074", L"0x1dd7e4ce612c54a"))
       .has_value() &&
    "Invalid hex FaultingOffset must fail");
  assert(
    !ParseWerExitEventXml(MakeNamedXml(L"a.dll", L"1.0", L"100000000", L"10", L"0x8074", L"0x1dd7e4ce612c54a"))
       .has_value() &&
    "ExceptionCode > 32-bit must fail");
  assert(
    !ParseWerExitEventXml(MakeNamedXml(L"a.dll", L"1.0", L"c0000005", L"10", L"100000000", L"0x1dd7e4ce612c54a"))
       .has_value() &&
    "ProcessId > 32-bit must fail");
}

void TestQueryStringAndRetryBehavior()
{
  const std::wstring query = WerExitEventQuery();
  assert(
    query ==
      L"*[System[Provider[@Name='Application Error'] and (EventID=1000) and "
      L"TimeCreated[timediff(@SystemTime) <= 600000]]]" &&
    "Default XPath query must match ADR-0038");

  const auto sampleEvent = ParseWerExitEventXml(kSampleNamedXml);
  assert(sampleEvent.has_value());

  // 1. Query failure -> no retry, no sleep, no line (even for NTSTATUS exit code).
  {
    int queryCalls = 0;
    int sleepCalls = 0;
    const auto line = FindWerExitEventLineWithRetry(
      0xC0000005u,
      kSamplePid,
      kSampleCreationTime,
      [&](std::uint32_t, std::uint64_t) {
        ++queryCalls;
        return WerExitQueryResult{ WerExitQueryStatus::kQueryFailed, std::nullopt };
      },
      [&](std::uint32_t) { ++sleepCalls; });
    assert(!line.has_value() && "Query failure must produce no line");
    assert(queryCalls == 1 && "Query failure must not retry");
    assert(sleepCalls == 0 && "Query failure must not sleep");
  }

  // 2. Non-NTSTATUS exit code (< 0xC0000000) not found -> no retry, no sleep.
  {
    int queryCalls = 0;
    int sleepCalls = 0;
    const auto line = FindWerExitEventLineWithRetry(
      1u,
      kSamplePid,
      kSampleCreationTime,
      [&](std::uint32_t, std::uint64_t) {
        ++queryCalls;
        return WerExitQueryResult{ WerExitQueryStatus::kNotFound, std::nullopt };
      },
      [&](std::uint32_t) { ++sleepCalls; });
    assert(!line.has_value());
    assert(queryCalls == 1 && "Exit code < 0xC0000000 must not retry");
    assert(sleepCalls == 0 && "Exit code < 0xC0000000 must not sleep");
  }

  // 3. NTSTATUS exit code (>= 0xC0000000) not found on first try, found on retry after 1 s.
  {
    int queryCalls = 0;
    std::vector<std::uint32_t> sleeps;
    const auto line = FindWerExitEventLineWithRetry(
      0xC0000409u,
      kSamplePid,
      kSampleCreationTime,
      [&](std::uint32_t, std::uint64_t) {
        ++queryCalls;
        if (queryCalls == 1) {
          return WerExitQueryResult{ WerExitQueryStatus::kNotFound, std::nullopt };
        }
        return WerExitQueryResult{ WerExitQueryStatus::kFound, sampleEvent };
      },
      [&](std::uint32_t ms) { sleeps.push_back(ms); });
    assert(line == kExpectedSampleLine && "Retry match must return formatted line");
    assert(queryCalls == 2 && "NTSTATUS exit code must retry once");
    assert(sleeps.size() == 1u && sleeps[0] == kWerExitEventRetryDelayMs && "Retry must sleep 1000 ms once");
  }

  // 4. NTSTATUS exit code not found on BOTH initial query and retry -> nullopt after 1 retry.
  {
    int queryCalls = 0;
    std::vector<std::uint32_t> sleeps;
    const auto line = FindWerExitEventLineWithRetry(
      0xC0000005u,
      kSamplePid,
      kSampleCreationTime,
      [&](std::uint32_t, std::uint64_t) {
        ++queryCalls;
        return WerExitQueryResult{ WerExitQueryStatus::kNotFound, std::nullopt };
      },
      [&](std::uint32_t ms) { sleeps.push_back(ms); });
    assert(!line.has_value() && "Both attempts not found must return nullopt");
    assert(queryCalls == 2 && "NTSTATUS exit code must try initial + 1 retry");
    assert(sleeps.size() == 1u && sleeps[0] == kWerExitEventRetryDelayMs && "Must sleep 1000 ms once");
  }

  // 5. NTSTATUS exit code found on first try -> no retry, no sleep.
  {
    int queryCalls = 0;
    int sleepCalls = 0;
    const auto line = FindWerExitEventLineWithRetry(
      0xC0000005u,
      kSamplePid,
      kSampleCreationTime,
      [&](std::uint32_t, std::uint64_t) {
        ++queryCalls;
        return WerExitQueryResult{ WerExitQueryStatus::kFound, sampleEvent };
      },
      [&](std::uint32_t) { ++sleepCalls; });
    assert(line == kExpectedSampleLine);
    assert(queryCalls == 1 && "Immediate match must not retry");
    assert(sleepCalls == 0 && "Immediate match must not sleep");
  }

  // 6. Creation time 0 -> never queries or sleeps.
  {
    int queryCalls = 0;
    int sleepCalls = 0;
    const auto line = FindWerExitEventLineWithRetry(
      0xC0000005u,
      kSamplePid,
      0ull,
      [&](std::uint32_t, std::uint64_t) {
        ++queryCalls;
        return WerExitQueryResult{ WerExitQueryStatus::kFound, sampleEvent };
      },
      [&](std::uint32_t) { ++sleepCalls; });
    assert(!line.has_value());
    assert(queryCalls == 0 && "Creation time 0 must not query");
    assert(sleepCalls == 0 && "Creation time 0 must not sleep");
  }

  assert(!ShouldRetryWerExitEventQuery(0xC0000005u, WerExitQueryStatus::kQueryFailed));
  assert(!ShouldRetryWerExitEventQuery(0xBFFFFFFFu, WerExitQueryStatus::kNotFound));
  assert(ShouldRetryWerExitEventQuery(0xC0000000u, WerExitQueryStatus::kNotFound));
}

void TestTargetBranchPolicy()
{
  // Excluded branches:
  assert(!ShouldQueryWerExitEvent(0u, false, 0u, false) && "Exit code 0 must not query");
  assert(!ShouldQueryWerExitEvent(0xC0000005u, true, 0u, false) && "Latched captured crash must not query");
  assert(!ShouldQueryWerExitEvent(0xC0000005u, false, 2u, false) && "dumpWriteFailedCrashSeq != 0 must not query");
  assert(!ShouldQueryWerExitEvent(0xC0000005u, false, 0u, true) && "crashOnRecord must not query");
  assert(
    !ShouldQueryWerExitEvent(kExitCodeClosedAsNotResponding, false, 0u, false) &&
    "0xCFFFFFFF (closed as not responding, event 1002) must not query");

  // Included branches (independent of EnableWerDumpFallbackHint):
  assert(
    ShouldQueryWerExitEvent(1u, false, 0u, false) &&
    "Non-zero unrecorded exit (closing/unresponsive/else) must query");
  assert(ShouldQueryWerExitEvent(0xC0000005u, false, 0u, false) && "NTSTATUS unrecorded exit must query");
  assert(ShouldQueryWerExitEvent(0xC0000409u, false, 0u, false) && "Fail-fast unrecorded exit must query");
}

#ifdef _WIN32
void TestWindowsProcessCreationTimeAndLiveQuerySmoke()
{
  assert(
    skydiag::helper::internal::GetProcessCreationTimeFileTime(nullptr) == 0u &&
    "Null handle must return 0 creation time");
  const std::uint64_t selfCreated =
    skydiag::helper::internal::GetProcessCreationTimeFileTime(GetCurrentProcess());
  assert(selfCreated != 0u && "GetCurrentProcess creation time must be non-zero");

  // Smoke check against the real Application log for this running test process
  // (which has not crashed, so it should return kNotFound cleanly, or kQueryFailed
  // if the event log service is unavailable — never crash or hang).
  const auto res = skydiag::helper::internal::QueryWerExitEventOnce(
    GetCurrentProcessId(), selfCreated, skydiag::helper::internal::kWerExitEventLookbackMs);
  assert(
    res.status == WerExitQueryStatus::kNotFound || res.status == WerExitQueryStatus::kQueryFailed);
}

int RunManualCheck(int argc, char** argv)
{
  if (argc < 4) {
    std::fprintf(stderr, "Usage: %s --manual-check <pid-hex> <creation-time-hex> [lookback-ms]\n", argv[0]);
    return 2;
  }
  auto toWide = [](const char* s) {
    std::wstring out;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
      out.push_back(static_cast<wchar_t>(*p));
    }
    return out;
  };
  std::uint64_t pid64 = 0;
  std::uint64_t creationTime = 0;
  if (!ParseWerHexUint64(toWide(argv[2]), &pid64) || pid64 > 0xFFFFFFFFull ||
      !ParseWerHexUint64(toWide(argv[3]), &creationTime) || creationTime == 0u) {
    std::fprintf(stderr, "Invalid hex arguments.\n");
    return 2;
  }
  std::uint64_t lookbackMs = 86400000ull;  // 24h default for manual check
  if (argc >= 5) {
    lookbackMs = std::strtoull(argv[4], nullptr, 10);
  }
  const auto t0 = std::chrono::steady_clock::now();
  const auto res =
    skydiag::helper::internal::QueryWerExitEventOnce(static_cast<std::uint32_t>(pid64), creationTime, lookbackMs);
  const auto elapsedMs =
    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  if (res.status == WerExitQueryStatus::kQueryFailed) {
    std::fprintf(stderr, "Query failed (elapsed=%lld ms).\n", static_cast<long long>(elapsedMs));
    return 1;
  }
  if (res.status != WerExitQueryStatus::kFound || !res.event) {
    std::fprintf(
      stderr,
      "No matching Application Error event 1000 found in lookback window (%llu ms; elapsed=%lld ms).\n",
      static_cast<unsigned long long>(lookbackMs),
      static_cast<long long>(elapsedMs));
    return 1;
  }
  const std::wstring line = skydiag::helper::internal::FormatWerExitEventLine(*res.event);
  std::wcout << line << L'\n';
  std::fprintf(stderr, "elapsed_ms=%lld\n", static_cast<long long>(elapsedMs));
  return 0;
}
#endif

}  // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
  if (argc >= 2 && std::strcmp(argv[1], "--manual-check") == 0) {
    return RunManualCheck(argc, argv);
  }
#else
  (void)argc;
  (void)argv;
#endif

  TestSampleNamedAndPositionalXml();
  TestProcessMatchingAndHexParsing();
  TestFormattingEdgeCasesAndSanitization();
  TestQueryStringAndRetryBehavior();
  TestTargetBranchPolicy();
#ifdef _WIN32
  TestWindowsProcessCreationTimeAndLiveQuerySmoke();
#endif

  std::puts("wer exit event tests passed");
  return 0;
}
