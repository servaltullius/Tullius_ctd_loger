// Interpretation fixes from the 2026-10-19 full code review, pinned on the
// evidence builder's output:
// - H4: an ENB/ReShade d3d11.dll or dxgi.dll in the game folder is a mod, not
//   a Windows system DLL, wherever the analyzer asks (also in a paired Crash
//   Logger log's frames);
// - M18: the SKSE runtime, Crash Logger and usvfs never own a freeze from the
//   pointer scan, and one weak clue is not called multi-signal support;
// - M20: a fault-location frame alone does not claim the stack agrees;
// - M21: the pointer scan is not called the "Tullius callstack";
// - L9: C++ exception advice matches the default CrashHookMode=1;
// - L10: one missing-master item, Low once the game reached the world;
// - L12/L13: one heartbeat rule for a manual capture on a loading screen, and
//   no crash-during-loading advice for a snapshot there;
// - L16: crash history shows UTF-8 module names as written.

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "Analyzer.h"
#include "AnalyzerPipeline.h"
#include "CrashLogger.h"
#include "EvidenceBuilder.h"
#include "FreezeCandidateConsensus.h"
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

using skydiag::dump_tool::minidump::ModuleInfo;

ModuleInfo LoadedModule(std::wstring filename, std::wstring dir, std::uint64_t base)
{
  ModuleInfo m{};
  m.filename = filename;
  m.path = dir + filename;
  m.base = base;
  m.end = base + 0x100000ull;
  return m;
}

// The ENB proxy loads the Windows d3d11.dll too, so the module list holds
// both; the frame addresses tell them apart.
std::vector<ModuleInfo> EnbModules()
{
  return {
    LoadedModule(L"SkyrimSE.exe", kGameDir, 0x7FF612300000ull),
    LoadedModule(L"d3d11.dll", kGameDir, 0x7FF900000000ull),
    LoadedModule(L"d3d11.dll", L"C:\\Windows\\System32\\", 0x7FFA00000000ull),
    LoadedModule(L"KERNEL32.DLL", L"C:\\Windows\\System32\\", 0x7FFB00000000ull),
    LoadedModule(L"ntdll.dll", L"C:\\Windows\\System32\\", 0x7FFC00000000ull),
  };
}

// A crash in the d3d11.dll at `top` (the ENB copy at 0x7FF9..., the Windows
// one at 0x7FFA...), two frames there, then the game and the thread start.
std::string CrashLoggerLogWithD3d11Top(const char* top)
{
  const std::string t(top);
  return "CRASH TIME: 2026-10-19 12:00:00\n"
         "CrashLoggerSSE v1-24-0-0 Jun 28 2026 00:14:22\n\n"
         "Unhandled exception \"EXCEPTION_ACCESS_VIOLATION\" at 0x" + t + "1234 d3d11.dll+0001234\tmov rax, [rcx]\n\n"
         "PROBABLE CALL STACK:\n"
         "\t[ 0] 0x" + t + "1234 d3d11.dll+0001234\n"
         "\t[ 1] 0x" + t + "5678 d3d11.dll+0005678\n"
         "\t[ 2] 0x7FF612340001 SkyrimSE.exe+0040001\n"
         "\t[ 3] 0x7FFB00001000 KERNEL32.DLL+0001000\n"
         "\t[ 4] 0x7FFC00002000 ntdll.dll+0002000\n\n"
         "REGISTERS:\n";
}

AnalysisResult IntegrateD3d11CrashLog(const char* top, const std::filesystem::path& logPath)
{
  {
    std::ofstream log(logPath, std::ios::binary | std::ios::trunc);
    log << CrashLoggerLogWithD3d11Top(top);
  }
  AnalysisResult r = MakeCrashIn(L"d3d11.dll", std::wstring(kGameDir) + L"d3d11.dll");
  r.fault_module_offset = 0x1234u;
  r.crash_logger_log_path = logPath.wstring();
  skydiag::dump_tool::IntegrateCrashLoggerFrameSignals(EnbModules(), &r);
  return r;
}

void TestEnbD3d11CrashLoggerFramesAreAMod()
{
  const auto logPath = std::filesystem::temp_directory_path() / "skydiag_h4_enb_crashlogger.log.txt";
  const auto modules = EnbModules();

  auto enb = IntegrateD3d11CrashLog("7FF90000", logPath);
  Require(enb.crash_logger_direct_fault_module == L"d3d11.dll" && enb.crash_logger_direct_fault_eligible,
    "the fault in the game-folder d3d11.dll is a promotable direct fault");
  Require(enb.crash_logger_first_actionable_probable_module == L"d3d11.dll" &&
            enb.crash_logger_first_actionable_probable_eligible,
    "its top frame is the first actionable frame");
  Require(enb.crash_logger_probable_streak_module == L"d3d11.dll" && enb.crash_logger_probable_streak_length == 2u &&
            enb.crash_logger_probable_streak_eligible,
    "and its two frames a streak");
  Require(enb.crash_logger_frame_signal_strength > 0u, "the frames carry weight");
  BuildEvidenceAndSummary(enb, i18n::Language::kEnglish);
  bool candidate = false;
  for (const auto& c : enb.actionable_candidates) {
    candidate = candidate || c.module_filename == L"d3d11.dll";
  }
  Require(candidate, "the ENB d3d11.dll is a candidate from the Crash Logger frames");

  const std::unordered_map<std::wstring, std::wstring> noCanonical;
  const auto log = CrashLoggerLogWithD3d11Top("7FF90000");
  const auto top = skydiag::dump_tool::ParseCrashLoggerTopModules(
    log, noCanonical, skydiag::dump_tool::CrashLoggerSystemNamedModTest(modules));
  Require(!top.empty() && top.front() == L"d3d11.dll", "and ranks first among the log's modules");

  // The same frames in the Windows copy stay a system DLL.
  auto windows = IntegrateD3d11CrashLog("7FFA0000", logPath);
  Require(!windows.crash_logger_direct_fault_eligible, "a fault in System32's d3d11.dll promotes nothing");
  Require(windows.crash_logger_first_actionable_probable_module.empty() &&
            !windows.crash_logger_first_actionable_probable_eligible,
    "no first actionable frame");
  Require(!windows.crash_logger_probable_streak_eligible, "no streak");
  Require(skydiag::dump_tool::ParseCrashLoggerTopModules(
            CrashLoggerLogWithD3d11Top("7FFA0000"), noCanonical,
            skydiag::dump_tool::CrashLoggerSystemNamedModTest(modules)).empty(),
    "nor a top module");
  std::error_code ec;
  std::filesystem::remove(logPath, ec);

  // A frame without an address is judged by name: only when no Windows copy
  // of that name is loaded.
  const auto bothLoaded = skydiag::dump_tool::CrashLoggerSystemNamedModTest(modules);
  Require(!bothLoaded("d3d11.dll", 0u), "both copies loaded: ambiguous, so Windows");
  const std::vector<ModuleInfo> enbOnly = { LoadedModule(L"d3d11.dll", kGameDir, 0x7FF900000000ull) };
  Require(skydiag::dump_tool::CrashLoggerSystemNamedModTest(enbOnly)("D3D11.DLL", 0u), "only the game-folder copy");
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

// A Crash Logger log that may belong to another crash (an ambiguous time
// pairing, or a different exception location) is a Low clue in the evidence
// and raises no candidate; the same log paired cleanly does.
void TestAmbiguousCrashLoggerPairingRaisesNoCandidate()
{
  for (const auto lang : kLanguages) {
    for (const bool ambiguous : { false, true }) {
      auto r = MakeCrashIn(L"SkyrimSE.exe", std::wstring(kGameDir) + L"SkyrimSE.exe");
      r.crash_logger_log_path = L"crash-2026-10-30-16-52-59.log";
      r.crash_logger_first_actionable_probable_eligible = true;
      r.crash_logger_first_actionable_probable_module = L"Mod.dll";
      r.crash_logger_probable_streak_eligible = true;
      r.crash_logger_probable_streak_module = L"Mod.dll";
      r.crash_logger_probable_streak_length = 2;
      r.crash_logger_pairing_ambiguous = ambiguous;
      BuildEvidenceAndSummary(r, lang);
      bool named = false;
      for (const auto& candidate : r.actionable_candidates) {
        named = named || candidate.module_filename == L"Mod.dll";
      }
      if (ambiguous) {
        Require(!named, "an ambiguous Crash Logger pairing raises no candidate");
        Require(!Contains(r.summary_sentence, L"Mod.dll"), "nor names its module in the summary");
      } else {
        Require(named, "the same log paired cleanly raises its frame module");
      }
    }
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
    // rc5's plugin also records an unhandled throw without Crash Logger, so a
    // missing Crash Logger log is no reason for doubt.
    Require(Contains(r.summary_sentence, en ? L"only when nothing handled it" : L"어디에서도 처리되지 않은 C++ 예외만"),
      "the summary says what the default mode records");
    Require(Contains(r.summary_sentence, L"abort()"), "including the paths without Crash Logger");
    Require(!Contains(r.summary_sentence, en ? L"no Crash Logger log confirmed it" : L"Crash Logger 로그로 확인하지 못했습니다"),
      "no Crash Logger log is no reason for low confidence");
    Require(!Contains(r.summary_sentence, en ? L"only while Crash Logger reports it" : L"Crash Logger가 크래시로 보고할 때만"),
      "nor is Crash Logger the only way it is recorded");
    for (const auto& line : r.recommendations) {
      Require(!Contains(line, en ? L"only while Crash Logger reports it" : L"Crash Logger가 크래시로 보고할 때만"),
        "the advice agrees");
    }
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

// A manual capture on a loading screen gets one verdict: the freeze state and
// the [Manual] advice both use kNotHangHeartbeatAgeSec, not the helper's
// longer hang threshold.
void TestManualLoadingCaptureHasOneHeartbeatRule()
{
  for (const double age : { 0.4, 20.0 }) {
    const bool fresh = age < skydiag::dump_tool::internal::kNotHangHeartbeatAgeSec;
    skydiag::dump_tool::FreezeSignalInput input{};
    input.is_snapshot_like = true;
    input.is_manual_capture = true;
    input.loading_context = true;
    input.wct = skydiag::dump_tool::internal::WctFreezeSummary{};
    input.wct->has = true;
    input.wct->has_capture = true;
    input.wct->capture_kind = "manual";
    input.wct->isLoading = true;
    input.wct->thresholdSec = 60u;
    input.wct->secondsSinceHeartbeat = age;

    AnalysisResult r{};
    r.dump_path = L"C:\\Logs\\SkyrimDiag_Manual_20261019_120000_000.dmp";
    r.state_flags = skydiag::kState_Loading;
    r.has_wct = true;
    r.wct_json_utf8 = std::string(R"({"capture":{"kind":"manual","secondsSinceHeartbeat":)") +
                      (fresh ? "0.4" : "20.0") + R"(,"thresholdSec":60,"isLoading":true},"threads":[]})";
    r.freeze_analysis = skydiag::dump_tool::BuildFreezeCandidateConsensus(input, i18n::Language::kEnglish);
    BuildEvidenceAndSummary(r, i18n::Language::kEnglish);
    Require(r.is_snapshot_like, "a manual snapshot");
    const bool stuck = r.freeze_analysis.state_id == "loader_stall_likely";
    const bool notAHang = AnyStartsWith(r.recommendations, L"[Manual] At capture time");
    Require(stuck == !fresh, "a stuck load only when the main thread had stopped answering");
    Require(notAHang == fresh, "'not a hang' only when the freeze verdict agrees");
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

// Text from the dump's WCT stream and from crash_history.json reaches the
// evidence, summary and advice without a fixed buffer: swprintf_s ends the
// process when the text does not fit.
void TestLongDumpTextNeedsNoFixedBuffer()
{
  const std::string kind(4000, 'k');
  for (const char* age : { "1e308", "-1e308" }) {
    for (const bool manual : { false, true }) {
      AnalysisResult r{};
      r.dump_path = manual ? L"C:\\Logs\\SkyrimDiag_Manual_20261019_120000_000.dmp"
                           : L"C:\\Logs\\SkyrimDiag_Hang_20261019_120000_000.dmp";
      r.has_wct = true;
      r.wct_json_utf8 = std::string(R"({"capture":{"kind":")") + kind + R"(","secondsSinceHeartbeat":)" + age +
                        R"(,"thresholdSec":10,"isLoading":false},"threads":[]})";
      r.history_correlation.count = 3;
      r.history_correlation.first_seen = std::string(4000, '9');
      BuildEvidenceAndSummary(r, i18n::Language::kEnglish);
      bool wctShown = false;
      bool historyShown = false;
      for (const auto& e : r.evidence) {
        wctShown = wctShown || Contains(e.details, L"capture=" + std::wstring(4000, L'k') + L", threads=0");
        historyShown = historyShown || Contains(e.details, L"(first: " + std::wstring(4000, L'9') + L")");
      }
      Require(wctShown, "the whole WCT capture kind is in the WCT summary");
      Require(historyShown, "the whole first_seen is in the repeated-pattern evidence");
    }
  }
}

}  // namespace

int main()
{
  try {
    TestSystemishByPath();
    TestEnbD3d11FaultIsAMod();
    TestEnbD3d11CrashLoggerFramesAreAMod();
    TestPassiveHookDoesNotOwnAFreeze();
    TestFrameOnlyFaultLocationDoesNotClaimTheStack();
    TestAmbiguousCrashLoggerPairingRaisesNoCandidate();
    TestPointerScanIsNotTheTulliusCallstack();
    TestUnconfirmedCppExceptionAdvice();
    TestOneMissingMasterItemLowAfterLoad();
    TestLoadingSnapshotGetsNoCrashAdvice();
    TestManualLoadingCaptureHasOneHeartbeatRule();
    TestHistoryShowsUtf8ModuleNames();
    TestLongDumpTextNeedsNoFixedBuffer();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
  return 0;
}
