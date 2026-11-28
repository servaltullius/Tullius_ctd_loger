// ADR-0039 in the analysis: a crash that happened while the thread handled an
// earlier exception. In the shape of the 2026-11-28 crash: an access
// violation (read) at Precision.dll+0xe5547 was being dispatched when the
// vectored handler of a frame generation DLL that ran first (nvngx_dlssg.dll)
// hit heap corruption (0xC0000374) in ntdll. Before ADR-0039 the report put
// nvngx_dlssg.dll first and Precision.dll second, and said nothing of the
// first exception.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "Analyzer.h"
#include "AnalyzerInternals.h"
#include "AnalyzerInternalsStackwalkPriv.h"
#include "AnalyzerPipeline.h"
#include "CandidateConsensus.h"
#include "EarlierException.h"
#include "EvidenceBuilder.h"
#include "EvidenceBuilderPrivate.h"
#include "OutputWriterPipeline.h"
#include "Utf.h"

namespace skydiag::dump_tool::internal::stackwalk {
std::vector<SuspectItem> ComputeCallstackSuspectsFromAddrs(
  const std::vector<minidump::ModuleInfo>& modules,
  const std::vector<std::uint64_t>& pcs,
  i18n::Language lang,
  bool walkReachedThreadStart = true);
}

namespace {

using skydiag::dump_tool::AnalysisResult;
using skydiag::dump_tool::BuildEvidenceAndSummary;
using skydiag::dump_tool::EarlierExceptionInfo;
using skydiag::dump_tool::SuspectItem;
using skydiag::dump_tool::WideToUtf8;
using skydiag::dump_tool::minidump::ModuleInfo;
namespace ee = skydiag::dump_tool::earlier_exception;
namespace i18n = skydiag::dump_tool::i18n;

constexpr i18n::Language kLanguages[] = { i18n::Language::kEnglish, i18n::Language::kKorean };

constexpr std::uint64_t kNtdllBase = 0x7FFD04D50000ull;
constexpr std::uint64_t kPrecisionBase = 0x7FFBD2970000ull;
constexpr std::uint64_t kHandlerBase = 0x7FFB9D360000ull;
constexpr std::uint64_t kGameBase = 0x7FF6409F0000ull;

void Require(bool condition, const char* message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

bool Contains(std::wstring_view text, std::wstring_view part)
{
  return text.find(part) != std::wstring_view::npos;
}

ModuleInfo Module(std::wstring name, std::uint64_t base, std::uint64_t size, std::wstring path)
{
  ModuleInfo m{};
  m.filename = std::move(name);
  m.path = std::move(path);
  m.base = base;
  m.end = base + size;
  return m;
}

std::vector<ModuleInfo> CrashModules()
{
  auto ntdll = Module(L"ntdll.dll", kNtdllBase, 0x26C000, L"C:\\Windows\\System32\\ntdll.dll");
  ntdll.is_systemish = true;
  auto game = Module(L"SkyrimSE.exe", kGameBase, 0x3870000, L"G:\\Skyrim\\SkyrimSE.exe");
  game.is_game_exe = true;
  auto precision = Module(L"Precision.dll", kPrecisionBase, 0x1D4000,
    L"G:\\MO2\\mods\\Precision - Accurate Melee Collisions\\SKSE\\Plugins\\Precision.dll");
  precision.inferred_mod_name = L"Precision - Accurate Melee Collisions";
  auto handler = Module(L"nvngx_dlssg.dll", kHandlerBase, 0x744000,
    L"G:\\MO2\\mods\\Upscaler\\SKSE\\Plugins\\nvngx_dlssg.dll");
  handler.inferred_mod_name = L"Upscaler";
  // Sorted by base, as LoadAllModules returns them.
  return { game, handler, precision, ntdll };
}

// The walk of 2026-11-28 (the report's 15 frames, then the game below).
constexpr std::size_t kDispatcherIndex = 10;
std::vector<std::uint64_t> CrashPcs()
{
  return {
    kNtdllBase + 0x112FC5ull, kNtdllBase + 0xF1A35ull, kNtdllBase + 0x11E602ull, kNtdllBase + 0x1094DAull,
    kNtdllBase + 0x2B527ull, kHandlerBase + 0x2C01FCull, kHandlerBase + 0x1626Full, kHandlerBase + 0x1565Aull,
    kNtdllBase + 0xA3416ull, kNtdllBase + 0xA2BB5ull, kNtdllBase + 0x9F2DEull,
    kPrecisionBase + 0xE5547ull, kPrecisionBase + 0xF17E3ull, kPrecisionBase + 0xF4E5Eull, kPrecisionBase + 0x11F5D2ull,
    kGameBase + 0x54C248ull, kGameBase + 0x54B000ull,
  };
}

void TestSuspectsComeFromTheEarlierException()
{
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    const auto modules = CrashModules();
    const auto pcs = CrashPcs();
    const auto before = skydiag::dump_tool::internal::stackwalk::ComputeCallstackSuspectsFromAddrs(modules, pcs, lang);
    Require(before.size() == 2u && before[0].module_filename == L"nvngx_dlssg.dll", "the walk alone put the handler first");

    const auto suspects = skydiag::dump_tool::internal::stackwalk::ComputeEarlierExceptionSuspects(
      modules, pcs, kDispatcherIndex, kPrecisionBase + 0xE5547ull, lang, /*walkReachedThreadStart=*/true);
    Require(suspects.size() == 2u, "both modules stay");
    Require(suspects[0].module_filename == L"Precision.dll", "the first exception's module leads");
    Require(suspects[0].confidence_level == i18n::ConfidenceLevel::kMedium,
      "Medium at most: whether the first exception would have been handled is not known");
    Require(Contains(suspects[0].reason, en ? L"exception being handled" : L"처리 중이던 예외"), "its reason says why");
    Require(suspects[1].module_filename == L"nvngx_dlssg.dll", "the handler's module after it");
    Require(suspects[1].confidence_level == i18n::ConfidenceLevel::kLow, "Low");
    Require(suspects[1].score < suspects[0].score, "scored below, so a later sort keeps the order");
    Require(Contains(suspects[1].reason, en ? L"exception handler" : L"예외 처리기"), "its reason says why");

    // An earlier exception in the game: the plugin below it, and the
    // handler's module after it, scored below it although its own frames
    // weigh more (8 against 4).
    auto inGame = pcs;
    inGame[kDispatcherIndex + 1u] = kGameBase + 0x1000u;
    inGame[kDispatcherIndex + 2u] = kGameBase + 0x2000u;
    inGame[kDispatcherIndex + 3u] = kGameBase + 0x3000u;
    const auto game = skydiag::dump_tool::internal::stackwalk::ComputeEarlierExceptionSuspects(
      modules, inGame, kDispatcherIndex, kGameBase + 0x1000u, lang, true);
    Require(game.size() == 2u && game[0].module_filename == L"Precision.dll" &&
              game[1].module_filename == L"nvngx_dlssg.dll",
      "the plugin below the game's fault, then the handler");
    Require(game[0].score == 4u && game[1].score < game[0].score, "the handler's module scored below it");

    // Only the game and Windows below the dispatcher: the handler's module is
    // the only suspect, and only Low.
    auto onlyGame = std::vector<std::uint64_t>(pcs.begin(), pcs.begin() + kDispatcherIndex + 1);
    onlyGame.push_back(kGameBase + 0x1000u);
    onlyGame.push_back(kGameBase + 0x2000u);
    const auto handlerOnly = skydiag::dump_tool::internal::stackwalk::ComputeEarlierExceptionSuspects(
      modules, onlyGame, kDispatcherIndex, kGameBase + 0x1000u, lang, true);
    Require(handlerOnly.size() == 1u && handlerOnly[0].module_filename == L"nvngx_dlssg.dll" &&
              handlerOnly[0].confidence_level == i18n::ConfidenceLevel::kLow,
      "the handler alone, Low");
  }
}

SuspectItem Suspect(std::wstring module, std::wstring modName, i18n::ConfidenceLevel level, std::uint32_t score)
{
  SuspectItem s{};
  s.module_filename = std::move(module);
  s.module_path = L"G:\\MO2\\mods\\" + modName + L"\\SKSE\\Plugins\\" + s.module_filename;
  s.inferred_mod_name = std::move(modName);
  s.confidence_level = level;
  s.confidence = i18n::ConfidenceText(i18n::Language::kEnglish, level);
  s.score = score;
  return s;
}

EarlierExceptionInfo CrashEarlier()
{
  EarlierExceptionInfo e{};
  e.detected = true;
  e.code = 0xC0000005u;
  e.address = kPrecisionBase + 0xE5547ull;
  e.info = { 0u, 0x1DFD5DF3000ull };
  e.module_filename = L"Precision.dll";
  e.module_path = L"G:\\MO2\\mods\\Precision - Accurate Melee Collisions\\SKSE\\Plugins\\Precision.dll";
  e.module_offset = 0xE5547u;
  e.module_plus_offset = L"Precision.dll+0xe5547";
  e.inferred_mod_name = L"Precision - Accurate Melee Collisions";
  e.module_is_mod = true;
  e.handler_module_filename = L"nvngx_dlssg.dll";
  e.handler_inferred_mod_name = L"Upscaler";
  e.dispatcher_depth = 10;
  e.context_address = 0x38559FE400ull;
  e.record_address = 0x38559FE8F0ull;
  e.handler_only_modules = { L"nvngx_dlssg.dll" };
  e.frames = { L"Precision.dll+0xe5547", L"Precision.dll+0xf17e3" };
  return e;
}

// What the analyzer hands the evidence builder for that dump.
AnalysisResult NestedCrash(bool withEarlier)
{
  AnalysisResult r{};
  r.dump_path = L"C:\\Logs\\SkyrimDiag_Crash_20261128_100235_374.dmp";
  r.exc_code = 0xC0000374u;
  r.exc_addr = kNtdllBase + 0x112FC5ull;
  r.exc_tid = 39372;
  r.fault_module_filename = L"ntdll.dll";
  r.fault_module_path = L"C:\\Windows\\System32\\ntdll.dll";
  r.fault_module_plus_offset = L"ntdll.dll+0x112fc5";
  r.fault_module_offset = 0x112FC5u;
  r.suspects_from_stackwalk = true;
  if (withEarlier) {
    r.suspects = {
      Suspect(L"Precision.dll", L"Precision - Accurate Melee Collisions", i18n::ConfidenceLevel::kMedium, 40),
      Suspect(L"nvngx_dlssg.dll", L"Upscaler", i18n::ConfidenceLevel::kLow, 8),
    };
    r.earlier_exception = CrashEarlier();
  } else {
    r.suspects = {
      Suspect(L"nvngx_dlssg.dll", L"Upscaler", i18n::ConfidenceLevel::kLow, 8),
      Suspect(L"Precision.dll", L"Precision - Accurate Melee Collisions", i18n::ConfidenceLevel::kLow, 5),
    };
  }
  return r;
}

void TestQuickAnswerAndConclusion()
{
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = NestedCrash(true);
    BuildEvidenceAndSummary(r, lang);
    const auto& qa = r.quick_answer;
    Require(qa.kind == "crash", "a crash");
    Require(qa.case_id == "earlier_exception", "the earlier exception's case");
    Require(qa.suspect_file == L"Precision.dll", "Precision.dll is the suspect");
    Require(qa.likelihood == "medium", "Medium");
    Require(qa.how_it_crashed == ee::DescribePlain(r.earlier_exception, r.exc_code, en), "how it crashed says both");
    Require(Contains(qa.why_not_certain, en ? L"while that error was being handled" : L"그 오류를 처리하는 중에"),
      "the doubt line says it crashed while that error was handled");
    for (const std::wstring_view field : { std::wstring_view(qa.headline), std::wstring_view(qa.action) }) {
      Require(!Contains(field, L"DLL") && !Contains(field, L"stack") && !Contains(field, L"스택"), "plain words");
    }

    const std::wstring tag = en ? L"[Crash while handling an exception] " : L"[예외 처리 중 크래시] ";
    const std::wstring sentence = ee::DescribeTechnical(r.earlier_exception, r.exc_code, en);
    Require(r.summary_sentence.rfind(tag + sentence + L" ", 0) == 0, "the conclusion starts with it");
    const std::wstring rest = r.summary_sentence.substr(tag.size() + sentence.size());
    Require(Contains(rest, L"Precision.dll") && !Contains(rest, L"nvngx_dlssg.dll"),
      "then the usual reading of a crash reported in Windows, now naming Precision.dll");

    const auto item = std::find_if(r.evidence.begin(), r.evidence.end(), [&](const auto& e) {
      return e.title == (en ? L"Crash while handling another exception" : L"다른 예외를 처리하던 중 난 크래시");
    });
    Require(item != r.evidence.end(), "an evidence item");
    Require(Contains(item->details, sentence), "with the sentence");
    Require(item->confidence_level == i18n::ConfidenceLevel::kMedium, "Medium");

    // Without the earlier exception (as before ADR-0039): none of it.
    auto plain = NestedCrash(false);
    BuildEvidenceAndSummary(plain, lang);
    Require(plain.quick_answer.case_id != "earlier_exception" && plain.quick_answer.suspect_file == L"nvngx_dlssg.dll",
      "the old answer");
    Require(!Contains(plain.summary_sentence, tag), "no tag");
    Require(std::none_of(plain.evidence.begin(), plain.evidence.end(), [&](const auto& e) {
      return e.title == (en ? L"Crash while handling another exception" : L"다른 예외를 처리하던 중 난 크래시");
    }), "no item");
  }
}

void TestTheHandlersModuleAsLead()
{
  // The earlier exception was in the game, and only the handler's module is
  // a suspect: it is named, with the doubt that fits.
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = NestedCrash(true);
    r.suspects = { Suspect(L"nvngx_dlssg.dll", L"Upscaler", i18n::ConfidenceLevel::kLow, 8) };
    r.earlier_exception.module_filename = L"SkyrimSE.exe";
    r.earlier_exception.module_plus_offset = L"SkyrimSE.exe+0x1000";
    r.earlier_exception.inferred_mod_name.clear();
    r.earlier_exception.module_is_mod = false;
    BuildEvidenceAndSummary(r, lang);
    Require(r.quick_answer.suspect_file == L"nvngx_dlssg.dll", "the handler's module");
    Require(r.quick_answer.case_id != "earlier_exception", "not the earlier exception's case");
    Require(Contains(r.quick_answer.why_not_certain, en ? L"happened first in another file" : L"다른 파일에서 먼저 난 오류"),
      "it was handling an error from elsewhere");
    Require(r.quick_answer.how_it_crashed == ee::DescribePlain(r.earlier_exception, r.exc_code, en), "how it crashed");
  }
}

void TestSummaryJsonAndReport()
{
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = NestedCrash(true);
    BuildEvidenceAndSummary(r, lang);
    const auto summary = skydiag::dump_tool::BuildSummaryJson(r, std::filesystem::temp_directory_path(), L"stem", true);
    Require(summary["exception"]["code"].get<std::uint32_t>() == 0xC0000374u, "the reported exception stays");
    Require(summary["exception"].contains("earlier_exception"), "the earlier exception");
    const auto& e = summary["exception"]["earlier_exception"];
    Require(e["code"].get<std::uint32_t>() == 0xC0000005u, "code");
    Require(e["address"].get<std::uint64_t>() == kPrecisionBase + 0xE5547ull, "address");
    Require(e["info"].is_array() && e["info"].size() == 2u && e["info"][1].get<std::uint64_t>() == 0x1DFD5DF3000ull, "info");
    Require(e["module_plus_offset"].get<std::string>() == "Precision.dll+0xe5547", "module+offset");
    Require(e["inferred_mod_name"].get<std::string>() == "Precision - Accurate Melee Collisions", "mod name");
    Require(e["module_is_mod"].get<bool>(), "a mod");
    Require(e["module_path"].get<std::string>().find("MO2") == std::string::npos, "the path is redacted");
    Require(e["handler_module"].get<std::string>() == "nvngx_dlssg.dll", "handler");
    Require(e["handler_inferred_mod_name"].get<std::string>() == "Upscaler", "handler's mod");
    Require(e["dispatcher_depth"].get<std::uint32_t>() == 10u, "dispatcher depth");
    Require(e["context_address"].get<std::uint64_t>() == 0x38559FE400ull &&
              e["record_address"].get<std::uint64_t>() == 0x38559FE8F0ull,
      "where on the stack");
    Require(summary["quick_answer"]["case"].get<std::string>() == "earlier_exception", "the quick answer's case");

    const auto report = skydiag::dump_tool::BuildReportText(r, summary, true);
    const std::string line = en
      ? "ExceptionBeingHandled: 0xc0000005 at Precision.dll+0xe5547 (read 0x1dfd5df3000), handler: nvngx_dlssg.dll\n"
      : "처리 중이던 예외: 0xc0000005, Precision.dll+0xe5547 (읽기 0x1dfd5df3000), 처리기: nvngx_dlssg.dll\n";
    Require(report.find("Module+Offset: ntdll.dll+0x112fc5\n" + line) != std::string::npos,
      "the report line after the reported exception");
    Require(e["handler_only_modules"].is_array() && e["handler_only_modules"].size() == 1u &&
              e["handler_only_modules"][0].get<std::string>() == "nvngx_dlssg.dll",
      "handler-only modules");
    Require(e["frames"].is_array() && e["frames"].size() == 2u &&
              e["frames"][0].get<std::string>() == "Precision.dll+0xe5547",
      "the earlier exception's frames");
    const std::string frames = std::string(en ? "\nCallstack of the exception being handled (from where it happened):\n"
                                              : "\n처리 중이던 예외의 콜스택(그 예외가 난 곳부터):\n") +
      "  Precision.dll+0xe5547\n  Precision.dll+0xf17e3\n";
    Require(report.find(frames) != std::string::npos, "the report shows its frames");

    // A validated clean exit writes none of it (Analyzer.cpp clears it too).
    auto clean = NestedCrash(true);
    clean.is_filtered_clean_exit = true;
    clean.suspects.clear();
    BuildEvidenceAndSummary(clean, lang);
    const auto cleanSummary =
      skydiag::dump_tool::BuildSummaryJson(clean, std::filesystem::temp_directory_path(), L"stem", true);
    Require(!cleanSummary["exception"].contains("earlier_exception"), "no key for a clean exit");
    const auto cleanReport = skydiag::dump_tool::BuildReportText(clean, cleanSummary, true);
    Require(cleanReport.find(en ? "ExceptionBeingHandled:" : "처리 중이던 예외:") == std::string::npos &&
              cleanReport.find(en ? "Callstack of the exception being handled" : "처리 중이던 예외의 콜스택") ==
                std::string::npos,
      "no line or frames for a clean exit");

    auto plain = NestedCrash(false);
    BuildEvidenceAndSummary(plain, lang);
    const auto plainSummary =
      skydiag::dump_tool::BuildSummaryJson(plain, std::filesystem::temp_directory_path(), L"stem", true);
    Require(!plainSummary["exception"].contains("earlier_exception"), "no key without one");
    const auto plainReport = skydiag::dump_tool::BuildReportText(plain, plainSummary, true);
    Require(plainReport.find(en ? "ExceptionBeingHandled:" : "처리 중이던 예외:") == std::string::npos, "no line");
  }
}

void TestCleanExitDropsIt()
{
  // A validated clean exit names nothing (Analyzer.cpp clears the rest).
  auto r = NestedCrash(true);
  r.is_filtered_clean_exit = true;
  r.suspects.clear();
  BuildEvidenceAndSummary(r, i18n::Language::kEnglish);
  Require(r.quick_answer.kind == "clean_exit", "a clean exit");
  Require(!Contains(r.summary_sentence, L"[Crash while handling an exception]"), "no tag");
}

void TestWalkEndingAtTheDispatcher()
{
  // The walk stopped at the dispatcher (no unwind data below it, or the
  // frame limit): the earlier exception's own place still leads.
  for (const auto lang : kLanguages) {
    auto pcs = CrashPcs();
    pcs.resize(kDispatcherIndex + 1u);
    const auto suspects = skydiag::dump_tool::internal::stackwalk::ComputeEarlierExceptionSuspects(
      CrashModules(), pcs, kDispatcherIndex, kPrecisionBase + 0xE5547ull, lang, /*walkReachedThreadStart=*/false);
    Require(suspects.size() == 2u && suspects[0].module_filename == L"Precision.dll" &&
              suspects[1].module_filename == L"nvngx_dlssg.dll" && suspects[1].score < suspects[0].score,
      "the earlier exception's module from its saved place, then the handler");
  }
}

void TestHookFrameworkPromotionKeepsTheHandlerBelow()
{
  // The earlier exception's stack puts a mod above the hook framework that
  // scored higher (16 against 12): the handler's module stays below both.
  auto modules = CrashModules();
  auto hook = Module(L"EngineFixes.dll", 0x7FFBA0000000ull, 0x100000, L"G:\\MO2\\mods\\EF\\SKSE\\Plugins\\EngineFixes.dll");
  hook.is_known_hook_framework = true;
  auto mod = Module(L"RealMod.dll", 0x7FFBB0000000ull, 0x100000, L"G:\\MO2\\mods\\Real\\SKSE\\Plugins\\RealMod.dll");
  modules.insert(modules.begin() + 2, { hook, mod });
  const std::vector<std::uint64_t> pcs = {
    kHandlerBase + 0x10u, kHandlerBase + 0x20u, kNtdllBase + 0xA3416ull, kNtdllBase + 0x9F2DEull,
    0x7FFBA0000100ull, 0x7FFBB0000100ull, kGameBase + 0x1000u, kGameBase + 0x2000u,
  };
  const auto suspects = skydiag::dump_tool::internal::stackwalk::ComputeEarlierExceptionSuspects(
    modules, pcs, /*dispatcherIndex=*/3u, 0x7FFBA0000100ull, i18n::Language::kEnglish, true);
  Require(suspects.size() == 3u && suspects[0].module_filename == L"RealMod.dll" &&
            suspects[1].module_filename == L"EngineFixes.dll" && suspects[2].module_filename == L"nvngx_dlssg.dll",
    "the mod, the hook framework, then the handler");
  Require(suspects[2].score < suspects[0].score && suspects[2].score < suspects[1].score,
    "the handler scored below both, whatever their order");
}

void TestCrashLoggerLogOfTheHandlerIsSetAside()
{
  // Crash Logger records the reported exception, so its frames name the
  // handler first (direct fault, first probable frame, top module).
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    // The earlier exception's module with one frame (16): the log's top
    // module rank alone (+18 to the handler's 8) would put the handler first.
    auto r = NestedCrash(true);
    r.language = lang;
    r.suspects[0].score = 16;
    r.crash_logger_log_path = L"C:\\Logs\\crash-2026-11-28-10-02-35.log";
    r.crash_logger_direct_fault_module = L"nvngx_dlssg.dll";
    r.crash_logger_direct_fault_eligible = true;
    r.crash_logger_first_actionable_probable_module = L"nvngx_dlssg.dll";
    r.crash_logger_first_actionable_probable_eligible = true;
    r.crash_logger_top_modules = { L"nvngx_dlssg.dll" };

    auto before = r;
    before.earlier_exception = {};
    skydiag::dump_tool::ApplyCrashLoggerCorroborationToSuspects(&before, CrashModules());
    Require(before.suspects[0].module_filename == L"nvngx_dlssg.dll", "the log alone puts the handler first");

    skydiag::dump_tool::SetAsideCrashLoggerHandlerFrames(&r);
    Require(!r.crash_logger_direct_fault_eligible && !r.crash_logger_first_actionable_probable_eligible,
      "its frame signals in the handler are set aside");
    Require(std::any_of(r.diagnostics.begin(), r.diagnostics.end(),
              [](const std::wstring& d) { return d.rfind(L"[CrashLogger]", 0) == 0; }),
      "and the diagnostics say so");
    skydiag::dump_tool::ApplyCrashLoggerCorroborationToSuspects(&r, CrashModules());
    Require(r.suspects[0].module_filename == L"Precision.dll" &&
              r.suspects[0].confidence_level == i18n::ConfidenceLevel::kMedium,
      "the earlier exception's module stays first, at Medium");
    Require(r.suspects[1].module_filename == L"nvngx_dlssg.dll" &&
              r.suspects[1].confidence_level == i18n::ConfidenceLevel::kLow,
      "the handler stays second, Low");

    BuildEvidenceAndSummary(r, lang);
    Require(r.quick_answer.case_id == "earlier_exception" && r.quick_answer.suspect_file == L"Precision.dll",
      "the quick answer still names the earlier exception's mod");
    Require(!r.actionable_candidates.empty() && r.actionable_candidates[0].module_filename == L"Precision.dll",
      "and so does the first candidate");
    Require(Contains(r.summary_sentence, en ? L"[Crash while handling an exception]" : L"[예외 처리 중 크래시]"),
      "with the conclusion's lead");
  }
}

void TestHookFrameworkLeadKeepsItsDoubt()
{
  // The earlier exception was in a hook framework, the handler in the game:
  // the lead keeps the hook framework's own doubt.
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = NestedCrash(true);
    r.suspects = { Suspect(L"EngineFixes.dll", L"SSE Engine Fixes", i18n::ConfidenceLevel::kMedium, 40) };
    auto& earlier = r.earlier_exception;
    earlier.module_filename = L"EngineFixes.dll";
    earlier.module_plus_offset = L"EngineFixes.dll+0x1234";
    earlier.inferred_mod_name = L"SSE Engine Fixes";
    earlier.handler_module_filename = L"SkyrimSE.exe";
    earlier.handler_inferred_mod_name.clear();
    earlier.handler_only_modules.clear();
    BuildEvidenceAndSummary(r, lang);
    Require(r.quick_answer.case_id == "hook_framework" && r.quick_answer.suspect_file == L"EngineFixes.dll",
      "a hook framework lead");
    Require(Contains(r.quick_answer.why_not_certain, en ? L"Many mods run through this one" : L"여러 모드가 이 모드를 거쳐"),
      "with the shared-mod doubt");
    Require(r.quick_answer.how_it_crashed == ee::DescribePlain(r.earlier_exception, r.exc_code, en),
      "and how it crashed");
  }
}

AnalysisResult::CrashLoggerModReference ObjectRef(std::wstring esp)
{
  AnalysisResult::CrashLoggerModReference ref{};
  ref.esp_name = std::move(esp);
  ref.best_object_type = L"TESObjectWEAP";
  ref.ref_count = 3;
  ref.relevance_score = 20;
  return ref;
}

void TestCandidatesFollowTheSuspects()
{
  // A Crash Logger object reference of the same mod plus its stack is what
  // makes a candidate High (cross-validated). The module of the exception
  // being handled stays Medium; a module only on the handler's side stays
  // Low and after it.
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = NestedCrash(true);
    r.suspects[0].inferred_mod_name = L"Precision";
    r.earlier_exception.inferred_mod_name = L"Precision";
    r.crash_logger_log_path = L"C:\\Logs\\crash-2026-11-28-10-02-35.log";
    r.crash_logger_object_refs = { ObjectRef(L"Precision.esp"), ObjectRef(L"Upscaler.esp") };

    auto control = r;
    control.earlier_exception = {};
    BuildEvidenceAndSummary(control, lang);
    const auto controlTop = std::find_if(control.actionable_candidates.begin(), control.actionable_candidates.end(),
      [](const auto& c) { return c.module_filename == L"Precision.dll"; });
    Require(controlTop != control.actionable_candidates.end() &&
              controlTop->confidence_level == i18n::ConfidenceLevel::kHigh,
      "without ADR-0039 the cross-validation makes it High");

    BuildEvidenceAndSummary(r, lang);
    const auto earlier = std::find_if(r.actionable_candidates.begin(), r.actionable_candidates.end(),
      [](const auto& c) { return c.module_filename == L"Precision.dll"; });
    const auto handler = std::find_if(r.actionable_candidates.begin(), r.actionable_candidates.end(),
      [](const auto& c) { return c.module_filename == L"nvngx_dlssg.dll"; });
    Require(earlier != r.actionable_candidates.end() && earlier->confidence_level == i18n::ConfidenceLevel::kMedium,
      "the earlier exception's module at Medium");
    Require(handler != r.actionable_candidates.end() && handler->confidence_level == i18n::ConfidenceLevel::kLow &&
              handler > earlier,
      "the handler's module Low and after it");
    Require(r.quick_answer.suspect_file == L"Precision.dll" && r.quick_answer.likelihood == "medium" &&
              !r.quick_answer.why_not_certain.empty(),
      "the quick answer at Medium, with its doubt line");
    Require(Contains(r.summary_sentence, en ? L"Confidence: Medium" : L"신뢰도: 중간") &&
              !Contains(r.summary_sentence, en ? L"Confidence: High" : L"신뢰도: 높음"),
      "and the conclusion says the same");

    // Only the handler's module has other clues (an object reference of its
    // mod and repeats in this crash bucket): they would raise it from Low.
    auto handlerOnly = NestedCrash(true);
    handlerOnly.crash_logger_log_path = L"C:\\Logs\\crash-2026-11-28-10-02-35.log";
    handlerOnly.crash_logger_object_refs = { ObjectRef(L"Upscaler.esp") };
    skydiag::dump_tool::BucketCandidateRepeat repeat{};
    repeat.candidate_key = WideToUtf8(skydiag::dump_tool::CanonicalCandidateKey(L"Upscaler"));
    repeat.prior_count = 3;
    handlerOnly.bucket_candidate_repeats = { repeat };
    const auto handlerLevel = [](const AnalysisResult& result) {
      const auto it = std::find_if(result.actionable_candidates.begin(), result.actionable_candidates.end(),
        [](const auto& c) { return c.module_filename == L"nvngx_dlssg.dll"; });
      return it == result.actionable_candidates.end() ? i18n::ConfidenceLevel::kUnknown : it->confidence_level;
    };
    auto handlerControl = handlerOnly;
    handlerControl.earlier_exception.handler_only_modules.clear();
    BuildEvidenceAndSummary(handlerControl, lang);
    Require(handlerLevel(handlerControl) == i18n::ConfidenceLevel::kMedium ||
              handlerLevel(handlerControl) == i18n::ConfidenceLevel::kHigh,
      "those clues alone raise the handler's module");
    BuildEvidenceAndSummary(handlerOnly, lang);
    Require(handlerLevel(handlerOnly) == i18n::ConfidenceLevel::kLow, "a module only on the handler's side stays Low");
    Require(!handlerOnly.actionable_candidates.empty() &&
              handlerOnly.actionable_candidates[0].module_filename == L"Precision.dll" &&
              handlerOnly.quick_answer.suspect_file == L"Precision.dll",
      "and does not lead");
  }
}

void TestReportedInTheHandlersOwnCode()
{
  // The handler faulted in its own code (not in Windows): the quick answer
  // still leads with the module of the exception being handled.
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = NestedCrash(true);
    r.exc_code = 0xC0000005u;
    r.exc_addr = kHandlerBase + 0x1626Full;
    r.exc_info = { 0u, 0x18u };
    r.fault_module_filename = L"nvngx_dlssg.dll";
    r.fault_module_path = L"G:\\MO2\\mods\\Upscaler\\SKSE\\Plugins\\nvngx_dlssg.dll";
    r.fault_module_plus_offset = L"nvngx_dlssg.dll+0x1626f";
    r.fault_module_offset = 0x1626Fu;
    r.inferred_mod_name = L"Upscaler";
    BuildEvidenceAndSummary(r, lang);
    Require(r.quick_answer.suspect_file == L"Precision.dll" && r.quick_answer.case_id == "earlier_exception",
      "the earlier exception's mod leads");
    Require(r.quick_answer.how_it_crashed == ee::DescribePlain(r.earlier_exception, r.exc_code, en),
      "how it crashed");
    Require(!r.actionable_candidates.empty() && r.actionable_candidates[0].module_filename == L"Precision.dll",
      "and the first candidate");
  }
}

void TestForcedCloseLeadComesFirst()
{
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = NestedCrash(true);
    r.forced_close.detected = true;
    r.forced_close.kind = "alt_f4";
    r.forced_close.seconds_before_crash = 1.5;
    BuildEvidenceAndSummary(r, lang);
    const std::wstring lead = skydiag::dump_tool::internal::ForcedCloseLead(r.forced_close, en);
    const std::wstring tag = en ? L"[Crash while handling an exception] " : L"[예외 처리 중 크래시] ";
    Require(r.summary_sentence.rfind(lead + L" " + tag, 0) == 0, "the forced close lead, then this one");
  }
}

}  // namespace

int main()
{
  struct Case
  {
    const char* name;
    void (*run)();
  };
  const Case cases[] = {
    { "SuspectsComeFromTheEarlierException", TestSuspectsComeFromTheEarlierException },
    { "QuickAnswerAndConclusion", TestQuickAnswerAndConclusion },
    { "TheHandlersModuleAsLead", TestTheHandlersModuleAsLead },
    { "SummaryJsonAndReport", TestSummaryJsonAndReport },
    { "CleanExitDropsIt", TestCleanExitDropsIt },
    { "WalkEndingAtTheDispatcher", TestWalkEndingAtTheDispatcher },
    { "HookFrameworkPromotionKeepsTheHandlerBelow", TestHookFrameworkPromotionKeepsTheHandlerBelow },
    { "CrashLoggerLogOfTheHandlerIsSetAside", TestCrashLoggerLogOfTheHandlerIsSetAside },
    { "HookFrameworkLeadKeepsItsDoubt", TestHookFrameworkLeadKeepsItsDoubt },
    { "ForcedCloseLeadComesFirst", TestForcedCloseLeadComesFirst },
    { "CandidatesFollowTheSuspects", TestCandidatesFollowTheSuspects },
    { "ReportedInTheHandlersOwnCode", TestReportedInTheHandlersOwnCode },
  };
  int failed = 0;
  for (const auto& c : cases) {
    try {
      c.run();
    } catch (const std::exception& e) {
      std::fprintf(stderr, "earlier_exception_analysis_tests: %s FAIL: %s\n", c.name, e.what());
      ++failed;
    }
  }
  if (failed != 0) {
    return 1;
  }
  std::printf("earlier_exception_analysis_tests: OK\n");
  return 0;
}
