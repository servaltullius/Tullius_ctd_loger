// What the 2026-10-12 crash test (tools/crash_tester) showed the reports got
// wrong, pinned with the shapes of those real captures:
// - a freeze whose main thread waited in the engine for a game job thread
//   that was stuck inside a plugin named only the main thread's call path;
// - an unhandled C++ exception that Crash Logger also logged was described as
//   a possibly caught throw, with advice to set the CrashHookMode already set;
// - the SKSE runtime, the path SKSE tasks take, came up as a candidate and as
//   a "nearby DLL" to compare.

#include <cstdio>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "Analyzer.h"
#include "AnalyzerInternals.h"
#include "AnalyzerPipeline.h"
#include "EvidenceBuilder.h"
#include "EvidenceBuilderPrivate.h"
#include "NextAction.h"
#include "OutputWriterPipeline.h"

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
using skydiag::dump_tool::SelectNextActionIndex;
using skydiag::dump_tool::SuspectItem;
namespace i18n = skydiag::dump_tool::i18n;

void Require(bool condition, const char* message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

bool Contains(const std::wstring& text, std::wstring_view part)
{
  return text.find(part) != std::wstring::npos;
}

bool AnyStartsWith(const std::vector<std::wstring>& lines, std::wstring_view prefix, std::wstring_view part = L"")
{
  for (const auto& line : lines) {
    if (line.rfind(prefix, 0) == 0 && Contains(line, part)) {
      return true;
    }
  }
  return false;
}

SuspectItem Suspect(std::wstring module, std::wstring modName, i18n::ConfidenceLevel level)
{
  SuspectItem s{};
  s.module_filename = std::move(module);
  s.inferred_mod_name = std::move(modName);
  s.confidence_level = level;
  s.score = 1;
  return s;
}

// SkyrimDiag_Hang_20261012_191024_970 after the fix: the lead the analyzer
// puts first, then the main thread's call path (demoted).
AnalysisResult MakeBlockedFreeze(std::wstring module, std::string state)
{
  AnalysisResult r{};
  r.dump_path = L"C:\\Logs\\SkyrimDiag_Hang_20261012_191024_970.dmp";
  r.freeze_analysis.has_analysis = true;
  r.freeze_analysis.state_id = "freeze_ambiguous";
  auto& wait = r.main_thread_wait;
  wait.kind = "engine_wait";
  wait.wait_class = "sync";
  wait.wait_api = L"KERNELBASE.dll!WaitForSingleObjectEx";
  wait.waiting_module = L"SkyrimSE.exe";
  wait.path_modules = { L"hdtsmp64.dll" };
  wait.blocked_thread_tid = 40400;
  wait.blocked_module = module;
  wait.blocked_mod_name = L"CrashTester (dev test)";
  wait.blocked_state = std::move(state);
  wait.blocked_top = L"KERNELBASE.dll!SleepEx";
  wait.blocked_thread_count = 1;
  r.suspects = {
    Suspect(module, L"CrashTester (dev test)", i18n::ConfidenceLevel::kMedium),
    Suspect(L"hdtsmp64.dll", L"FSMP", i18n::ConfidenceLevel::kLow),
  };
  r.suspects_from_stackwalk = true;
  r.suspects_from_main_thread = true;
  return r;
}

void TestBlockedGameThreadLeadsTheFreezeReport()
{
  for (const auto lang : { i18n::Language::kEnglish, i18n::Language::kKorean }) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = MakeBlockedFreeze(L"CrashTester.dll", "waiting_in_plugin");
    BuildEvidenceAndSummary(r, lang);
    Require(Contains(r.summary_sentence, en ? L"that thread (tid 40400) was waiting inside CrashTester (dev test) (CrashTester.dll)"
                                            : L"그 스레드(tid 40400)는 CrashTester (dev test) (CrashTester.dll) 코드 안에서 대기 중이었습니다"),
      "the summary names the plugin holding the thread the main thread waited for");
    Require(!Contains(r.summary_sentence, en ? L"cannot tell what the engine was waiting for" : L"무엇을 기다렸는지는"),
      "no longer says the wait cannot be explained");
    const auto next = SelectNextActionIndex(r.recommendations);
    Require(next != std::wstring::npos &&
              r.recommendations[next].rfind(en ? L"[Main thread]" : L"[메인 스레드]", 0) == 0 &&
              Contains(r.recommendations[next], L"CrashTester.dll"),
      "the next action is the blocked plugin");
    Require(!AnyStartsWith(r.recommendations, en ? L"[Main thread]" : L"[메인 스레드]", L"hdtsmp64.dll"),
      "the main thread's call path is not the advice");
    bool hasEvidence = false;
    for (const auto& e : r.evidence) {
      hasEvidence = hasEvidence || (e.confidence_level == i18n::ConfidenceLevel::kMedium &&
        Contains(e.title, en ? L"was inside plugin code" : L"플러그인 코드 안에 있었음") && Contains(e.details, L"tid 40400"));
    }
    Require(hasEvidence, "evidence item for the blocked thread");
    bool blockedIsCandidate = false;
    for (const auto& c : r.actionable_candidates) {
      Require(c.module_filename != L"hdtsmp64.dll", "the main thread's call path is not a candidate");
      blockedIsCandidate = blockedIsCandidate || c.module_filename == L"CrashTester.dll";
    }
    Require(!r.actionable_candidates.empty() && blockedIsCandidate &&
              r.actionable_candidates.front().module_filename == L"CrashTester.dll",
      "the blocked plugin leads the actionable candidates, not only the summary");
  }

  auto r = MakeBlockedFreeze(L"CrashTester.dll", "waiting_in_plugin");
  BuildEvidenceAndSummary(r, i18n::Language::kEnglish);
  const auto summary = skydiag::dump_tool::BuildSummaryJson(r, std::filesystem::temp_directory_path(), L"stem", false);
  const auto& blocked = summary["freeze_analysis"]["main_thread_wait"]["blocked_thread"];
  Require(blocked["tid"] == 40400 && blocked["module"] == "CrashTester.dll" && blocked["state"] == "waiting_in_plugin",
    "summary JSON blocked_thread");
  const auto report = skydiag::dump_tool::BuildReportText(r, summary, false);
  Require(report.find("blocked_tid=40400 blocked_module=CrashTester.dll blocked_state=waiting_in_plugin") != std::string::npos,
    "report main_thread_wait line");

  const auto keys = skydiag::dump_tool::CollectHistoryCandidateKeys(r);
  // As a candidate it is recorded like any other: by mod name and DLL name.
  Require(!keys.empty(), "history records the blocked plugin");
  for (const auto& key : keys) {
    Require(key.find("crashtester") != std::string::npos && key.find("hdtsmp") == std::string::npos,
      "history records the blocked plugin, not the call path");
  }
}

void TestBlockedPluginSplitsTheHangBucket()
{
  const auto key = [](std::wstring module, std::string state) {
    auto r = MakeBlockedFreeze(std::move(module), std::move(state));
    skydiag::dump_tool::internal::ComputeCrashBucket(r);
    return r.crash_bucket_key;
  };
  const auto sleeping = key(L"CrashTester.dll", "waiting_in_plugin");
  Require(sleeping.rfind(L"HANG1-", 0) == 0, "a wait bucket");
  Require(sleeping == key(L"CRASHTESTER.DLL", "waiting_in_plugin"), "the same plugin and state share a bucket");
  Require(sleeping != key(L"CrashTester.dll", "engine_call_from_plugin"), "the state splits it");
  Require(sleeping != key(L"Other.dll", "waiting_in_plugin"), "another plugin splits it");
  auto plain = MakeBlockedFreeze(L"CrashTester.dll", "waiting_in_plugin");
  plain.main_thread_wait.blocked_thread_tid = 0;
  plain.main_thread_wait.blocked_module.clear();
  skydiag::dump_tool::internal::ComputeCrashBucket(plain);
  Require(plain.crash_bucket_key != sleeping, "a plain engine wait keeps its own bucket");
}

AnalysisResult MakeCppCrash(std::wstring crashLoggerType)
{
  AnalysisResult r{};
  r.dump_path = L"C:\\Logs\\SkyrimDiag_Crash_20261012_192248_896.dmp";
  r.exc_code = 0xE06D7363u;
  r.fault_module_filename = L"KERNELBASE.dll";
  r.crash_logger_exception_type = std::move(crashLoggerType);
  return r;
}

void TestConfirmedCppExceptionIsARealCrash()
{
  for (const auto lang : { i18n::Language::kEnglish, i18n::Language::kKorean }) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = MakeCppCrash(L"C++ Exception");
    Require(skydiag::dump_tool::IsCrashLoggerConfirmedCppException(r), "Crash Logger's C++ Exception line confirms it");
    BuildEvidenceAndSummary(r, lang);
    Require(AnyStartsWith(r.recommendations, en ? L"[Interpretation]" : L"[해석]", en ? L"real CTD" : L"실제 CTD"),
      "explained as a real crash");
    Require(!AnyStartsWith(r.recommendations, en ? L"[Config]" : L"[설정]"), "no CrashHookMode advice");
    Require(!AnyStartsWith(r.recommendations, en ? L"[Interpretation]" : L"[해석]", en ? L"false positive" : L"오탐일 수"),
      "not called a possible false positive");
  }

  auto unconfirmed = MakeCppCrash(L"");
  Require(!skydiag::dump_tool::IsCrashLoggerConfirmedCppException(unconfirmed), "no Crash Logger line, no confirmation");
  BuildEvidenceAndSummary(unconfirmed, i18n::Language::kEnglish);
  Require(AnyStartsWith(unconfirmed.recommendations, L"[Config]"), "without Crash Logger the old caution stays");

  auto other = MakeCppCrash(L"EXCEPTION_ACCESS_VIOLATION");
  Require(!skydiag::dump_tool::IsCrashLoggerConfirmedCppException(other), "only Crash Logger's C++ exception counts");
}

void TestSkseRuntimeIsThePathNotASuspect()
{
  using skydiag::dump_tool::minidump::ModuleInfo;
  const auto module = [](std::wstring name, std::uint64_t base) {
    ModuleInfo m{};
    m.filename = std::move(name);
    m.path = L"G:\\mods\\x\\SKSE\\Plugins\\" + m.filename;
    m.base = base;
    m.end = base + 0x1000;
    return m;
  };
  const std::vector<ModuleInfo> modules = {
    module(L"CrashTester.dll", 0x10000), module(L"skse64_1_6_1170.dll", 0x20000),
  };
  const auto below = skydiag::dump_tool::internal::stackwalk::ComputeCallstackSuspectsFromAddrs(
    modules, { 0x10100, 0x10200, 0x20100 }, i18n::Language::kEnglish);
  Require(below.size() == 1u && below[0].module_filename == L"CrashTester.dll", "SKSE below a plugin is the path");
  const auto faulting = skydiag::dump_tool::internal::stackwalk::ComputeCallstackSuspectsFromAddrs(
    modules, { 0x20100, 0x10100 }, i18n::Language::kEnglish);
  bool hasSkse = false;
  for (const auto& s : faulting) {
    hasSkse = hasSkse || s.module_filename == L"skse64_1_6_1170.dll";
  }
  Require(hasSkse, "SKSE that faulted is still a suspect");

  // A thread-entry hook shape (game x3, plugin, game, system) is left out only
  // when the walk reached the thread start; a walk cut on a system frame
  // without unwind data hides what lies below it.
  std::vector<ModuleInfo> hookModules = {
    module(L"SkyrimSE.exe", 0x30000), module(L"Hook.dll", 0x40000), module(L"KERNELBASE.dll", 0x50000),
  };
  hookModules[0].is_game_exe = true;
  hookModules[2].is_systemish = true;
  const std::vector<std::uint64_t> hookStack = { 0x30100, 0x30200, 0x30300, 0x40100, 0x30400, 0x50100 };
  Require(skydiag::dump_tool::internal::stackwalk::ComputeCallstackSuspectsFromAddrs(
            hookModules, hookStack, i18n::Language::kEnglish, /*walkReachedThreadStart=*/true).empty(),
          "a thread-entry hook of a complete walk is no suspect");
  const auto cut = skydiag::dump_tool::internal::stackwalk::ComputeCallstackSuspectsFromAddrs(
    hookModules, hookStack, i18n::Language::kEnglish, /*walkReachedThreadStart=*/false);
  Require(cut.size() == 1u && cut[0].module_filename == L"Hook.dll", "a cut walk keeps the plugin a suspect");

  AnalysisResult r{};
  r.fault_module_filename = L"CrashTester.dll";
  r.crash_logger_top_modules = { L"CrashTester.dll", L"skse64_1_6_1170.dll" };
  Require(!skydiag::dump_tool::internal::HasNearbyProbableDll(r), "SKSE is not a nearby DLL to compare");
  r.crash_logger_top_modules.push_back(L"NPCWaterAIFix.dll");
  Require(skydiag::dump_tool::internal::HasNearbyProbableDll(r), "another plugin is");
}

const skydiag::dump_tool::EvidenceItem* FindEvidence(const AnalysisResult& r, std::wstring_view titlePart)
{
  for (const auto& e : r.evidence) {
    if (Contains(e.title, titlePart)) {
      return &e;
    }
  }
  return nullptr;
}

// A walk cut short says where and why, so a missing suspect is explained.
void TestTruncatedCallStackIsExplained()
{
  for (const auto lang : { i18n::Language::kEnglish, i18n::Language::kKorean }) {
    const bool en = lang == i18n::Language::kEnglish;
    AnalysisResult r{};
    r.exc_code = 0xC0000005u;
    r.stackwalk_primary_tid = 7;
    r.stackwalk_primary_frames = { L"SkyrimSE.exe+0x1234" };
    r.stackwalk_truncation = "outside_modules";
    r.stackwalk_truncation_depth = 3;
    r.stackwalk_truncation_address = 0x7FF912340000ull;
    r.stackwalk_truncation_unloaded_module = L"FailedPlugin.dll";
    BuildEvidenceAndSummary(r, lang);
    const auto* cut = FindEvidence(r, en ? L"Call stack cut short" : L"콜스택이 중간에 끊김");
    Require(cut != nullptr, "the cut is reported");
    Require(Contains(cut->details, L"FailedPlugin.dll") && Contains(cut->details, L"0x7ff912340000"),
      "names the unloaded DLL and the address");
    Require(Contains(cut->details, en ? L"after 3 frames" : L"3번째 프레임"), "says how far the walk got");
  }

  // A Windows DLL without unwind data: the old-dump-after-update hint.
  AnalysisResult old{};
  old.stackwalk_primary_frames = { L"ntdll.dll+0x1" };
  old.stackwalk_truncation = "no_unwind_info";
  old.stackwalk_truncation_depth = 2;
  old.stackwalk_truncation_module = L"ntdll.dll";
  BuildEvidenceAndSummary(old, i18n::Language::kEnglish);
  const auto* noUnwind = FindEvidence(old, L"Call stack cut short");
  Require(noUnwind && Contains(noUnwind->details, L"ntdll.dll has no unwind data") &&
            Contains(noUnwind->details, L"Windows update"),
    "a system DLL without unwind data points at the Windows update case");

  // A complete walk says nothing.
  AnalysisResult whole{};
  whole.stackwalk_primary_frames = { L"SkyrimSE.exe+0x1234" };
  BuildEvidenceAndSummary(whole, i18n::Language::kEnglish);
  Require(!FindEvidence(whole, L"Call stack cut short"), "no report for a complete walk");

  AnalysisResult gone{};
  gone.exc_code = 0xC0000005u;
  gone.fault_unloaded_module = L"FailedPlugin.dll";
  BuildEvidenceAndSummary(gone, i18n::Language::kKorean);
  const auto* unloaded = FindEvidence(gone, L"언로드된 DLL");
  Require(unloaded && Contains(unloaded->details, L"FailedPlugin.dll"), "a fault in an unloaded DLL names it");
}

// 2026-10-13 17:28:38: an int3 outside every module where the dump holds a
// finished jump to zzzz_TypeMode.dll (ADR-0016). The stack scan and Crash
// Logger pointed at SkyrimUpscaler.dll and a music record instead.
AnalysisResult MakeHookStubRace(std::wstring module, std::uint64_t stub)
{
  AnalysisResult r{};
  r.dump_path = L"C:\\Logs\\SkyrimDiag_Crash_20261013_172838_711.dmp";
  r.exc_code = 0x80000003u;
  r.exc_addr = stub;
  r.hook_stub_race.detected = true;
  r.hook_stub_race.stub_address = stub;
  r.hook_stub_race.target_module = std::move(module);
  r.hook_stub_race.target_mod_name = L"Typing Mode";
  r.hook_stub_race.target_offset = 0x1BFC0u;
  r.hook_stub_race.target = 0x7FFA4C4EBFC0ull;
  r.hook_stub_race.call_site = L"SkyrimSE.exe+0xcd8fbb";
  SuspectItem typeMode{};
  typeMode.module_filename = r.hook_stub_race.target_module;
  typeMode.inferred_mod_name = L"Typing Mode";
  typeMode.confidence_level = i18n::ConfidenceLevel::kHigh;
  typeMode.score = 9;
  SuspectItem upscaler{};
  upscaler.module_filename = L"SkyrimUpscaler.dll";
  upscaler.confidence_level = i18n::ConfidenceLevel::kMedium;
  upscaler.score = 8;
  r.suspects = { typeMode, upscaler };
  r.crash_logger_top_modules = { L"SkyrimUpscaler.dll" };
  r.stackwalk_primary_frames = { L"SkyrimSE.exe+0xcd8fc0", L"SkyrimUpscaler.dll+0x285a7c" };
  return r;
}

void TestHookStubRaceNamesTheHookOwner()
{
  for (const bool en : { true, false }) {
    auto r = MakeHookStubRace(L"zzzz_TypeMode.dll", 0x7FF6AA84583Dull);
    BuildEvidenceAndSummary(r, en ? i18n::Language::kEnglish : i18n::Language::kKorean);
    Require(Contains(r.summary_sentence, L"Typing Mode (zzzz_TypeMode.dll)") &&
              Contains(r.summary_sentence, en ? L"(Confidence: High)" : L"(신뢰도: 높음)"),
      "the conclusion names the hook owner");
    Require(!Contains(r.summary_sentence, L"SkyrimUpscaler"), "the stack scan is not the conclusion");
    Require(!r.actionable_candidates.empty() &&
              r.actionable_candidates[0].display_name == L"zzzz_TypeMode.dll" &&
              r.actionable_candidates[0].confidence_level == i18n::ConfidenceLevel::kHigh,
      "the hook owner is the high-confidence candidate");
    for (const auto& candidate : r.actionable_candidates) {
      Require(candidate.display_name != L"SkyrimUpscaler.dll", "no candidate from the stack scan");
    }
    const auto next = SelectNextActionIndex(r.recommendations);
    Require(next != std::wstring::npos &&
              r.recommendations[next].rfind(en ? L"[Known cause]" : L"[알려진 원인]", 0) == 0 &&
              Contains(r.recommendations[next], L"zzzz_TypeMode.dll"),
      "the next action is the hook owner");
    const auto* evidence = FindEvidence(r, en ? L"Crash in a hook being installed" : L"설치 중인 후킹에서 크래시");
    Require(evidence && Contains(evidence->details, L"0x7ff6aa84583d") &&
              Contains(evidence->details, L"zzzz_TypeMode.dll+0x1bfc0") &&
              Contains(evidence->details, L"SkyrimSE.exe+0xcd8fbb"),
      "the evidence shows the stub, its target and the patched call");
  }

  auto r = MakeHookStubRace(L"zzzz_TypeMode.dll", 0x7FF6AA84583Dull);
  BuildEvidenceAndSummary(r, i18n::Language::kEnglish);
  const auto summary = skydiag::dump_tool::BuildSummaryJson(r, std::filesystem::temp_directory_path(), L"stem", false);
  const auto& race = summary["hook_stub_race"];
  Require(race["stub_address"] == "0x7FF6AA84583D" && race["target_module"] == "zzzz_TypeMode.dll" &&
            race["target_offset"] == "0x1BFC0" && race["call_site"] == "SkyrimSE.exe+0xcd8fbb",
    "summary JSON hook_stub_race");

  const auto key = [](std::wstring module, std::uint64_t stub) {
    auto bucket = MakeHookStubRace(std::move(module), stub);
    skydiag::dump_tool::internal::ComputeCrashBucket(bucket);
    return bucket.crash_bucket_key;
  };
  const auto first = key(L"zzzz_TypeMode.dll", 0x7FF6AA84583Dull);
  Require(first.rfind(L"CTD3-", 0) == 0, "a crash bucket");
  Require(first == key(L"zzzz_TypeMode.dll", 0x7FF6BB000010ull), "the stub address changes per run, the bucket not");
  Require(first != key(L"Other.dll", 0x7FF6AA84583Dull), "another hook owner splits it");
}

}  // namespace

AnalysisResult::CrashLoggerModReference ObjectRef(std::wstring esp, std::wstring type, std::uint32_t relevance)
{
  AnalysisResult::CrashLoggerModReference ref{};
  ref.esp_name = std::move(esp);
  ref.best_object_type = std::move(type);
  ref.best_location = L"RCX";
  ref.ref_count = 1;
  ref.relevance_score = relevance;
  return ref;
}

AnalysisResult MakeExeCrashWithRefs(std::vector<AnalysisResult::CrashLoggerModReference> refs)
{
  AnalysisResult r{};
  r.dump_path = L"SkyrimDiag_Crash_20261014_120000_000.dmp";
  r.exc_code = 0xC0000005u;
  r.fault_module_filename = L"SkyrimSE.exe";
  r.crash_logger_log_path = L"crash-2026-10-14-12-00-00.log";
  r.crash_logger_object_refs = std::move(refs);
  return r;
}

// The 2026-10-07 field crash: the cell the player stood in came from Lux.esp.
// Candidates already skipped it; the summary and advice still named it.
void TestLocationRefIsNotTheCulprit()
{
  for (const auto lang : { i18n::Language::kEnglish, i18n::Language::kKorean }) {
    const bool en = lang == i18n::Language::kEnglish;
    auto cellOnly = MakeExeCrashWithRefs({ ObjectRef(L"Lux.esp", L"TESObjectCELL*", 14) });
    BuildEvidenceAndSummary(cellOnly, lang);
    Require(!Contains(cellOnly.summary_sentence, L"Lux.esp"), "a cell's plugin is not named in the summary");
    Require(!AnyStartsWith(cellOnly.recommendations, L"[ESP/ESM]", L"Lux.esp"), "no advice to disable the cell's plugin");
    Require(cellOnly.actionable_candidates.empty(), "a cell is not a candidate");

    auto withNpc = MakeExeCrashWithRefs({
      ObjectRef(L"Lux.esp", L"TESWorldSpace*", 14),
      ObjectRef(L"NpcMod.esp", L"TESNPC*", 12),
    });
    BuildEvidenceAndSummary(withNpc, lang);
    Require(Contains(withNpc.summary_sentence, L"NpcMod.esp") && !Contains(withNpc.summary_sentence, L"Lux.esp"),
      "the summary names the first non-location ref");
    Require(AnyStartsWith(withNpc.recommendations, L"[ESP/ESM]", L"NpcMod.esp") || !withNpc.actionable_candidates.empty(),
      "the advice points at the object's plugin");
    Require(!AnyStartsWith(withNpc.recommendations, L"[ESP/ESM]", L"Lux.esp"), "never the worldspace's plugin");
    (void)en;
  }
}

// A fault inside Crash Logger is its memory probe while it reported another
// crash; the summary said "known hook framework" while the advice explained it.
void TestCrashLoggerSelfFaultSummary()
{
  for (const auto lang : { i18n::Language::kEnglish, i18n::Language::kKorean }) {
    const bool en = lang == i18n::Language::kEnglish;
    AnalysisResult r{};
    r.dump_path = L"SkyrimDiag_Crash_20261005_122858_100.dmp";
    r.exc_code = 0xC0000005u;
    r.fault_module_filename = L"CrashLogger.dll";
    BuildEvidenceAndSummary(r, lang);
    Require(Contains(r.summary_sentence, en ? L"secondary fault" : L"2차 예외"), "the summary explains the secondary fault");
    Require(!Contains(r.summary_sentence, en ? L"known hook framework" : L"알려진 훅 프레임워크"),
      "Crash Logger is not called a hook framework victim location");
  }
}

int main()
{
  try {
    TestTruncatedCallStackIsExplained();
    TestBlockedGameThreadLeadsTheFreezeReport();
    TestBlockedPluginSplitsTheHangBucket();
    TestConfirmedCppExceptionIsARealCrash();
    TestSkseRuntimeIsThePathNotASuspect();
    TestHookStubRaceNamesTheHookOwner();
    TestLocationRefIsNotTheCulprit();
    TestCrashLoggerSelfFaultSummary();
    std::puts("crash test findings tests passed");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
