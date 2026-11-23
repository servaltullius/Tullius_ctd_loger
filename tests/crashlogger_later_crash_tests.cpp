#include <Windows.h>
#include <DbgHelp.h>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>

#include <nlohmann/json.hpp>

#include "Analyzer.h"
#include "EvidenceBuilder.h"
#include "OutputWriterPipeline.h"
#include "SkyrimDiagProtocol.h"
#include "SkyrimDiagShared.h"

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
  MINIDUMP_DIRECTORY streams[4]{};
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
};

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
  const auto dump = dir / (input.dump_time_from_last_write ? L"Fixture_crash.dmp" : L"Fixture_crash_20970713_120000.dmp");
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
  Require(result.crash_logger_log_path.empty() == input.log_name.empty(), "pair the expected fixture log");
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

}  // namespace

int main()
{
  try {
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
