#include <Windows.h>
#include <DbgHelp.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "Analyzer.h"
#include "EvidenceBuilder.h"
#include "EvidenceBuilderPrivate.h"
#include "OutputWriterPipeline.h"
#include "SkyrimDiagCloseRequest.h"
#include "SkyrimDiagProtocol.h"
#include "SkyrimDiagSecondFault.h"
#include "SkyrimDiagShared.h"
#include "Utf.h"

namespace {

namespace fs = std::filesystem;
using namespace skydiag::dump_tool;

constexpr ULONG32 kDumpTid = 35;
constexpr ULONG32 kLogTid = 56;
constexpr ULONG32 kPid = 785;
constexpr wchar_t kEarlierModule[] = L"C:\\Fixture\\mods\\Earlier Example\\SKSE\\Plugins\\Earlier.dll";
constexpr std::uint64_t kModuleBase = 0x180000000;

struct DumpModule
{
  std::wstring_view path;
  std::uint64_t base;
};

void Require(bool condition, std::string_view message)
{
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

struct TempDirectory
{
  fs::path path = fs::temp_directory_path() /
    ("skydiag-later-crash-" + std::to_string(GetCurrentProcessId()) + "-" +
     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

  TempDirectory() { Require(fs::create_directory(path), "create fixture directory"); }
  ~TempDirectory()
  {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

struct DumpFixture
{
  MINIDUMP_HEADER header{};
  MINIDUMP_DIRECTORY streams[5]{};
  struct
  {
    ULONG32 count = 0;
    MINIDUMP_MODULE records[3]{};
  } modules;
  struct
  {
    ULONG32 length = 0;
    wchar_t name[MAX_PATH]{};
  } module_names[3];
  MINIDUMP_EXCEPTION_STREAM exception{};
  skydiag::SharedHeader blackbox{};
  skydiag::BlackboxEvent events[skydiag::kEventCapacity]{};
  skydiag::ResourceLog resources{};
  skydiag::second_fault::Stream second_fault{};
};

struct Case
{
  std::string_view location = "SkyrimSE.exe+4321";
  std::string_view thread_line = "Exception Thread ID: 56";
  std::string_view process_line = "Process ID: 785";
  std::string_view process_section = "PROCESS INFO:";
  std::string_view section_tail;
  std::string_view address = "0x180004321";
  std::span<const DumpModule> extra_modules;
  std::string_view wct;
  std::wstring_view log_name = L"crash-2097-07-13-12-00-04.log";
  bool dump_time_from_last_write = false;
  bool log_time_from_last_write = false;
  std::string_view manifest;
  ULONG32 dump_pid = kPid;
  ULONG32 dump_tid = kDumpTid;
  bool unhandled = true;
  bool second_fault_timeline = false;
  bool second_fault_stream = false;
  bool damaged_second_fault_stream = false;
  bool second_fault_qpc_unknown = false;
  // A's blackbox clock with only a crash marker (no close request), by
  // default A's own (its address and thread).
  bool crash_timeline = false;
  std::uint64_t crash_marker_address = kModuleBase + 0x123;
  std::optional<ULONG32> crash_marker_tid;  // dump_tid when not given
  bool expect_log_pairing = true;
};

constexpr std::uint64_t kTimelineStart = 1000;
constexpr std::uint64_t kFirstFaultMs = 10000;
constexpr std::uint64_t kSecondFaultMs = 14000;

void WriteSecondFaultTimeline(DumpFixture& dump)
{
  dump.blackbox.start_qpc = kTimelineStart;
  dump.blackbox.qpc_freq = 1000;
  dump.blackbox.crash_seq = 2;
  dump.blackbox.crash.exception_code = EXCEPTION_ILLEGAL_INSTRUCTION;
  dump.blackbox.crash.faulting_tid = kLogTid;
  dump.blackbox.crash.exception_addr = kModuleBase + 0x321;
  std::uint32_t i = 0;
  const auto record = [&](skydiag::EventType type, std::uint64_t relativeMs, std::uint64_t a = 0u) {
    auto& event = dump.events[i];
    event.seq = i * 2;
    event.qpc = kTimelineStart + relativeMs;
    event.type = static_cast<std::uint16_t>(type);
    event.payload.a = a;
    ++i;
  };
  record(skydiag::EventType::kSaveLoadStart, 1000);
  record(skydiag::EventType::kCloseRequest, 3500, static_cast<std::uint64_t>(skydiag::CloseRequestKind::kAltF4));
  record(skydiag::EventType::kFirstChanceException, 6000);
  record(skydiag::EventType::kFirstChanceException, 9800);
  record(skydiag::EventType::kCrash, kFirstFaultMs, EXCEPTION_ILLEGAL_INSTRUCTION);
  dump.blackbox.write_index = i;

  dump.resources.write_index = 2;
  dump.resources.entries[0].qpc = kTimelineStart + 6000;
  dump.resources.entries[1].qpc = kTimelineStart + 9800;
  dump.resources.entries[1].seq = 2;
  constexpr char older[] = "meshes\\older.nif";
  constexpr char recent[] = "meshes\\recent.nif";
  std::memcpy(dump.resources.entries[0].path_utf8, older, sizeof(older));
  std::memcpy(dump.resources.entries[1].path_utf8, recent, sizeof(recent));
}

void WriteDump(const fs::path& path, const Case& input)
{
  auto d = std::make_unique<DumpFixture>();
  d->header.Signature = MINIDUMP_SIGNATURE;
  d->header.Version = MINIDUMP_VERSION;
  d->header.NumberOfStreams = input.wct.empty() ? 3 : 4;
  d->header.StreamDirectoryRva = offsetof(DumpFixture, streams);
  d->modules.count = static_cast<ULONG32>(1 + input.extra_modules.size());
  Require(d->modules.count <= std::size(d->modules.records), "too many synthetic modules");
  d->streams[0] = { ModuleListStream,
    { static_cast<ULONG32>(offsetof(MINIDUMP_MODULE_LIST, Modules) + d->modules.count * sizeof(MINIDUMP_MODULE)),
      offsetof(DumpFixture, modules) } };
  d->streams[1] = { ExceptionStream, { sizeof(d->exception), offsetof(DumpFixture, exception) } };
  d->streams[2] = { skydiag::protocol::kMinidumpUserStream_Blackbox,
    { offsetof(skydiag::SharedLayout, resources), offsetof(DumpFixture, blackbox) } };
  d->streams[3] = { skydiag::protocol::kMinidumpUserStream_WctJson,
    { static_cast<ULONG32>(input.wct.size()), sizeof(DumpFixture) } };
  const auto addModule = [&](std::size_t i, const DumpModule& module) {
    auto& record = d->modules.records[i];
    auto& name = d->module_names[i];
    Require(module.path.size() < std::size(name.name), "synthetic module path is too long");
    record.BaseOfImage = module.base;
    record.SizeOfImage = 0x1000;
    record.ModuleNameRva = static_cast<RVA>(offsetof(DumpFixture, module_names) + i * sizeof(name));
    name.length = static_cast<ULONG32>(module.path.size() * sizeof(wchar_t));
    std::memcpy(name.name, module.path.data(), name.length);
  };
  addModule(0, { kEarlierModule, kModuleBase });
  for (std::size_t i = 0; i < input.extra_modules.size(); ++i) {
    addModule(i + 1, input.extra_modules[i]);
  }
  d->exception.ThreadId = input.dump_tid;
  d->exception.ExceptionRecord.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
  d->exception.ExceptionRecord.ExceptionAddress = kModuleBase + 0x123;
  d->blackbox.magic = skydiag::kMagic;
  d->blackbox.version = skydiag::kVersion;
  d->blackbox.pid = input.dump_pid;
  static_assert(offsetof(DumpFixture, events) - offsetof(DumpFixture, blackbox) ==
                offsetof(skydiag::SharedLayout, events));
  static_assert(offsetof(DumpFixture, resources) - offsetof(DumpFixture, blackbox) ==
                offsetof(skydiag::SharedLayout, resources));
  if (input.second_fault_timeline) {
    WriteSecondFaultTimeline(*d);
    d->streams[2].Location.DataSize = sizeof(skydiag::SharedLayout);
  } else if (input.crash_timeline) {
    d->blackbox.start_qpc = kTimelineStart;
    d->blackbox.qpc_freq = 1000;
    d->blackbox.crash_seq = 2;
    auto& crash = d->events[0];
    crash.tid = input.crash_marker_tid.value_or(input.dump_tid);
    crash.qpc = kTimelineStart + kFirstFaultMs;
    crash.type = static_cast<std::uint16_t>(skydiag::EventType::kCrash);
    crash.payload.a = EXCEPTION_ACCESS_VIOLATION;
    crash.payload.b = input.crash_marker_address;
    d->blackbox.write_index = 1;
    d->streams[2].Location.DataSize = sizeof(skydiag::SharedLayout);
  }
  if (input.second_fault_stream) {
    d->second_fault = {
      .version = skydiag::second_fault::kVersion,
      .size = sizeof(d->second_fault),
      .tid = d->exception.ThreadId,
      .code = d->exception.ExceptionRecord.ExceptionCode,
      .address = d->exception.ExceptionRecord.ExceptionAddress,
      .qpc = input.second_fault_qpc_unknown ? 0u : kTimelineStart + kSecondFaultMs,
      .a_crash_seq = 2,
      .a_code = EXCEPTION_ILLEGAL_INSTRUCTION,
      .a_address = kModuleBase + 0x321,
      .a_tid = kLogTid,
    };
    if (input.damaged_second_fault_stream) {
      ++d->second_fault.version;
    }
    d->streams[d->header.NumberOfStreams] = {
      skydiag::second_fault::kStreamType, { sizeof(d->second_fault), offsetof(DumpFixture, second_fault) }
    };
    ++d->header.NumberOfStreams;
  }

  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(d.get()), sizeof(*d));
  file << input.wct;
  file.close();
  Require(static_cast<bool>(file), "write synthetic minidump");
}

void SetFixtureLastWrite(const fs::path& path, WORD hour, WORD minute, WORD second, WORD millisecond)
{
  const SYSTEMTIME local{ .wYear = 2097, .wMonth = 7, .wDay = 13,
    .wHour = hour, .wMinute = minute, .wSecond = second, .wMilliseconds = millisecond };
  SYSTEMTIME utc{};
  FILETIME fileTime{};
  Require(TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) && SystemTimeToFileTime(&utc, &fileTime),
          "convert synthetic timestamp");
  const HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  Require(file != INVALID_HANDLE_VALUE, "open fixture to set last-write time");
  const bool written = SetFileTime(file, nullptr, nullptr, &fileTime);
  CloseHandle(file);
  Require(written, "set fixture last-write time");
}

AnalysisResult Analyze(const fs::path& dir, const Case& input, i18n::Language lang)
{
  const auto dump = dir / (input.second_fault_stream ? L"SkyrimDiag_SecondFault_20970713_120000_000.dmp"
    : input.dump_time_from_last_write ? L"Fixture_crash.dmp" : L"Fixture_crash_20970713_120000.dmp");
  WriteDump(dump, input);
  if (input.dump_time_from_last_write) {
    SetFixtureLastWrite(dump, 11, 59, 59, 999);
  }
  if (!input.manifest.empty()) {
    std::ofstream manifest(dir / L"SkyrimDiag_Incident_Crash_20970713_120000.json", std::ios::binary);
    manifest << input.manifest;
    manifest.close();
    Require(static_cast<bool>(manifest), "write synthetic incident manifest");
  }
  if (!input.log_name.empty()) {
    const auto logPath = dir / input.log_name;
    std::ofstream log(logPath, std::ios::binary);
    log << "CRASH TIME: 2097-07-13 12:00:04\r\nCrashLoggerSSE v1.27.0\r\n\r\n";
    if (input.unhandled) {
      log << "Unhandled exception \"EXCEPTION_ACCESS_VIOLATION\" at " << input.address << ' '
          << input.location << "\tcmp qword ptr [rax], 0\r\n";
    }
    log << "\t " << input.thread_line << " \t\r\n\r\n"
        << "\t " << input.process_section << " \t\r\n"
        << "\t " << input.process_line << " \t\r\n\r\n"
        << input.section_tail << "\r\n"
        << "PROBABLE CALL STACK:\r\n";
    log.close();
    Require(static_cast<bool>(log), "write synthetic Crash Logger log");
    if (input.log_time_from_last_write) {
      SetFixtureLastWrite(logPath, 12, 0, 4, 0);
    }
  }
  AnalyzeOptions options{};
  options.language = lang;
  AnalysisResult result;
  std::wstring error;
  Require(AnalyzeDump(dump.wstring(), dir.wstring(), options, result, &error), "analyze synthetic minidump");
  Require(result.pid == input.dump_pid && result.exc_tid == input.dump_tid &&
          result.fault_module_filename == L"Earlier.dll", "read the fixture's dump PID, TID, and module");
  Require(result.crash_logger_log_path.empty() == (input.log_name.empty() || !input.expect_log_pairing),
          "pair the expected fixture log");
  return result;
}

void TestOutput(i18n::Language lang, std::string_view location, std::wstring_view suspectFile)
{
  TempDirectory temp;
  auto r = Analyze(temp.path, Case{ .location = location }, lang);
  Require(r.crash_logger_location_mismatch && r.crash_logger_pairing_ambiguous &&
          r.crash_logger_pairing_confidence == "low", "keep mismatched pairing downgraded for scoring");
  const auto parsed = BuildSummaryJson(r, temp.path, L"fixture", true);
  Require(parsed["crash_logger"].value("process_id", 0u) == kPid, "parse PID in PROCESS INFO with CRLF and whitespace");
  Require(parsed["crash_logger"].value("exception_tid", 0u) == kLogTid, "parse exception TID with CRLF and whitespace");
  Require(parsed["analysis"].value("crash_logger_later_crash", false), "recognize later crash in the same process");

  r.signature_match.emplace();
  r.signature_match->id = "EARLIER_EXCEPTION_SIGNATURE";
  r.signature_match->scope = "root_cause";
  r.signature_match->cause = L"A known cause for the earlier exception.";
  r.signature_match->recommendations = { L"[Known cause] Change the earlier DLL." };
  r.fault_instruction.verdict = "null_pointer";
  auto dumpOnly = r;
  dumpOnly.crash_logger_pairing_log_before_dump = true;
  BuildEvidenceAndSummary(dumpOnly, lang);
  Require(dumpOnly.quick_answer.case_id == "known_issue" && !dumpOnly.quick_answer.how_it_crashed.empty(),
          "without a later crash the known signature and dump instruction are shown");

  BuildEvidenceAndSummary(r, lang);
  const auto summary = BuildSummaryJson(r, temp.path, L"fixture", true);
  const bool en = lang == i18n::Language::kEnglish;
  const std::string_view tag = en ? "[Different crash from dump]" : "[덤프와 다른 크래시]";
  const auto& answer = r.quick_answer;
  Require(answer.kind == "crash" && answer.case_id == "crash_logger_later_crash" &&
          answer.likelihood == "low", "later crash takes priority over a known dump signature");
  Require(answer.headline.find(r.crash_logger_exception_location) != std::wstring::npos &&
          answer.headline.find(L"Earlier Example") != std::wstring::npos, "headline names both exceptions");
  Require(answer.headline.find(en ? L"4 seconds" : L"4초") != std::wstring::npos, "headline includes elapsed seconds");
  Require(answer.how_it_crashed.empty(), "no dump instruction in the quick answer");
  Require(answer.suspect_file == suspectFile && answer.suspect_mod.empty() == suspectFile.empty(),
          "only the Crash Logger fault DLL can be a suspect");
  const std::wstring logFile(Case{}.log_name);
  const auto action = en
    ? L"Check the relevant objects and the call stack for that crash in the Crash Logger log (" + logFile +
      L"), and share that log when asking for help."
    : L"Crash Logger 로그(" + logFile +
      L")에서 그 크래시의 관련 오브젝트와 콜스택을 확인하고, 도움을 요청할 때 그 로그를 함께 공유하세요.";
  Require(answer.action.starts_with(action), "action points to the paired log");
  Require(answer.why_not_certain == (en
    ? L"This dump does not hold the crash that ended the game, so that crash is seen only through the Crash Logger log."
    : L"이 덤프에는 게임을 끝낸 크래시가 담겨 있지 않아, 그 크래시는 Crash Logger 로그로만 봤습니다."),
    "explain which crash the dump holds");
  Require(summary["summary_sentence"].get<std::string>().starts_with(tag), "later crash leads the conclusion");
  Require(summary["summary_sentence"].get<std::string>().find("Earlier.dll+0x123") != std::string::npos,
          "conclusion identifies the dump location");
  const std::wstring summaryTransition = en
    ? L"(Crash Logger log, same process). The analysis below is of the earlier exception in this dump. "
    : L"(Crash Logger 로그, 같은 프로세스). 아래는 이 덤프에 찍힌 앞선 예외의 분석입니다. ";
  Require(r.summary_sentence.ends_with(summaryTransition + dumpOnly.summary_sentence),
          "identify the earlier exception analysis before the original conclusion");
  if (!suspectFile.empty()) {
    Require(answer.headline.ends_with(L" That location is in Later.dll (Later.dll)."),
            "name the mod at the Crash Logger location");
  }
  Require(!r.evidence.empty() && r.evidence.front().confidence_level == i18n::ConfidenceLevel::kMedium &&
          r.evidence.front().details.find(r.crash_logger_exception_location) != std::wstring::npos,
          "later crash leads the evidence with medium confidence");
  Require(summary["next_action_index"] == 0 && summary["recommendations"][0].get<std::string>().starts_with(tag),
          "later crash recommendation leads ahead of the known cause");
  Require(BuildReportText(r, summary, true).find(", later_crash=1\n") != std::string::npos,
          "report appends later_crash to the pairing line");

  r.crash_logger_pairing_time_delta_ms = 4500;
  BuildEvidenceAndSummary(r, lang);
  Require(r.quick_answer.headline.find(en ? L"5 seconds" : L"5초") != std::wstring::npos,
          "round the pairing delay to the nearest second");
  r.crash_logger_pairing_time_delta_ms = 499;
  BuildEvidenceAndSummary(r, lang);
  const auto fewSeconds = en ? L"within a few seconds" : L"몇 초 안에";
  Require(r.quick_answer.headline.find(fewSeconds) != std::wstring::npos &&
          r.summary_sentence.find(fewSeconds) != std::wstring::npos,
          "describe a positive sub-half-second delay in plain words");
}

void TestRejectedInputs()
{
  const struct
  {
    std::string_view name;
    Case input;
    ULONG32 expected_pid = kPid;
    ULONG32 expected_tid = kLogTid;
  } cases[] = {
    { "different process", { .process_line = "Process ID: 786" }, 786 },
    { "older log", { .log_name = L"crash-2097-07-13-11-59-56.log" } },
    { "same timestamp", { .log_name = L"crash-2097-07-13-12-00-00.log" } },
    { "same thread", { .thread_line = "Exception Thread ID: 35" }, kPid, kDumpTid },
    { "same fault location", { .location = "Earlier.dll+123" } },
    { "no unhandled exception", { .unhandled = false } },
    { "freeze", { .wct = R"({"threads":[{"isCycle":true}],"capture":{"kind":"hang"}})" } },
    { "freeze without a thread list", { .wct = R"({"capture":{"kind":"hang"}})" } },
    { "manual capture", { .wct = R"({"threads":[],"capture":{"kind":"manual"}})" } },
    { "missing log", { .log_name = L"" }, 0, 0 },
    { "missing dump PID", { .dump_pid = 0 } },
    { "missing dump thread ID", { .dump_tid = 0 } },
    { "missing log PID", { .process_line = "" }, 0 },
    { "missing log thread ID", { .thread_line = "" }, kPid, 0 },
    { "nonnumeric IDs", { .thread_line = "Exception Thread ID: 56oops", .process_line = "Process ID: NaN" }, 0, 0 },
    { "overflowing IDs", { .thread_line = "Exception Thread ID: 4294967296", .process_line = "Process ID: 4294967296" }, 0, 0 },
    { "PID outside PROCESS INFO", { .process_section = "PROCESS INFO:\r\nSYSTEM SPECS:" }, 0 },
    { "exception thread ID after the first section starts",
      { .thread_line = "ACCESS VIOLATION ANALYSIS:\r\nException Thread ID: 56" }, kPid, 0 },
    { "conflicting crash thread ID in PROCESS INFO",
      { .section_tail = "Crash Thread ID: 57" }, kPid, 0 },
    { "two unhandled exceptions",
      { .section_tail = "Unhandled exception \"EXCEPTION_ACCESS_VIOLATION\" at 0x180005000 Other.dll+123" }, 0, 0 },
    { "malformed first IDs followed by valid ones",
      { .thread_line = "Exception Thread ID: bad\r\nException Thread ID: 56",
        .process_line = "Process ID: bad\r\nProcess ID: 785" }, 0, 0 },
  };
  for (const auto& [name, input, expectedPid, expectedTid] : cases) {
    TempDirectory temp;
    const auto r = Analyze(temp.path, input, i18n::Language::kEnglish);
    const auto summary = BuildSummaryJson(r, temp.path, L"fixture", true);
    Require(summary["analysis"].value("crash_logger_later_crash", true) == false &&
            r.quick_answer.case_id != "crash_logger_later_crash", name);
    const auto& crashLogger = summary["crash_logger"];
    Require(crashLogger.value("process_id", 0u) == expectedPid &&
            crashLogger.contains("process_id") == (expectedPid != 0), name);
    Require(crashLogger.value("exception_tid", 0u) == expectedTid &&
            crashLogger.contains("exception_tid") == (expectedTid != 0), name);
  }
}

void TestPairingTime()
{
  const struct
  {
    Case input;
    std::uint64_t expected_delta;
    bool expected_later_crash;
  } cases[] = {
    { { .log_name = L"crash-renamed.log", .log_time_from_last_write = true }, 4000, false },
    { { .log_name = L"crash-2097-07-13-12-00-30.log" }, 30000, true },
    { { .log_name = L"crash-2097-07-13-12-00-30.log", .dump_time_from_last_write = true }, 30001, false },
  };
  for (const auto& [input, expectedDelta, expectedLaterCrash] : cases) {
    TempDirectory temp;
    const auto r = Analyze(temp.path, input, i18n::Language::kEnglish);
    Require(!r.crash_logger_pairing_log_before_dump && r.crash_logger_pairing_time_delta_ms == expectedDelta,
            "pair the later log at the expected timestamp");
    Require(r.crash_logger_later_crash == expectedLaterCrash,
            "require a log filename timestamp and a delay of at most 30000 ms");
  }
}

void TestUnhandledSignal()
{
  TempDirectory temp;
  const auto r = Analyze(temp.path, Case{ .manifest =
    R"({"capture_kind":"crash","context":{"unhandled_exception_signal":true}})" }, i18n::Language::kEnglish);
  Require(r.incident_capture_kind == "crash", "read the synthetic incident manifest");
  Require(!r.crash_logger_later_crash, "reject a dump that reached the unhandled exception filter");
}

void TestExceptionModules()
{
  constexpr DumpModule gameMod{ L"C:\\Fixture\\Skyrim\\d3d11.dll", kModuleBase + 0x4000 };
  constexpr DumpModule systemMod{ L"C:\\Windows\\System32\\d3d11.dll", kModuleBase + 0x4000 };
  constexpr DumpModule duplicates[] = {
    gameMod, { systemMod.path, kModuleBase + 0x5000 }
  };
  constexpr DumpModule eligibleCopies[] = {
    gameMod, { L"C:\\Fixture\\Other\\D3D11.DLL", kModuleBase + 0x5000 }
  };
  constexpr DumpModule runtime{ L"C:\\Fixture\\Skyrim\\hostfxr.dll", kModuleBase + 0x4000 };
  constexpr DumpModule driver{ L"C:\\Fixture\\Skyrim\\nvwgf2umx.dll", kModuleBase + 0x4000 };
  const struct
  {
    std::string_view name;
    Case input;
    bool expected_mod;
  } cases[] = {
    { "address of game-folder DLL", { .location = "d3d11.dll+321", .extra_modules = { &gameMod, 1 } }, true },
    { "address of System32 DLL", { .location = "d3d11.dll+321", .extra_modules = { &systemMod, 1 } }, false },
    { "address of game-folder DLL with a same-name System32 copy",
      { .location = "d3d11.dll+321", .extra_modules = duplicates }, true },
    { "address of System32 copy with a same-name mod",
      { .location = "d3d11.dll+321", .address = "0X180005321", .extra_modules = duplicates }, false },
    { "unmapped address and mixed copies",
      { .location = "d3d11.dll+321", .address = "0x180006321", .extra_modules = duplicates }, false },
    { "malformed address and mixed copies",
      { .location = "d3d11.dll+321", .address = "0x180004321oops", .extra_modules = duplicates }, false },
    { "unmapped address and two eligible copies",
      { .location = "d3d11.dll+321", .address = "0x180006321", .extra_modules = eligibleCopies }, true },
    { "address of runtime DLL", { .location = "hostfxr.dll+321", .extra_modules = { &runtime, 1 } }, false },
    { "unlisted runtime DLL", { .location = "hostfxr.dll+321" }, false },
    { "address of graphics driver", { .location = "nvwgf2umx.dll+321", .extra_modules = { &driver, 1 } }, false },
    { "unlisted graphics driver", { .location = "nvwgf2umx.dll+321" }, false },
  };
  for (const auto& [name, input, expectedMod] : cases) {
    TempDirectory temp;
    const auto r = Analyze(temp.path, input, i18n::Language::kEnglish);
    Require(r.crash_logger_later_crash, "recognize the later crash independently of its module");
    Require(r.crash_logger_exception_module_is_mod == expectedMod &&
            r.quick_answer.suspect_mod.empty() == !expectedMod, name);
  }
}

void TestSameDll(i18n::Language lang)
{
  TempDirectory temp;
  const auto r = Analyze(temp.path, Case{ .location = "eArLiEr.dll+321", .address = "0x180000321" }, lang);
  const auto& answer = r.quick_answer;
  const bool en = lang == i18n::Language::kEnglish;
  Require(r.crash_logger_later_crash && answer.suspect_file == L"eArLiEr.dll" &&
          answer.suspect_mod == L"Earlier Example", "identify the shared DLL as the later crash suspect");
  Require(answer.action.find(en ? L"as the cause" : L"원인으로 보지") == std::wstring::npos,
          "omit the dismissal of the shared DLL");
  Require(answer.headline.ends_with(en ? L" That location is in Earlier Example (eArLiEr.dll)."
                                       : L" 그 위치는 Earlier Example(eArLiEr.dll)입니다."),
          "end the headline with the mod display name and file");
}

void TestOutputExclusions()
{
  TempDirectory temp;
  const auto analyzed = Analyze(temp.path, Case{}, i18n::Language::kEnglish);
  const auto check = [](AnalysisResult r, const auto& edit, std::string_view expectedKind) {
    edit(r);
    BuildEvidenceAndSummary(r, i18n::Language::kEnglish);
    Require(!r.crash_logger_later_crash && r.quick_answer.kind == expectedKind &&
            r.quick_answer.case_id != "crash_logger_later_crash", expectedKind);
  };
  check(analyzed, [](AnalysisResult& r) { r.is_filtered_clean_exit = true; }, "clean_exit");
  check(analyzed, [](AnalysisResult& r) { r.forced_close.detected = true; }, "forced_close");
  check(analyzed, [](AnalysisResult& r) { r.quit_before_freeze.detected = true; }, "slow_shutdown");
  check(analyzed, [](AnalysisResult& r) {
    r.dump_path = L"snapshot.dmp";
    r.exc_code = 0;
  }, "snapshot");
  check(analyzed, [](AnalysisResult& r) { r.crash_logger_exception_type.clear(); }, "crash");
  // Another log of the same process near the dump time: the pairing could have picked the wrong one.
  check(analyzed, [](AnalysisResult& r) { r.crash_logger_pairing_nearby_competitor_count = 1; }, "crash");
}

void TestSecondFaultIncident(i18n::Language lang)
{
  TempDirectory baselineDir;
  const auto baseline = Analyze(baselineDir.path, Case{ .log_name = L"" }, lang);
  constexpr auto stem = L"Fixture_crash_20970713_120000";
  const auto original = BuildSummaryJson(baseline, baselineDir.path, stem, true);
  Require(!original.contains("incident") && fs::is_regular_file(baselineDir.path / "crash_history.json"),
          "the original capture writes history and has no second-fault metadata");

  constexpr std::uint64_t unmappedAddress = 0xABCDEF;
  constexpr std::string_view filename = "SkyrimDiag_SecondFault_20970713_120000_000.dmp";
  constexpr std::string_view noDumpReason = "dump_write_failed";
  const bool en = lang == i18n::Language::kEnglish;
  struct IncidentCase
  {
    bool hasDump;
    bool resumed;
    bool present;
  };
  for (const auto [hasDump, resumed, present] :
       { IncidentCase{ true, false, true }, { false, false, false }, { true, true, true }, { true, false, false } }) {
    TempDirectory temp;
    if (present) {
      std::ofstream(temp.path / filename, std::ios::binary) << "dump";
    }
    nlohmann::json fault = {
      { "exception_code", EXCEPTION_ACCESS_VIOLATION },
      { "exception_addr", hasDump ? kModuleBase + 0x321 : unmappedAddress },
      { "faulting_tid", kLogTid },
      { "resumed", resumed },
    };
    if (hasDump) {
      fault["dump_filename"] = filename;
    } else {
      fault["reason"] = noDumpReason;
    }
    const nlohmann::json manifest = {
      { "capture_kind", "crash" },
      { "context", { { "second_fault", fault } } },
    };
    const auto serialized = manifest.dump();
    auto r = Analyze(temp.path, Case{ .log_name = L"", .manifest = serialized }, lang);
    const auto summary = BuildSummaryJson(r, temp.path, stem, true);
    Require(summary["incident"]["second_fault"] == fault, "copy second_fault from the incident to the summary");
    // ADR-0036 addendum: B's dump, there and with no resume, leads
    // (TestSecondFaultLead); without one, A's analysis is as it was.
    const bool lead = hasDump && !resumed && present;
    Require(r.second_fault_lead == lead, "lead only with B's dump beside A's and no resume");
    for (const char* field : { "quick_answer", "summary_sentence", "actionable_candidates",
                               "crash_bucket_key", "next_action_index", "analysis" }) {
      const std::string_view name(field);
      if (!lead) {
        Require(summary[field] == original[field], field);
      } else if (name == "actionable_candidates" || name == "crash_bucket_key") {
        Require(summary[field] == original[field], field);
      } else if (name == "next_action_index") {
        Require(summary[field] == 0, field);
      } else {
        Require(summary[field] != original[field], field);
      }
    }
    Require(r.evidence.size() == baseline.evidence.size() + 1 &&
            r.recommendations.size() == baseline.recommendations.size() + 1,
            "add one evidence item and one recommendation");
    const auto report = BuildReportText(r, summary, true);
    Require(report.find(en ? "reached the top-level exception filter unhandled" : "최상위 예외 필터에 닿았습니다") !=
              std::string::npos, "identify the second exception in the report");
    Require(report.find(en ? "thread 56" : "스레드 56") != std::string::npos,
            "give the second exception's thread");
    const bool reportsResume = report.find(en ? "Execution resumed after that exception"
                                               : "그 예외는 그 뒤 실행이 이어졌습니다") != std::string::npos;
    Require(reportsResume == resumed, "mention that execution resumed only when recorded");
    // Last, or first as the lead.
    const auto& item = lead ? r.evidence.front() : r.evidence.back();
    Require(item.title == (en ? L"Second exception reached the top-level filter" : L"최상위 예외 필터에 닿은 두 번째 예외"),
            "the second-fault item is last, or first as the lead");
    if (hasDump) {
      Require(report.find(en ? "from this dump's module list" : "이 덤프의 모듈 목록 기준") != std::string::npos &&
              item.details.find(L"Earlier.dll+0x321") != std::wstring::npos,
              "resolve the second address against the first dump and say where it came from");
      Require(report.find(filename) != std::string::npos, "name the second dump");
      const bool directed = report.find(en ? "in the viewer" : "뷰어에서") != std::string::npos;
      const bool gone = report.find(en ? "not in this folder any more" : "지금은 이 폴더에 없습니다") != std::string::npos;
      Require(directed == present && gone == !present,
              "direct the reader to the second dump only while it is there");
    } else {
      Require(item.details.find(L"0xABCDEF") != std::wstring::npos &&
              report.find(noDumpReason) == std::string::npos &&
              report.find(en ? "writing the dump failed" : "덤프 쓰기에 실패했습니다") != std::string::npos &&
              report.find(en ? "No dump was saved" : "덤프는 남기지 못했습니다") != std::string::npos,
              "give the raw address and explain in words why the second dump is missing");
    }
    if (lead) {
      continue;
    }
    Require(r.second_fault_note_last, "the note is the last recommendation");
    r.recommendations = { en ? L"[Basics] Original note." : L"[기본] 원래 문구.", r.recommendations.back() };
    Require(BuildSummaryJson(r, temp.path, stem, true)["next_action_index"] == 0,
            "the appended note does not displace the background fallback");
  }
}

constexpr std::string_view kSecondDump = "SkyrimDiag_SecondFault_20970713_120000_000.dmp";
constexpr std::wstring_view kSecondDumpW = L"SkyrimDiag_SecondFault_20970713_120000_000.dmp";
constexpr auto kFirstStem = L"Fixture_crash_20970713_120000";

nlohmann::json SecondFault(ULONG32 tid, std::uint64_t address = kModuleBase + 0x321)
{
  return {
    { "exception_code", EXCEPTION_ACCESS_VIOLATION },
    { "exception_addr", address },
    { "faulting_tid", tid },
    { "resumed", false },
    { "dump_filename", kSecondDump },
  };
}

// The record the helper writes beside B's dump (CrashCapture.cpp). In place:
// a json converts to std::optional<json> in two ways.
std::optional<nlohmann::json> SecondFaultRecord(
  ULONG32 tid, std::uint64_t qpc, std::uint64_t firstAddress = kModuleBase + 0x123, ULONG32 firstTid = kDumpTid)
{
  auto record = SecondFault(tid);
  record["schema"] = "skydiag.second_fault.v1";
  record["qpc"] = qpc;
  record["dropped"] = 0;
  record["a"] = { { "crash_seq", 2 }, { "exception_code", EXCEPTION_ACCESS_VIOLATION },
                  { "exception_addr", firstAddress }, { "faulting_tid", firstTid } };
  return std::optional<nlohmann::json>(std::in_place, std::move(record));
}

AnalysisResult AnalyzeWithSecondFault(
  const fs::path& dir, Case input, const nlohmann::json& fault, const std::optional<nlohmann::json>& record,
  i18n::Language lang, bool unhandledSignal = false)
{
  std::ofstream(dir / kSecondDump, std::ios::binary) << "dump";
  if (record) {
    std::ofstream(dir / L"SkyrimDiag_SecondFault_20970713_120000_000.json", std::ios::binary) << record->dump();
  }
  nlohmann::json context = { { "second_fault", fault } };
  if (unhandledSignal) {
    context["unhandled_exception_signal"] = true;
  }
  const auto manifest = nlohmann::json{ { "capture_kind", "crash" }, { "context", context } }.dump();
  input.manifest = manifest;
  return Analyze(dir, input, lang);
}

std::size_t CountOf(std::wstring_view text, std::wstring_view part)
{
  std::size_t count = 0;
  for (auto at = text.find(part); at != std::wstring_view::npos; at = text.find(part, at + part.size())) {
    ++count;
  }
  return count;
}

// ADR-0036 addendum: with B's own dump, the quick answer, the conclusion and
// the first recommendation say that this dump holds an earlier exception and
// point to B's dump; B's delay comes from its record on A's blackbox clock.
void TestSecondFaultLead(i18n::Language lang)
{
  const bool en = lang == i18n::Language::kEnglish;
  const std::wstring file(kSecondDumpW);
  const std::wstring tag = en ? L"[Different crash from dump] " : L"[덤프와 다른 크래시] ";
  const auto delayed = SecondFaultRecord(kLogTid, kTimelineStart + kSecondFaultMs);
  TempDirectory temp;
  auto r = AnalyzeWithSecondFault(temp.path, Case{ .log_name = L"", .crash_timeline = true },
                                  SecondFault(kLogTid), delayed, lang);
  const auto& answer = r.quick_answer;
  Require(r.second_fault_lead && answer.kind == "crash" && answer.case_id == "second_fault_crash" &&
          answer.likelihood == "low", "B's dump leads the quick answer");
  Require(answer.headline.find(file) != std::wstring::npos &&
          answer.headline.find(L"Earlier.dll+0x321") != std::wstring::npos,
          "the headline names B's location and dump");
  Require(answer.headline.find(en ? L"on another thread about 4 seconds before" : L"그보다 약 4초 앞서 다른 스레드에서") !=
            std::wstring::npos, "the headline gives the thread and B's delay from its record");
  Require(answer.suspect_file == L"Earlier.dll" && answer.suspect_mod == L"Earlier Example" &&
          answer.headline.ends_with(en ? L" That location is in Earlier Example (Earlier.dll)."
                                       : L" 그 위치는 Earlier Example(Earlier.dll)입니다."),
          "B's module is the suspect");
  const std::wstring action = en ? L"Open " + file + L" in the viewer to see the analysis of that crash."
                                 : L"뷰어에서 " + file + L" 파일을 열어 그 크래시의 분석을 확인하세요.";
  Require(answer.action == action, "open B's dump; no Crash Logger log, and the same DLL is not dismissed");
  Require(answer.why_not_certain.find(en ? L"its own dump" : L"따로 남은 덤프") != std::wstring::npos &&
          answer.how_it_crashed.empty(), "say where that crash is examined");
  Require(r.summary_sentence.starts_with(tag + (en ? L"The exception in this dump (Earlier.dll+0x123)"
                                                   : L"이 덤프의 예외(Earlier.dll+0x123)")) &&
          r.summary_sentence.find(en ? L"about 4 seconds later on another thread at Earlier.dll+0x321 (its own dump: "
                                     : L"약 4초 뒤 다른 스레드에서 난 Earlier.dll+0x321입니다(그 덤프: ") !=
            std::wstring::npos && CountOf(r.summary_sentence, tag) == 1,
          "the conclusion leads with B once");
  const auto summary = BuildSummaryJson(r, temp.path, kFirstStem, true);
  Require(summary["next_action_index"] == 0 && r.recommendations.front() == tag + action &&
          !r.second_fault_note_last, "B's dump is the first recommendation and the next action");
  Require(std::count_if(r.recommendations.begin(), r.recommendations.end(), [&](const std::wstring& line) {
            return line.find(file) != std::wstring::npos;
          }) == 1, "one recommendation names B's dump");
  Require(summary["analysis"].value("second_fault_lead", false), "flag the lead in the summary");
  Require(!r.evidence.empty() &&
          r.evidence.front().title == (en ? L"Second exception reached the top-level filter"
                                          : L"최상위 예외 필터에 닿은 두 번째 예외"),
          "the second-fault item leads the evidence");
  Require(BuildReportText(r, summary, true).find(WideToUtf8(L"NextAction: " + action)) != std::string::npos,
          "the report's next action opens B's dump");

  {
    TempDirectory same;
    const auto sameThread = AnalyzeWithSecondFault(same.path, Case{ .log_name = L"", .crash_timeline = true },
      SecondFault(kDumpTid), SecondFaultRecord(kDumpTid, kTimelineStart + kSecondFaultMs), lang);
    Require(sameThread.second_fault_lead &&
            sameThread.quick_answer.headline.find(en ? L"on the same thread about 4 seconds" : L"약 4초 앞서 같은 스레드에서") !=
              std::wstring::npos &&
            sameThread.summary_sentence.find(en ? L"later on the same thread" : L"뒤 같은 스레드에서") != std::wstring::npos,
            "B on A's own thread leads too, and says so");
  }

  // No usable record: no number of seconds.
  const auto edited = [](const auto& edit) {
    auto record = SecondFaultRecord(kLogTid, kTimelineStart + kSecondFaultMs);
    edit(*record);
    return record;
  };
  const Case timed{ .log_name = L"", .crash_timeline = true };
  const struct
  {
    std::string_view name;
    Case input;
    std::optional<nlohmann::json> record;
  } untimed[] = {
    { "no record", timed, std::nullopt },
    { "record of another thread", timed, SecondFaultRecord(kDumpTid, kTimelineStart + kSecondFaultMs) },
    { "record of another second address", timed,
      edited([](nlohmann::json& j) { j["exception_addr"] = kModuleBase + 0x999; }) },
    { "record of another first exception", timed,
      SecondFaultRecord(kLogTid, kTimelineStart + kSecondFaultMs, kModuleBase + 0x999) },
    { "record of another first thread", timed,
      SecondFaultRecord(kLogTid, kTimelineStart + kSecondFaultMs, kModuleBase + 0x123, 99) },
    { "record of another schema", timed, edited([](nlohmann::json& j) { j["schema"] = "skydiag.second_fault.v0"; }) },
    { "record without the first exception", timed, edited([](nlohmann::json& j) { j.erase("a"); }) },
    { "record with a null first address", timed,
      edited([](nlohmann::json& j) { j["a"]["exception_addr"] = nullptr; }) },
    { "record with a text thread", timed, edited([](nlohmann::json& j) { j["faulting_tid"] = "56"; }) },
    { "record without a time", timed, edited([](nlohmann::json& j) { j["qpc"] = 0; }) },
    { "record before the session", timed, edited([](nlohmann::json& j) { j["qpc"] = kTimelineStart - 1; }) },
    { "record before the first exception", timed, SecondFaultRecord(kLogTid, kTimelineStart + kFirstFaultMs - 1) },
    { "marker of another exception",
      { .log_name = L"", .crash_timeline = true, .crash_marker_address = kModuleBase + 0x777 }, delayed },
    { "marker of another thread", { .log_name = L"", .crash_timeline = true, .crash_marker_tid = 99u }, delayed },
    { "no crash marker", { .log_name = L"" }, delayed },
  };
  for (const auto& [name, input, record] : untimed) {
    TempDirectory dir;
    const auto u = AnalyzeWithSecondFault(dir.path, input, SecondFault(kLogTid), record, lang);
    Require(u.second_fault_lead && !u.incident_second_fault->seconds_after_first &&
            u.quick_answer.headline.find(en ? L"on another thread before the crash" : L"그보다 앞서 다른 스레드에서") !=
              std::wstring::npos &&
            u.summary_sentence.find(en ? L"happened after it on another thread" : L"크래시는 그 뒤 다른 스레드에서") !=
              std::wstring::npos, name);
  }
  {
    TempDirectory dir;
    std::ofstream(dir.path / L"SkyrimDiag_SecondFault_20970713_120000_000.json", std::ios::binary) << "{\"schema\":";
    const auto u = AnalyzeWithSecondFault(dir.path, timed, SecondFault(kLogTid), std::nullopt, lang);
    Require(u.second_fault_lead && !u.incident_second_fault->seconds_after_first, "a torn record gives no time");
  }
  {
    // A record past 64 KB is no record of the helper's, even if it parses.
    TempDirectory dir;
    std::ofstream(dir.path / L"SkyrimDiag_SecondFault_20970713_120000_000.json", std::ios::binary)
      << delayed->dump() << std::string(70000, ' ');
    const auto u = AnalyzeWithSecondFault(dir.path, timed, SecondFault(kLogTid), std::nullopt, lang);
    Require(u.second_fault_lead && !u.incident_second_fault->seconds_after_first, "an oversized record gives no time");
  }
  {
    // A resume in a record of another pair is not B's.
    TempDirectory dir;
    auto other = SecondFaultRecord(kDumpTid, kTimelineStart + kSecondFaultMs);
    (*other)["resumed"] = true;
    const auto u = AnalyzeWithSecondFault(dir.path, timed, SecondFault(kLogTid), other, lang);
    Require(u.second_fault_lead && !u.incident_second_fault->resumed, "a resume in another pair's record is ignored");
  }
  {
    // A folder where B's dump should be is no dump to open.
    TempDirectory dir;
    fs::create_directory(dir.path / kSecondDump);
    const auto u = AnalyzeWithSecondFault(dir.path, timed, SecondFault(kLogTid), delayed, lang);
    Require(!u.second_fault_lead && !u.incident_second_fault->dump_present && !u.incident_second_fault->dump_missing &&
            u.quick_answer.case_id != "second_fault_crash", "a folder is not B's dump");
  }
  {
    // The helper recorded after A's incident that execution resumed after B.
    TempDirectory dir;
    const auto u = AnalyzeWithSecondFault(dir.path, timed, SecondFault(kLogTid),
                                          edited([](nlohmann::json& j) { j["resumed"] = true; }), lang);
    Require(!u.second_fault_lead && u.incident_second_fault->resumed && u.quick_answer.case_id != "second_fault_crash" &&
            u.evidence.back().details.find(en ? L"Execution resumed after that exception"
                                              : L"그 예외는 그 뒤 실행이 이어졌습니다") != std::wstring::npos,
            "a resume in B's record keeps A's analysis in the lead");
  }
}

// ADR-0036 addendum: B in another mod, outside any module, or in Windows;
// this dump's thread unknown; the lead as the only recommendation; the note on
// an answer that does not start with its kind.
void TestSecondFaultLeadTexts(i18n::Language lang)
{
  const bool en = lang == i18n::Language::kEnglish;
  const std::wstring dismiss = en ? L" Do not treat Earlier Example from this dump as the cause of the crash."
                                  : L" 덤프에 찍힌 Earlier Example은(는) 이 크래시의 원인으로 보지 마세요.";
  {
    constexpr DumpModule later{ L"C:\\Fixture\\mods\\Later Example\\SKSE\\Plugins\\Later.dll", kModuleBase + 0x4000 };
    TempDirectory temp;
    const auto r = AnalyzeWithSecondFault(temp.path, Case{ .extra_modules = { &later, 1 }, .log_name = L"" },
                                          SecondFault(kLogTid, kModuleBase + 0x4321), std::nullopt, lang);
    Require(r.second_fault_lead && r.quick_answer.suspect_mod == L"Later Example" &&
            r.quick_answer.suspect_file == L"Later.dll" &&
            r.quick_answer.headline.find(L"Later.dll+0x321") != std::wstring::npos &&
            r.quick_answer.action.ends_with(dismiss), "B in another mod is the suspect, and A's mod is set aside");
  }
  {
    constexpr DumpModule windows{ L"C:\\Windows\\System32\\d3d11.dll", kModuleBase + 0x4000 };
    TempDirectory temp;
    const auto r = AnalyzeWithSecondFault(temp.path, Case{ .extra_modules = { &windows, 1 }, .log_name = L"" },
                                          SecondFault(kLogTid, kModuleBase + 0x4321), std::nullopt, lang);
    Require(r.second_fault_lead && r.quick_answer.suspect_mod.empty() && r.quick_answer.suspect_file.empty() &&
            r.quick_answer.headline.find(L"d3d11.dll+0x321") != std::wstring::npos &&
            r.quick_answer.action.ends_with(dismiss), "B in Windows names no mod");
  }
  {
    TempDirectory temp;
    const auto r = AnalyzeWithSecondFault(temp.path, Case{ .log_name = L"" }, SecondFault(kLogTid, 0xABCDEF),
                                          std::nullopt, lang);
    Require(r.second_fault_lead && r.quick_answer.suspect_mod.empty() &&
            r.quick_answer.headline.find(L"0xABCDEF") != std::wstring::npos &&
            r.summary_sentence.find(L"0xABCDEF") != std::wstring::npos && r.quick_answer.action.ends_with(dismiss),
            "B outside any module is given by its address");
  }
  {
    TempDirectory temp;
    auto r = AnalyzeWithSecondFault(temp.path, Case{ .log_name = L"", .dump_tid = 0 }, SecondFault(kLogTid),
                                    std::nullopt, lang);
    Require(r.second_fault_lead &&
            r.quick_answer.headline.find(en ? L"the game was handling before the crash" : L"그보다 앞서 나서 게임이") !=
              std::wstring::npos &&
            r.quick_answer.headline.find(en ? L"thread" : L"스레드") == std::wstring::npos &&
            r.summary_sentence.find(en ? L"happened after it at " : L"크래시는 그 뒤 난 ") != std::wstring::npos,
            "with this dump's thread unknown, say no thread");
    r.recommendations = { r.recommendations.front() };
    Require(BuildSummaryJson(r, temp.path, kFirstStem, true)["next_action_index"] == 0,
            "the lead is the next action even as the only recommendation");
  }
  QuickAnswer answer;
  answer.kind = "forced_close";
  answer.headline = L"Closed.";
  internal::MarkSecondFaultDumpAnswer(answer, en);
  Require(answer.headline.starts_with(en ? L"This is the separate dump" : L"이 덤프는 Tullius가") &&
          answer.headline.ends_with(L" Closed."), "an answer without its kind phrase starts with the note");
}

// ADR-0036 addendum: a Crash Logger log of B is named as B's, and with the
// later crash it shows (another thread) the lead is still said once.
void TestSecondFaultLeadCrashLogger(i18n::Language lang)
{
  const bool en = lang == i18n::Language::kEnglish;
  const std::wstring tag = en ? L"[Different crash from dump] " : L"[덤프와 다른 크래시] ";
  const std::wstring logFile(Case{}.log_name);
  const std::wstring sameCrash = en ? L" The Crash Logger log (" + logFile + L") recorded the same crash."
                                    : L" Crash Logger 로그(" + logFile + L")도 같은 크래시를 기록했습니다.";
  const auto crashLoggerItem = [en](const AnalysisResult& r) {
    const auto item = std::find_if(r.evidence.begin(), r.evidence.end(), [en](const EvidenceItem& e) {
      return e.title == (en ? L"Crash Logger SSE/AE log auto-detected" : L"Crash Logger SSE/AE 로그를 자동으로 찾음");
    });
    Require(item != r.evidence.end(), "keep the Crash Logger log item");
    return item->details;
  };
  const std::wstring_view isSecond = en ? L"but that of the second exception" : L"두 번째 예외의 위치라";
  const std::wstring_view otherCrash = en ? L"it may be another crash's log" : L"다른 크래시의 로그일 수 있어";
  {
    // Another thread, as Ctrl+Shift+F5 does: the rc1 later crash holds too.
    TempDirectory temp;
    const auto r = AnalyzeWithSecondFault(temp.path, Case{ .location = "Earlier.dll+321", .address = "0x180000321" },
                                          SecondFault(kLogTid), std::nullopt, lang);
    Require(r.crash_logger_later_crash && r.second_fault_lead && r.incident_second_fault->crash_logger_log_is_second &&
            r.quick_answer.case_id == "second_fault_crash", "B's dump leads over the later crash Crash Logger shows");
    Require(r.quick_answer.action.find(sameCrash) != std::wstring::npos, "say the Crash Logger log is of the same crash");
    Require(CountOf(r.summary_sentence, tag) == 1 &&
            std::count_if(r.recommendations.begin(), r.recommendations.end(), [&](const std::wstring& line) {
              return line.starts_with(tag);
            }) == 1, "one lead in the conclusion and the recommendations");
    Require(std::none_of(r.evidence.begin(), r.evidence.end(), [en](const EvidenceItem& e) {
              return e.title == (en ? L"Different crash from dump" : L"덤프와 다른 크래시");
            }), "the Crash Logger later-crash item gives way to the second-fault item");
    Require(crashLoggerItem(r).find(isSecond) != std::wstring::npos &&
            crashLoggerItem(r).find(otherCrash) == std::wstring::npos, "the log is B's, not maybe another crash's");
    Require(r.evidence.front().details.find(en ? L"is of that exception" : L"그 예외의 것입니다") != std::wstring::npos,
            "the second-fault item names the Crash Logger log");
  }
  {
    // The same thread, as Ctrl+Shift+PageDown does: no rc1 later crash.
    TempDirectory temp;
    const auto r = AnalyzeWithSecondFault(temp.path, Case{ .location = "Earlier.dll+321",
      .thread_line = "Exception Thread ID: 35", .address = "0x180000321" }, SecondFault(kDumpTid), std::nullopt, lang);
    Require(!r.crash_logger_later_crash && r.second_fault_lead && r.incident_second_fault->crash_logger_log_is_second &&
            r.quick_answer.headline.find(en ? L"on the same thread" : L"같은 스레드에서") != std::wstring::npos &&
            r.quick_answer.action.find(sameCrash) != std::wstring::npos &&
            crashLoggerItem(r).find(isSecond) != std::wstring::npos,
            "B on the same thread leads, and its Crash Logger log is named as B's");
  }
  // A log that is not B's. One of another crash in this process (the later
  // crash it shows, ADR-0017) is named next to B, and which of the two ended
  // the game is said not to be certain.
  const struct
  {
    std::string_view name;
    Case input;
    bool another_crash;
    std::wstring_view log_tid;
  } others[] = {
    { "a log at another location", { .location = "Earlier.dll+999", .address = "0x180000999" }, true, L"56" },
    // B's location on another thread than B's and A's: still another crash.
    { "a log of another thread", { .location = "Earlier.dll+321", .thread_line = "Exception Thread ID: 57",
                                   .address = "0x180000321" }, true, L"57" },
    { "a log of another process", { .location = "Earlier.dll+321", .process_line = "Process ID: 786",
                                    .address = "0x180000321" }, false, L"" },
  };
  for (const auto& [name, input, anotherCrash, logTid] : others) {
    TempDirectory temp;
    const auto r = AnalyzeWithSecondFault(temp.path, input, SecondFault(kLogTid), std::nullopt, lang);
    Require(r.second_fault_lead && !r.incident_second_fault->crash_logger_log_is_second &&
            r.crash_logger_later_crash == anotherCrash && crashLoggerItem(r).find(isSecond) == std::wstring::npos &&
            CountOf(r.summary_sentence, tag) == 1, name);
    const std::wstring label = Utf8ToWide(input.location) + (en ? L", thread " : L", 스레드 ") + std::wstring(logTid);
    const bool named =
      r.quick_answer.action.find(en ? L"recorded another crash (" + label + L"); check that log too."
                                    : L"다른 크래시(" + label + L")가 기록됐으니 그 로그도 확인하세요.") !=
      std::wstring::npos;
    const bool uncertain =
      r.quick_answer.why_not_certain.find(en ? L"which of the two" : L"둘 중 어느 것이") != std::wstring::npos &&
      r.summary_sentence.find(en ? L"which of the two ended the game is not certain"
                                 : L"둘 중 어느 것이 게임을 끝냈는지 확실하지 않습니다") != std::wstring::npos;
    // B is then only a crash that may have ended the game.
    const bool hedged =
      r.quick_answer.headline.find(en ? L"a crash that may have ended the game" : L"게임을 끝냈을 수 있는 크래시(") !=
        std::wstring::npos &&
      r.summary_sentence.find(en ? L"the crash that may have ended the game happened" : L"게임을 끝냈을 수 있는 크래시는") !=
        std::wstring::npos;
    const bool laterItem = std::any_of(r.evidence.begin(), r.evidence.end(), [en](const EvidenceItem& e) {
      return e.title == (en ? L"Different crash from dump" : L"덤프와 다른 크래시");
    });
    Require(named == anotherCrash && uncertain == anotherCrash && hedged == anotherCrash &&
            laterItem == anotherCrash &&
            (anotherCrash || r.quick_answer.action.find(L"Crash Logger") == std::wstring::npos), name);
  }
  {
    // The other crash in this dump's module: that module is not set aside.
    constexpr DumpModule windows{ L"C:\\Windows\\System32\\d3d11.dll", kModuleBase + 0x4000 };
    TempDirectory temp;
    const auto r = AnalyzeWithSecondFault(temp.path, Case{ .location = "Earlier.dll+999", .address = "0x180000999",
                                                           .extra_modules = { &windows, 1 } },
                                          SecondFault(kLogTid, kModuleBase + 0x4321), std::nullopt, lang);
    Require(r.second_fault_lead && internal::CrashLoggerShowsAnotherCrash(r) &&
            r.quick_answer.action.find(en ? L"Do not treat" : L"원인으로 보지 마세요") == std::wstring::npos,
            "with the other crash in this dump's module, that module is not set aside");
  }

  // How the log's location and IDs are read against B.
  const struct
  {
    std::string_view name;
    Case input;
    nlohmann::json fault;
    bool is_second;
  } readings[] = {
    { "a log without a thread ID", { .location = "Earlier.dll+321", .thread_line = "", .address = "0x180000321" },
      SecondFault(kLogTid), true },
    { "a log without a process ID", { .location = "Earlier.dll+321", .process_line = "", .address = "0x180000321" },
      SecondFault(kLogTid), true },
    { "a path-qualified location", { .location = "C:\\Fixture\\Earlier.dll+321", .address = "0x180000321" },
      SecondFault(kLogTid), true },
    { "another case and a 0x prefix", { .location = "EARLIER.DLL+0x321", .address = "0x180000321" },
      SecondFault(kLogTid), true },
    { "an unreadable offset", { .location = "Earlier.dll+zz", .address = "0x180000321" }, SecondFault(kLogTid), false },
    // A's own location: the log is not set apart from this dump's fault.
    { "B at A's location", { .location = "Earlier.dll+123", .address = "0x180000123" },
      SecondFault(kLogTid, kModuleBase + 0x123), false },
  };
  for (const auto& [name, input, fault, isSecondLog] : readings) {
    TempDirectory temp;
    const auto r = AnalyzeWithSecondFault(temp.path, input, fault, std::nullopt, lang);
    Require(r.second_fault_lead && r.incident_second_fault->crash_logger_log_is_second == isSecondLog &&
            (r.quick_answer.action.find(sameCrash) != std::wstring::npos) == isSecondLog, name);
  }

  {
    // No dump of B, so no lead; the log is still named as B's.
    TempDirectory temp;
    const nlohmann::json fault = {
      { "exception_code", EXCEPTION_ACCESS_VIOLATION }, { "exception_addr", kModuleBase + 0x321 },
      { "faulting_tid", kLogTid }, { "resumed", false }, { "reason", "dump_write_failed" },
    };
    const auto r = AnalyzeWithSecondFault(temp.path, Case{ .location = "Earlier.dll+321", .address = "0x180000321" },
                                          fault, std::nullopt, lang);
    Require(!r.second_fault_lead && r.incident_second_fault->crash_logger_log_is_second &&
            r.crash_logger_pairing_ambiguous && crashLoggerItem(r).find(isSecond) != std::wstring::npos &&
            r.evidence.back().details.find(en ? L"is of that exception" : L"그 예외의 것입니다") != std::wstring::npos,
            "without B's dump the log is named as B's and stays downgraded");
  }
}

// ADR-0036 addendum: A's own unhandled signal, a forced close, a clean exit
// or a manual capture keep A's analysis in the lead.
void TestSecondFaultLeadExclusions()
{
  {
    TempDirectory temp;
    const auto r = AnalyzeWithSecondFault(temp.path, Case{ .log_name = L"" }, SecondFault(kLogTid), std::nullopt,
                                          i18n::Language::kEnglish, /*unhandledSignal=*/true);
    Require(r.incident_unhandled_exception_signal && !r.second_fault_lead &&
            r.quick_answer.case_id != "second_fault_crash" && r.second_fault_note_last,
            "A that reached the top-level filter itself is the crash");
  }
  TempDirectory temp;
  const auto analyzed = AnalyzeWithSecondFault(temp.path, Case{ .log_name = L"" }, SecondFault(kLogTid), std::nullopt,
                                               i18n::Language::kEnglish);
  Require(analyzed.second_fault_lead, "the fixture leads with B");
  const auto check = [](AnalysisResult r, const auto& edit, std::string_view name) {
    edit(r);
    BuildEvidenceAndSummary(r, i18n::Language::kEnglish);
    Require(!r.second_fault_lead && r.quick_answer.case_id != "second_fault_crash" &&
            !r.summary_sentence.starts_with(L"[Different crash from dump]"), name);
  };
  check(analyzed, [](AnalysisResult& r) { r.forced_close.detected = true; }, "forced close");
  check(analyzed, [](AnalysisResult& r) { r.is_filtered_clean_exit = true; }, "clean exit");
  check(analyzed, [](AnalysisResult& r) { r.quit_before_freeze.detected = true; }, "slow shutdown");
  check(analyzed, [](AnalysisResult& r) { r.incident_capture_kind = "manual"; }, "manual capture kind");
  check(analyzed, [](AnalysisResult& r) { r.incident_capture_kind = "hang"; }, "freeze capture kind");
  check(analyzed, [](AnalysisResult& r) { r.dump_path = L"Fixture_hang_20970713_120000.dmp"; }, "freeze by name");
  check(analyzed, [](AnalysisResult& r) {
    r.has_wct = true;
    r.wct_json_utf8 = R"({"capture":{"kind":"hang"}})";
  }, "freeze by its WCT record");
  // Without a thread list the WCT summary is not read for the capture kind,
  // so only the lead's own check sees this one.
  check(analyzed, [](AnalysisResult& r) {
    r.has_wct = true;
    r.wct_json_utf8 = R"({"capture":{"kind":"manual"}})";
  }, "manual capture by its WCT record");
  check(analyzed, [](AnalysisResult& r) { r.exc_code = 0; }, "no exception in this dump");
  check(analyzed, [](AnalysisResult& r) { r.incident_second_fault->resumed = true; }, "resumed");
  check(analyzed, [](AnalysisResult& r) { r.incident_second_fault->dump_missing = true; }, "dump gone");
  check(analyzed, [](AnalysisResult& r) { r.incident_second_fault->dump_present = false; }, "dump not seen");
}

void TestSecondFaultStream(i18n::Language lang)
{
  const bool en = lang == i18n::Language::kEnglish;
  constexpr auto stem = L"SkyrimDiag_SecondFault_20970713_120000_000";
  TempDirectory temp;
  const auto r = Analyze(temp.path, Case{ .log_name = L"", .second_fault_timeline = true,
                                         .second_fault_stream = true }, lang);
  const auto summary = BuildSummaryJson(r, temp.path, stem, true);
  const auto report = BuildReportText(r, summary, true);
  Require(r.is_second_fault_capture && r.is_crash_like && !r.is_hang_like && !r.crash_bucket_key.empty() &&
          summary["analysis"].at("is_second_fault_capture") == true,
          "recognize a second-fault minidump without a sidecar");
  Require(r.exc_code == EXCEPTION_ACCESS_VIOLATION && r.exc_tid == kDumpTid &&
          r.blackbox_exception_code == EXCEPTION_ILLEGAL_INSTRUCTION && r.blackbox_faulting_tid == kLogTid,
          "use B's exception stream and keep the blackbox's A fields");
  Require(r.events.back().t_ms == kFirstFaultMs && r.second_fault_time_ms == kSecondFaultMs,
          "preserve the source events and use B's QPC as the crash anchor");
  Require(!r.forced_close.detected && r.save_load.timed_to_crash && r.save_load.seconds_loading == 13.0 &&
          r.first_chance_summary.recent_count == 1,
          "forced-close, save-load, and recent first-chance windows use B's QPC");
  const auto resources = std::find_if(r.evidence.begin(), r.evidence.end(), [en](const EvidenceItem& item) {
    return item.title.starts_with(en ? L"Resources loaded near the crash" : L"크래시 직전/직후 로드된 리소스");
  });
  Require(resources != r.evidence.end() && resources->details.find(L"-4200ms") != std::wstring::npos &&
          resources->details.find(L"recent.nif") != std::wstring::npos &&
          resources->details.find(L"older.nif") == std::wstring::npos,
          "resource evidence uses B's QPC for both its cutoff and relative time");
  Require(report.find(en ? "earlier exception held it frozen" : "일반 이벤트가 기록되지 않았습니다") !=
            std::string::npos &&
          r.summary_sentence.starts_with(en ? L"This dump is of an exception" : L"이 덤프는 Tullius"),
          "explain what the second dump contains before its conclusion and account for the blackbox gap");
  Require(r.quick_answer.headline.starts_with(
            en ? L"The game crashed to desktop (CTD). This is the separate dump of an exception that reached the "
                 L"top-level exception filter unhandled while Tullius was checking an earlier exception. "
               : L"게임이 튕겼습니다(CTD). 이 덤프는 Tullius가 앞선 예외를 확인하던 동안 처리되지 않은 채 최상위 예외 "
                 L"필터에 닿은 예외의 보조 덤프입니다. "),
          "B's quick answer says what the dump is, after the kind of problem");
  Require(!fs::exists(temp.path / "crash_history.json"), "analyzing B does not write history");

  const auto withManifest = Analyze(temp.path, Case{ .log_name = L"",
    .manifest = R"({"capture_kind":"crash","context":{"unhandled_exception_signal":true}})",
    .second_fault_timeline = true, .second_fault_stream = true }, lang);
  Require(!withManifest.incident_unhandled_exception_signal &&
          !BuildSummaryJson(withManifest, temp.path, stem, true).contains("incident"),
          "a timestamp collision does not attach A's incident to B");

  TempDirectory oldDir;
  const auto old = Analyze(oldDir.path, Case{ .log_name = L"", .second_fault_timeline = true }, lang);
  Require(!old.is_second_fault_capture && !old.second_fault_time_ms && old.forced_close.detected &&
          old.save_load.seconds_loading == 9.0 && old.first_chance_summary.recent_count == 2,
          "a legacy dump still uses A's crash marker for its timing");
  TempDirectory badDir;
  const auto bad = Analyze(badDir.path, Case{ .log_name = L"", .second_fault_timeline = true,
    .second_fault_stream = true, .damaged_second_fault_stream = true }, lang);
  Require(!bad.is_second_fault_capture && !bad.second_fault_time_ms && bad.forced_close.detected,
          "an unsupported stream version leaves the original analysis in place");
}

// Item 18: a second fault dump is a crash capture for Crash Logger pairing,
// whatever its name says: a thread dump's log is never its log.
void TestSecondFaultCrashLoggerPairing(i18n::Language lang)
{
  {
    TempDirectory temp;
    const auto r = Analyze(temp.path, Case{ .log_name = L"threaddump-2097-07-13-12-00-04.log",
      .second_fault_stream = true, .expect_log_pairing = false }, lang);
    Require(r.is_second_fault_capture && r.crash_logger_log_path.empty(),
            "a thread dump log does not pair with a second fault dump");
  }
  TempDirectory temp;
  const auto r = Analyze(temp.path, Case{ .second_fault_stream = true }, lang);
  Require(r.is_second_fault_capture && !r.crash_logger_log_path.empty(),
          "a crash log near a second fault dump pairs as with a crash");
}

// Item 18: the history is read for a second fault dump as for a crash, and
// never written.
void TestSecondFaultReadsCrashHistory(i18n::Language lang)
{
  TempDirectory temp;
  const auto earlier = Analyze(temp.path, Case{ .log_name = L"" }, lang);
  const auto history = temp.path / "crash_history.json";
  Require(fs::is_regular_file(history), "the crash fixture writes the history");
  std::ifstream before(history, std::ios::binary);
  const std::string written((std::istreambuf_iterator<char>(before)), std::istreambuf_iterator<char>());
  before.close();

  const auto r = Analyze(temp.path, Case{ .log_name = L"", .second_fault_stream = true }, lang);
  Require(r.is_second_fault_capture && r.crash_bucket_key == earlier.crash_bucket_key &&
            r.history_correlation.count == 2u,
          "B's report reads the earlier crash of its bucket");
  std::ifstream after(history, std::ios::binary);
  const std::string now((std::istreambuf_iterator<char>(after)), std::istreambuf_iterator<char>());
  Require(now == written, "analyzing B leaves the history as it was");
}

// Item 19: B's time is unknown when its stream has no usable QPC.
void TestSecondFaultTimeUnknown(i18n::Language lang)
{
  const bool en = lang == i18n::Language::kEnglish;
  TempDirectory temp;
  const auto r = Analyze(temp.path, Case{ .log_name = L"", .second_fault_timeline = true, .second_fault_stream = true,
    .second_fault_qpc_unknown = true }, lang);
  Require(r.is_second_fault_capture && !r.second_fault_time_ms, "a zero QPC gives no B time");
  const bool said = std::any_of(r.evidence.begin(), r.evidence.end(), [en](const EvidenceItem& item) {
    return item.details.find(en ? L"When this exception happened is not known" : L"이 예외가 난 시각은 알 수 없습니다") !=
      std::wstring::npos;
  });
  Require(said, "the report says B's time is not known");
}

}  // namespace

int main(int argc, char* argv[])
{
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--second-fault") {
      TestSecondFaultIncident(i18n::Language::kEnglish);
      TestSecondFaultIncident(i18n::Language::kKorean);
      TestSecondFaultStream(i18n::Language::kEnglish);
      TestSecondFaultStream(i18n::Language::kKorean);
      TestSecondFaultCrashLoggerPairing(i18n::Language::kEnglish);
      TestSecondFaultReadsCrashHistory(i18n::Language::kEnglish);
      TestSecondFaultTimeUnknown(i18n::Language::kEnglish);
      TestSecondFaultTimeUnknown(i18n::Language::kKorean);
      TestSecondFaultLead(i18n::Language::kEnglish);
      TestSecondFaultLead(i18n::Language::kKorean);
      TestSecondFaultLeadTexts(i18n::Language::kEnglish);
      TestSecondFaultLeadTexts(i18n::Language::kKorean);
      TestSecondFaultLeadCrashLogger(i18n::Language::kEnglish);
      TestSecondFaultLeadCrashLogger(i18n::Language::kKorean);
      TestSecondFaultLeadExclusions();
      return 0;
    }
    TestPairingTime();
    TestUnhandledSignal();
    TestExceptionModules();
    TestSameDll(i18n::Language::kKorean);
    TestSameDll(i18n::Language::kEnglish);
    TestOutput(i18n::Language::kKorean, "SkyrimSE.exe+4321", L"");
    TestOutput(i18n::Language::kEnglish, "C:\\Fixture\\Later.dll+4321", L"Later.dll");
    TestOutput(i18n::Language::kEnglish, "KERNELBASE.dll+4321", L"");
    // Crash Logger itself, SKSE and an external program are never the mod to blame.
    TestOutput(i18n::Language::kEnglish, "CrashLoggerSSE.dll+4321", L"");
    TestOutput(i18n::Language::kEnglish, "skse64_1_6_1170.dll+4321", L"");
    TestOutput(i18n::Language::kEnglish, "RTSSHooks64.dll+4321", L"");
    TestRejectedInputs();
    TestOutputExclusions();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
