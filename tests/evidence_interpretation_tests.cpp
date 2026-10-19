// Interpretation fixes from the 2026-10-19 full code review, pinned on the
// evidence builder's output:
// - H4: an ENB/ReShade d3d11.dll or dxgi.dll in the game folder is a mod, not
//   a Windows system DLL, wherever the analyzer asks;
// - M18: the SKSE runtime, Crash Logger and usvfs never own a freeze from the
//   pointer scan, and one weak clue is not called multi-signal support;
// - M20: a fault-location frame alone does not claim the stack agrees;
// - M21: the pointer scan is not called the "Tullius callstack";
// - L9: C++ exception advice matches the default CrashHookMode=1;
// - L10: one missing-master item, Low once the game reached the world;
// - L13: no crash-during-loading advice for a snapshot on a loading screen;
// - L16: crash history shows UTF-8 module names as written.

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "Analyzer.h"
#include "EvidenceBuilder.h"
#include "MinidumpUtil.h"
#include "SkyrimDiagShared.h"
#include "Utf.h"

namespace {

using skydiag::dump_tool::AnalysisResult;
using skydiag::dump_tool::BuildEvidenceAndSummary;
using skydiag::dump_tool::EventRow;
using skydiag::dump_tool::PluginRuleDiagnosis;
using skydiag::dump_tool::SuspectItem;
using skydiag::dump_tool::WideToUtf8;
using skydiag::dump_tool::minidump::IsPassiveHookModule;
using skydiag::dump_tool::minidump::IsSystemishModuleAt;
namespace i18n = skydiag::dump_tool::i18n;

constexpr i18n::Language kLanguages[] = { i18n::Language::kEnglish, i18n::Language::kKorean };
constexpr wchar_t kGameDir[] = L"G:\\Steam\\steamapps\\common\\Skyrim Special Edition\\";

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

bool AnyContains(const std::vector<std::wstring>& lines, std::wstring_view part)
{
  for (const auto& line : lines) {
    if (Contains(line, part)) {
      return true;
    }
  }
  return false;
}

bool AnyStartsWith(const std::vector<std::wstring>& lines, std::wstring_view prefix)
{
  for (const auto& line : lines) {
    if (line.rfind(prefix, 0) == 0) {
      return true;
    }
  }
  return false;
}

SuspectItem MakeSuspect(std::wstring filename, std::wstring path, i18n::ConfidenceLevel level)
{
  SuspectItem s{};
  s.module_filename = std::move(filename);
  s.module_path = std::move(path);
  s.confidence_level = level;
  s.confidence = i18n::ConfidenceText(i18n::Language::kEnglish, level);
  s.score = 12;
  return s;
}

void TestSystemishByPath()
{
  const std::wstring gameD3d11 = std::wstring(kGameDir) + L"d3d11.dll";
  Require(!IsSystemishModuleAt(L"d3d11.dll", gameD3d11), "ENB d3d11.dll in the game folder is a mod");
  Require(!IsSystemishModuleAt(L"DXGI.DLL", std::wstring(kGameDir) + L"DXGI.DLL"), "ReShade dxgi.dll too");
  Require(IsSystemishModuleAt(L"d3d11.dll", L"C:\\Windows\\System32\\d3d11.dll"), "the Windows copy");
  Require(IsSystemishModuleAt(L"d3d11.dll", L""), "without a path only the name is known");
  Require(IsSystemishModuleAt(L"KERNELBASE.dll", std::wstring(kGameDir) + L"KERNELBASE.dll"), "other system names");
  Require(IsSystemishModuleAt(L"Unknown.dll", L"C:\\Windows\\System32\\Unknown.dll"), "anything under Windows");
  Require(!IsSystemishModuleAt(L"ExampleMod.dll", L"G:\\MO2\\mods\\Example\\ExampleMod.dll"), "a mod DLL");

  Require(IsPassiveHookModule(L"skse64_1_6_1170.dll"), "SKSE runtime");
  Require(IsPassiveHookModule(L"CrashLoggerSSE.dll"), "Crash Logger");
  Require(IsPassiveHookModule(L"usvfs_x64.dll"), "MO2 usvfs");
  Require(!IsPassiveHookModule(L"hdtsmp64.dll"), "a hook framework that runs its own code");
  Require(!IsPassiveHookModule(L"ExampleMod.dll"), "a mod DLL");
}

AnalysisResult MakeCrashIn(std::wstring faultModule, std::wstring faultPath)
{
  AnalysisResult r{};
  r.dump_path = L"C:\\Logs\\SkyrimDiag_Crash_20261019_120000_000.dmp";
  r.exc_code = 0xC0000005u;
  r.exc_addr = 0x7FF612345678ull;
  r.fault_module_filename = std::move(faultModule);
  r.fault_module_path = std::move(faultPath);
  r.fault_module_plus_offset = r.fault_module_filename + L"+0x1234";
  return r;
}

void TestEnbD3d11FaultIsAMod()
{
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    const std::wstring windowsDll = en ? L"Windows system DLL" : L"Windows 시스템 DLL";

    auto r = MakeCrashIn(L"d3d11.dll", std::wstring(kGameDir) + L"d3d11.dll");
    r.suspects.push_back(MakeSuspect(L"d3d11.dll", r.fault_module_path, i18n::ConfidenceLevel::kMedium));
    r.suspects.push_back(
      MakeSuspect(L"OtherPlugin.dll", L"G:\\MO2\\mods\\Other\\SKSE\\Plugins\\OtherPlugin.dll", i18n::ConfidenceLevel::kMedium));
    r.suspects_from_stackwalk = true;
    BuildEvidenceAndSummary(r, lang);
    Require(!Contains(r.summary_sentence, windowsDll), "the game-folder d3d11.dll is not called a Windows DLL");
    Require(Contains(r.summary_sentence, L"d3d11.dll"), "the summary names the injector DLL");
    for (const auto& e : r.evidence) {
      Require(!Contains(e.title, windowsDll), "no Windows-DLL evidence for it");
    }
    bool candidate = false;
    for (const auto& c : r.actionable_candidates) {
      candidate = candidate || c.module_filename == L"d3d11.dll";
    }
    Require(candidate, "the injector DLL on the stack is a candidate like any mod DLL");

    auto windows = MakeCrashIn(L"d3d11.dll", L"C:\\Windows\\System32\\d3d11.dll");
    windows.suspects.push_back(
      MakeSuspect(L"OtherPlugin.dll", L"G:\\MO2\\mods\\Other\\SKSE\\Plugins\\OtherPlugin.dll", i18n::ConfidenceLevel::kMedium));
    windows.suspects_from_stackwalk = true;
    BuildEvidenceAndSummary(windows, lang);
    Require(Contains(windows.summary_sentence, windowsDll), "the System32 copy is still Windows");
  }
}

AnalysisResult MakePointerScanHang(std::wstring topModule, std::wstring topPath)
{
  AnalysisResult r{};
  r.dump_path = L"C:\\Logs\\SkyrimDiag_Hang_20261019_120000_000.dmp";
  r.suspects.push_back(MakeSuspect(std::move(topModule), std::move(topPath), i18n::ConfidenceLevel::kLow));
  r.suspects.push_back(
    MakeSuspect(L"Second.dll", L"G:\\MO2\\mods\\Second\\SKSE\\Plugins\\Second.dll", i18n::ConfidenceLevel::kLow));
  r.suspects_from_stackwalk = false;
  return r;
}

void TestPassiveHookDoesNotOwnAFreeze()
{
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    auto skse = MakePointerScanHang(L"skse64_1_6_1170.dll", std::wstring(kGameDir) + L"skse64_1_6_1170.dll");
    BuildEvidenceAndSummary(skse, lang);
    Require(skse.is_hang_like, "a hang capture");
    for (const auto& c : skse.actionable_candidates) {
      Require(c.module_filename != L"skse64_1_6_1170.dll", "the SKSE runtime owns no freeze");
    }

    auto hdt = MakePointerScanHang(L"hdtsmp64.dll", L"G:\\MO2\\mods\\HDT\\SKSE\\Plugins\\hdtsmp64.dll");
    BuildEvidenceAndSummary(hdt, lang);
    Require(!hdt.actionable_candidates.empty() && hdt.actionable_candidates[0].module_filename == L"hdtsmp64.dll",
      "a hook framework on top of the main-thread scan stays the weak lead");
    Require(hdt.actionable_candidates[0].family_count == 1u, "one family");
    Require(AnyContains(hdt.recommendations, en ? L"A single weak clue points to" : L"약한 단서 하나가"),
      "one weak clue is called that");
    Require(!AnyContains(hdt.recommendations, en ? L"Partial multi-signal support" : L"부분적인 다중 신호"),
      "not multi-signal support");

    // A single strong signal (ADR-0016 hook stub owner, High) is no weak clue.
    auto stub = MakeCrashIn(L"", L"");
    stub.hook_stub_race.detected = true;
    stub.hook_stub_race.target_module = L"Hooker.dll";
    BuildEvidenceAndSummary(stub, lang);
    Require(!stub.actionable_candidates.empty() &&
              stub.actionable_candidates[0].confidence_level == i18n::ConfidenceLevel::kHigh,
      "the hook stub owner leads at High");
    Require(AnyContains(stub.recommendations, en ? L"One signal points to Hooker.dll" : L"신호 하나가 Hooker.dll"),
      "one signal, named as such");
    Require(!AnyContains(stub.recommendations, en ? L"A single weak clue" : L"약한 단서 하나가"), "not called weak");
  }
}

void TestFrameOnlyFaultLocationDoesNotClaimTheStack()
{
  const std::wstring modPath = L"G:\\MO2\\mods\\Mod\\SKSE\\Plugins\\Mod.dll";
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    const std::wstring stackClaim = en ? L"same-dump stack" : L"같은 덤프의 스택";

    auto r = MakeCrashIn(L"Mod.dll", modPath);
    r.crash_logger_direct_fault_eligible = true;
    r.crash_logger_direct_fault_module = L"Mod.dll";
    BuildEvidenceAndSummary(r, lang);
    Require(!r.actionable_candidates.empty() && r.actionable_candidates[0].status_id == "reference_clue",
      "a frame-only fault-location clue");
    Require(AnyStartsWith(r.recommendations, en ? L"[Actionable candidate]" : L"[행동 우선 후보]"), "candidate advice");
    Require(!AnyContains(r.recommendations, stackClaim), "no stack agreement without a stack signal");

    auto withStack = MakeCrashIn(L"Mod.dll", modPath);
    withStack.crash_logger_direct_fault_eligible = true;
    withStack.crash_logger_direct_fault_module = L"Mod.dll";
    withStack.suspects.push_back(MakeSuspect(L"Mod.dll", modPath, i18n::ConfidenceLevel::kMedium));
    withStack.suspects_from_stackwalk = true;
    BuildEvidenceAndSummary(withStack, lang);
    Require(AnyContains(withStack.recommendations, stackClaim), "a stack signal may say both");
  }
}

AnalysisResult MakeExeCrashWithFirstChance(bool fromStackwalk, i18n::ConfidenceLevel suspectLevel)
{
  auto r = MakeCrashIn(L"SkyrimSE.exe", std::wstring(kGameDir) + L"SkyrimSE.exe");
  r.suspects.push_back(MakeSuspect(L"Foo.dll", L"G:\\MO2\\mods\\Foo\\SKSE\\Plugins\\Foo.dll", suspectLevel));
  r.suspects_from_stackwalk = fromStackwalk;
  r.first_chance_summary.has_context = true;
  r.first_chance_summary.recent_count = 3;
  r.first_chance_summary.repeated_signature_count = 1;
  r.first_chance_summary.recent_non_system_modules.push_back(L"Foo.dll");
  return r;
}

void TestPointerScanIsNotTheTulliusCallstack()
{
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    const std::wstring callstack = en ? L"Tullius callstack" : L"Tullius 콜스택";

    // Stack scan (2) + first-chance context (3) reached the old score-5 bar.
    auto scan = MakeExeCrashWithFirstChance(/*fromStackwalk=*/false, i18n::ConfidenceLevel::kLow);
    BuildEvidenceAndSummary(scan, lang);
    Require(!scan.actionable_candidates.empty() && scan.actionable_candidates[0].score >= 5u, "the old bar is met");
    Require(!Contains(scan.summary_sentence, callstack), "the summary does not call the stack scan a callstack");
    Require(!AnyContains(scan.recommendations, callstack), "nor does the advice");

    auto walk = MakeExeCrashWithFirstChance(/*fromStackwalk=*/true, i18n::ConfidenceLevel::kMedium);
    BuildEvidenceAndSummary(walk, lang);
    Require(Contains(walk.summary_sentence, callstack), "a real stack walk still is the Tullius callstack");
    Require(AnyContains(walk.recommendations, callstack), "in the advice too");
  }
}

void TestUnconfirmedCppExceptionAdvice()
{
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = MakeCrashIn(L"KERNELBASE.dll", L"C:\\Windows\\System32\\KERNELBASE.dll");
    r.exc_code = 0xE06D7363u;
    BuildEvidenceAndSummary(r, lang);
    Require(AnyContains(r.recommendations, en ? L"set it back to 1 (the default)" : L"기본값 1로 되돌리세요"),
      "CrashHookMode advice is about undoing mode 2");
    Require(!AnyContains(r.recommendations, en ? L"CrashHookMode=1 (fatal only)" : L"CrashHookMode=1(치명 예외만)로 두면"),
      "no advice to set the default that is already set");
    Require(Contains(r.summary_sentence, en ? L"no Crash Logger log confirmed it" : L"Crash Logger 로그로 확인하지 못했습니다"),
      "the summary says what is missing");
  }
}

AnalysisResult MakeMissingMasterCrash(bool reachedWorld)
{
  auto r = MakeCrashIn(L"SkyrimSE.exe", std::wstring(kGameDir) + L"SkyrimSE.exe");
  r.missing_masters.push_back(L"Gone.esm");
  PluginRuleDiagnosis pd{};
  pd.rule_id = "MISSING_MASTER";
  pd.confidence_level = i18n::ConfidenceLevel::kHigh;
  pd.confidence = L"High";
  pd.cause = L"A master of an active plugin is not in the plugin list.";
  pd.recommendations.push_back(L"[Required] Install the missing master mod or deactivate the plugin");
  r.plugin_diagnostics.push_back(pd);
  if (reachedWorld) {
    EventRow cell{};
    cell.type = static_cast<std::uint16_t>(skydiag::EventType::kCellChange);
    cell.t_ms = 1000.0;
    r.events.push_back(cell);
  }
  return r;
}

void TestOneMissingMasterItemLowAfterLoad()
{
  for (const bool reachedWorld : { false, true }) {
    auto r = MakeMissingMasterCrash(reachedWorld);
    BuildEvidenceAndSummary(r, i18n::Language::kEnglish);
    std::size_t items = 0;
    for (const auto& e : r.evidence) {
      if (Contains(e.details, L"Gone.esm")) {
        ++items;
        Require(e.confidence_level == (reachedWorld ? i18n::ConfidenceLevel::kLow : i18n::ConfidenceLevel::kHigh),
          "High only before the game reached the world");
        Require(Contains(e.details, L"may be stale") == reachedWorld, "the stale-list note after a load");
      }
    }
    Require(items == 1u, "one evidence item per missing-master finding");
    Require(!AnyStartsWith(r.recommendations, L"[Masters]"), "the rule's advice is not repeated");
    Require(AnyStartsWith(r.recommendations, L"[Required]"), "the rule's advice stays");
  }

  // Without plugin_rules.json the list keeps its own item and advice.
  auto bare = MakeMissingMasterCrash(/*reachedWorld=*/false);
  bare.plugin_diagnostics.clear();
  BuildEvidenceAndSummary(bare, i18n::Language::kEnglish);
  Require(AnyStartsWith(bare.recommendations, L"[Masters]"), "fallback advice");
}

void TestLoadingSnapshotGetsNoCrashAdvice()
{
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    const std::wstring loading = en ? L"[Loading]" : L"[로딩 중]";

    AnalysisResult snapshot{};
    snapshot.dump_path = L"C:\\Logs\\SkyrimDiag_Manual_20261019_120000_000.dmp";
    snapshot.state_flags = skydiag::kState_Loading;
    BuildEvidenceAndSummary(snapshot, lang);
    Require(snapshot.is_snapshot_like, "a snapshot");
    Require(!AnyStartsWith(snapshot.recommendations, loading), "no crash-during-loading advice");

    auto crash = MakeCrashIn(L"SkyrimSE.exe", std::wstring(kGameDir) + L"SkyrimSE.exe");
    crash.state_flags = skydiag::kState_Loading;
    BuildEvidenceAndSummary(crash, lang);
    Require(AnyStartsWith(crash.recommendations, loading), "a crash on a loading screen keeps it");
  }
}

void TestHistoryShowsUtf8ModuleNames()
{
  auto r = MakeCrashIn(L"SkyrimSE.exe", std::wstring(kGameDir) + L"SkyrimSE.exe");
  skydiag::dump_tool::ModuleStats stats{};
  stats.module_name = WideToUtf8(L"한글패치.dll");
  stats.total_appearances = 2;
  stats.as_top_suspect = 1;
  stats.total_crashes = 3;
  r.history_stats.push_back(stats);
  BuildEvidenceAndSummary(r, i18n::Language::kKorean);
  bool shown = false;
  for (const auto& e : r.evidence) {
    shown = shown || Contains(e.details, L"한글패치.dll");
  }
  Require(shown, "the Korean module name is readable");
}

}  // namespace

int main()
{
  try {
    TestSystemishByPath();
    TestEnbD3d11FaultIsAMod();
    TestPassiveHookDoesNotOwnAFreeze();
    TestFrameOnlyFaultLocationDoesNotClaimTheStack();
    TestPointerScanIsNotTheTulliusCallstack();
    TestUnconfirmedCppExceptionAdvice();
    TestOneMissingMasterItemLowAfterLoad();
    TestLoadingSnapshotGetsNoCrashAdvice();
    TestHistoryShowsUtf8ModuleNames();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
  return 0;
}
