// The quick answer (QuickAnswer.h): a Nexus user found the report "too
// complicated ... we are just playing games" (2026-10-21). Players get what
// happened, the suspect, one thing to try and why it is not certain, in plain
// words, before any technical detail; the analysis itself is unchanged.

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
#include "EvidenceBuilder.h"
#include "EvidenceBuilderPrivate.h"
#include "OutputWriterPipeline.h"
#include "SignatureDatabase.h"
#include "Utf.h"

namespace {

using skydiag::dump_tool::ActionableCandidate;
using skydiag::dump_tool::AnalysisResult;
using skydiag::dump_tool::BuildEvidenceAndSummary;
using skydiag::dump_tool::QuickAnswer;
using skydiag::dump_tool::SuspectItem;
using skydiag::dump_tool::WideToUtf8;
using skydiag::dump_tool::internal::BuildQuickAnswer;
using skydiag::dump_tool::internal::EvidenceBuildContext;
namespace i18n = skydiag::dump_tool::i18n;

constexpr i18n::Language kLanguages[] = { i18n::Language::kEnglish, i18n::Language::kKorean };

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

// Words a player should not meet in the first lines. A file name in
// parentheses is fine ("(SmoothCam.dll)"), so "DLL" is matched upper case.
void RequirePlainWords(const QuickAnswer& qa, const char* what)
{
  for (const std::wstring_view field : { std::wstring_view(qa.headline), std::wstring_view(qa.action) }) {
    for (const std::wstring_view jargon :
         { L"DLL", L"stack", L"Stack", L"스택", L"WCT", L"heartbeat", L"signal", L"신호", L"Confidence", L"신뢰도" }) {
      if (Contains(field, jargon)) {
        std::fprintf(stderr, "jargon \"%s\" in: %s\n", WideToUtf8(std::wstring(jargon)).c_str(),
          WideToUtf8(std::wstring(field)).c_str());
        Require(false, what);
      }
    }
  }
  Require(!qa.headline.empty() && !qa.action.empty(), what);
}

EvidenceBuildContext CrashContext(bool en, std::wstring_view faultModule)
{
  EvidenceBuildContext ctx{};
  ctx.en = en;
  ctx.hasException = true;
  ctx.isCrashLike = true;
  ctx.hasModule = !faultModule.empty();
  return ctx;
}

EvidenceBuildContext FreezeContext(bool en)
{
  EvidenceBuildContext ctx{};
  ctx.en = en;
  ctx.isHangLike = true;
  return ctx;
}

AnalysisResult CrashIn(std::wstring module, std::wstring modName)
{
  AnalysisResult r{};
  r.dump_path = L"C:\\Logs\\SkyrimDiag_Crash_20261021_053954_072.dmp";
  r.exc_code = 0xC0000005u;
  r.fault_module_filename = std::move(module);
  r.fault_module_path = L"G:\\MO2\\mods\\" + modName + L"\\SKSE\\Plugins\\" + r.fault_module_filename;
  r.fault_module_plus_offset = r.fault_module_filename + L"+0x1de5";
  r.inferred_mod_name = std::move(modName);
  return r;
}

ActionableCandidate Candidate(
  std::string status,
  i18n::ConfidenceLevel level,
  std::wstring module,
  std::wstring modName,
  std::vector<std::string> families)
{
  ActionableCandidate c{};
  c.status_id = std::move(status);
  c.confidence_level = level;
  c.confidence = i18n::ConfidenceText(i18n::Language::kEnglish, level);
  c.module_filename = module;
  c.display_name = std::move(module);
  c.primary_identifier = c.display_name;
  c.mod_name = std::move(modName);
  c.supporting_families = std::move(families);
  c.family_count = static_cast<std::uint32_t>(c.supporting_families.size());
  c.cross_validated = c.status_id == "cross_validated";
  return c;
}

void TestCrashWithHighCandidate()
{
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = CrashIn(L"ExampleMod.dll", L"Example Mod");
    r.actionable_candidates.push_back(Candidate(
      "cross_validated", i18n::ConfidenceLevel::kHigh, L"ExampleMod.dll", L"Example Mod",
      { "actionable_stack", "crash_logger_frame", "history_repeat" }));
    const auto qa = BuildQuickAnswer(r, lang, CrashContext(en, r.fault_module_filename));
    Require(qa.kind == "crash" && qa.case_id == "candidate", "a crash with a candidate");
    Require(qa.likelihood == "high", "the candidate's High");
    Require(qa.suspect_mod == L"Example Mod" && qa.suspect_file == L"ExampleMod.dll", "MO2 mod name and file");
    Require(qa.likelihood_text == (en ? L"likely" : L"가능성 높음"), "plain likelihood");
    Require(qa.why_not_certain.empty(), "no doubt line when likely");
    Require(Contains(qa.action, en ? L"Disable or update Example Mod" : L"Example Mod 모드를 끄거나 업데이트"),
      "the action names the mod");
    RequirePlainWords(qa, "high candidate");
  }
}

void TestCrashWithMediumCandidateElsewhere()
{
  // The 2026-10-21 field crash: reported in ffx_backend_dx11_x64.dll, the
  // candidate is ffx_fsr3upscaler_x64.dll of the same MO2 mod.
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = CrashIn(L"ffx_backend_dx11_x64.dll", L"SkyrimUpscalerAIOBuild22");
    r.actionable_candidates.push_back(Candidate(
      "related", i18n::ConfidenceLevel::kMedium, L"ffx_fsr3upscaler_x64.dll", L"SkyrimUpscalerAIOBuild22",
      { "actionable_stack", "crash_logger_frame" }));
    const auto qa = BuildQuickAnswer(r, lang, CrashContext(en, r.fault_module_filename));
    Require(qa.case_id == "candidate" && qa.likelihood == "medium", "the Medium candidate");
    Require(qa.suspect_mod == L"SkyrimUpscalerAIOBuild22", "the MO2 mod, not a raw path");
    Require(qa.suspect_file == L"ffx_fsr3upscaler_x64.dll", "the candidate's file");
    Require(Contains(qa.headline, L"SkyrimUpscalerAIOBuild22 (ffx_fsr3upscaler_x64.dll)"), "headline names mod and file");
    Require(qa.likelihood_text == (en ? L"possible" : L"가능성 중간"), "plain likelihood");
    Require(Contains(qa.why_not_certain, en ? L"crashed inside this mod" : L"이 모드 안에서 튕겼지만"),
      "the same mod as the fault module");
    RequirePlainWords(qa, "medium candidate");

    // A candidate in another mod than the fault module.
    auto other = CrashIn(L"Victim.dll", L"Victim Mod");
    other.actionable_candidates.push_back(Candidate(
      "related", i18n::ConfidenceLevel::kMedium, L"Culprit.dll", L"Culprit Mod", { "crash_logger_frame" }));
    const auto elsewhere = BuildQuickAnswer(other, lang, CrashContext(en, other.fault_module_filename));
    Require(elsewhere.suspect_mod == L"Culprit Mod", "the candidate, not the fault module");
    Require(Contains(elsewhere.why_not_certain, en ? L"another file" : L"다른 파일"), "crashed elsewhere");
  }
}

void TestCrashWithLowFaultModuleOnly()
{
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = CrashIn(L"ExampleMod.dll", L"Example Mod");
    const auto qa = BuildQuickAnswer(r, lang, CrashContext(en, r.fault_module_filename));
    Require(qa.case_id == "fault_module" && qa.likelihood == "low", "only the fault location");
    Require(qa.likelihood_text == (en ? L"weak" : L"가능성 낮음"), "plain likelihood");
    Require(Contains(qa.why_not_certain, en ? L"another mod can cause a crash that shows up here"
                                           : L"다른 모드가 일으킨 문제가 여기서 드러나는"),
      "the victim-location caveat in plain words");
    RequirePlainWords(qa, "low fault module");
  }
}

void TestConflictingCandidatesNameBoth()
{
  auto r = CrashIn(L"ExampleMod.dll", L"Example Mod");
  r.actionable_candidates.push_back(
    Candidate("conflicting", i18n::ConfidenceLevel::kMedium, L"First.dll", L"First Mod", { "crash_logger_frame" }));
  r.actionable_candidates.push_back(
    Candidate("conflicting", i18n::ConfidenceLevel::kMedium, L"Second.dll", L"Second Mod", { "actionable_stack" }));
  const auto qa = BuildQuickAnswer(r, i18n::Language::kEnglish, CrashContext(true, r.fault_module_filename));
  Require(qa.suspect_mod == L"First Mod", "the first candidate leads");
  Require(Contains(qa.why_not_certain, L"Second Mod"), "the doubt names the other mod");
}

void TestCrashWithNoSuspect()
{
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    AnalysisResult r{};
    r.exc_code = 0xC0000005u;
    r.fault_module_filename = L"ntdll.dll";
    r.fault_module_path = L"C:\\Windows\\System32\\ntdll.dll";
    auto ctx = CrashContext(en, r.fault_module_filename);
    ctx.isSystem = true;
    const auto qa = BuildQuickAnswer(r, lang, ctx);
    Require(qa.case_id == "none" && qa.likelihood == "none", "nothing to name");
    Require(qa.suspect_mod.empty() && qa.likelihood_text.empty() && qa.why_not_certain.empty(), "no suspect fields");
    Require(Contains(qa.headline, en ? L"could not name a mod" : L"원인 모드는 특정하지 못했습니다"), "says so");
    Require(Contains(qa.action, en ? L"Share this report when asking for help" : L"이 리포트를 함께 공유"),
      "share the report");
    RequirePlainWords(qa, "no suspect");
  }
}

void TestExternalSoftware()
{
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    AnalysisResult r{};
    r.exc_code = 0xC0000005u;
    r.fault_module_filename = L"NvTelemetryBridge64.dll";
    r.fault_module_path = L"C:\\Program Files\\NVIDIA Corporation\\NvTelemetry\\NvTelemetryBridge64.dll";
    r.fault_module_external_software = L"NVIDIA";
    auto ctx = CrashContext(en, r.fault_module_filename);
    ctx.isSystem = true;
    ctx.externalSoftware = r.fault_module_external_software;
    const auto qa = BuildQuickAnswer(r, lang, ctx);
    Require(qa.case_id == "external_software" && qa.likelihood == "low", "a program outside the mods");
    Require(qa.suspect_mod == L"NVIDIA" && qa.suspect_file == L"NvTelemetryBridge64.dll", "program and file");
    Require(Contains(qa.headline, en ? L"Suspected program: NVIDIA" : L"의심되는 프로그램: NVIDIA"), "not called a mod");
    Require(Contains(qa.action, en ? L"overlay" : L"오버레이"), "update, reinstall or turn the overlay off");
    Require(!Contains(qa.action, en ? L"Disable or update" : L"모드를 끄거나"), "not mod advice");
    RequirePlainWords(qa, "external software");
  }
}

void TestKnownIssueUsesItsFix()
{
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    AnalysisResult r{};
    r.exc_code = 0xE06D7363u;
    r.fault_module_filename = L"KERNELBASE.dll";
    r.fault_module_path = L"C:\\Windows\\System32\\KERNELBASE.dll";
    skydiag::dump_tool::SignatureMatch sig{};
    sig.id = "CONTENT_CATALOG_FORMAT_MISMATCH";
    sig.scope = "root_cause";
    sig.confidence_level = i18n::ConfidenceLevel::kMedium;
    sig.recommendations.push_back(en
      ? L"[Known cause] Move ContentCatalog.txt to the Recycle Bin and start the game again. It can come back."
      : L"[알려진 원인] ContentCatalog.txt를 휴지통으로 옮긴 뒤 게임을 다시 실행하세요. 다시 생길 수 있습니다.");
    r.signature_match = sig;
    auto ctx = CrashContext(en, r.fault_module_filename);
    ctx.isSystem = true;
    const auto qa = BuildQuickAnswer(r, lang, ctx);
    Require(qa.case_id == "known_issue" && qa.likelihood == "medium", "the known problem leads");
    Require(qa.action == (en ? L"Move ContentCatalog.txt to the Recycle Bin and start the game again."
                             : L"ContentCatalog.txt를 휴지통으로 옮긴 뒤 게임을 다시 실행하세요."),
      "its fix, first sentence, without the tag");
    Require(!qa.why_not_certain.empty(), "Medium keeps a doubt line");

    // A mechanism pattern only says how it crashed: the lead stays.
    sig.scope = "mechanism";
    r.signature_match = sig;
    Require(BuildQuickAnswer(r, lang, ctx).case_id != "known_issue", "no fix for a mechanism");
  }
}

void TestHookStubRaceIsLikely()
{
  AnalysisResult r{};
  r.exc_code = 0x80000003u;
  r.hook_stub_race.detected = true;
  r.hook_stub_race.target_module = L"zzzz_TypeMode.dll";
  r.hook_stub_race.target_mod_name = L"Typing Mode";
  const auto qa = BuildQuickAnswer(r, i18n::Language::kEnglish, CrashContext(true, L""));
  Require(qa.case_id == "hook_stub_race" && qa.likelihood == "high", "the hook's owner");
  Require(qa.suspect_mod == L"Typing Mode" && qa.why_not_certain.empty(), "named, no doubt line");
}

void TestFreezeLockOwnerInGpuDriver()
{
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    AnalysisResult r{};
    r.freeze_analysis.has_analysis = true;
    r.freeze_analysis.state_id = "lock_owner_wait";
    r.main_thread_wait.kind = "engine_wait";
    r.main_thread_wait.waiting_module = L"SkyrimSE.exe";
    r.main_thread_lock_owner.tid = 51944;
    r.main_thread_lock_owner.confirmed = true;
    r.main_thread_lock_owner.lock_type = "critical_section";
    r.main_thread_lock_owner.thread_name = L"nv.sl.dlss_g.thread.dlssg";
    r.main_thread_lock_owner.top_module = L"nvwgf2umx.dll";
    const auto qa = BuildQuickAnswer(r, lang, FreezeContext(en));
    Require(qa.kind == "freeze" && qa.case_id == "gpu_driver", "the graphics driver side");
    Require(qa.likelihood == "medium" && qa.suspect_file == L"nvwgf2umx.dll", "confirmed owner, driver file");
    Require(Contains(qa.action, en ? L"graphics driver" : L"그래픽 드라이버"), "driver advice");
    Require(Contains(qa.action, en ? L"frame generation" : L"프레임 생성"), "test without frame generation");
    Require(!Contains(qa.headline, en ? L"Suspected mod" : L"의심되는 모드"), "a driver is not a mod");
    RequirePlainWords(qa, "gpu lock owner");

    // A lock owner in a plugin names that plugin.
    r.main_thread_lock_owner.top_module = L"SomePlugin.dll";
    r.main_thread_lock_owner.top_mod_name = L"Some Plugin";
    const auto plugin = BuildQuickAnswer(r, lang, FreezeContext(en));
    Require(plugin.case_id == "lock_owner" && plugin.suspect_mod == L"Some Plugin", "the owner's plugin");
  }
}

void TestCrashLoggerThreadDumpDeadlock()
{
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    AnalysisResult r{};
    r.freeze_analysis.has_analysis = true;
    r.freeze_analysis.state_id = "crash_logger_thread_dump_deadlock";
    r.main_thread_lock_owner.tid = 51944;
    r.main_thread_lock_owner.confirmed = true;
    r.main_thread_lock_owner.top_module = L"nvwgf2umx.dll";
    r.main_thread_lock_owner.blocked_without_wait_object = true;
    r.main_thread_lock_owner.context_switches_stable = true;
    r.main_thread_lock_owner.ip_state = "not_after_syscall";
    auto& deadlock = r.crash_logger_thread_dump_deadlock;
    deadlock.detected = true;
    deadlock.crash_logger_tid = 54776;
    deadlock.crash_logger_module = L"CrashLogger.dll";
    deadlock.thread_dump_truncated = true;
    deadlock.thread_dump_before_capture = true;
    deadlock.freeze_started_after_thread_dump = true;
    deadlock.thread_dump_stopped_before_capture = true;
    const auto qa = BuildQuickAnswer(r, lang, FreezeContext(en));
    Require(qa.case_id == "crash_logger_thread_dump" && qa.likelihood == "high", "Crash Logger's thread dump");
    Require(qa.suspect_file == L"CrashLogger.dll", "Crash Logger's file");
    Require(Contains(qa.action, en ? L"Don't press Crash Logger's thread dump hotkey" : L"스레드 덤프 단축키를 누르지 마세요"),
      "do not press the hotkey");
    Require(!Contains(qa.action, en ? L"graphics driver" : L"그래픽 드라이버"), "not the driver advice");
    RequirePlainWords(qa, "thread dump deadlock");
  }
}

// The 2026-10-22 freeze: the main thread waited in the engine for game thread
// 19588, which ran engine code five frames above SkyrimUpscaler.dll.
AnalysisResult EngineCallPathHang()
{
  AnalysisResult r{};
  r.dump_path = L"C:\\Logs\\SkyrimDiag_Hang_20261022_120435_196.dmp";
  r.freeze_analysis.has_analysis = true;
  r.freeze_analysis.state_id = "freeze_candidate";
  auto& wait = r.main_thread_wait;
  wait.kind = "engine_wait";
  wait.wait_class = "sync";
  wait.waiting_module = L"SkyrimSE.exe";
  wait.wait_api = L"KERNELBASE.dll!WaitForSingleObjectEx";
  wait.blocked_thread_tid = 19588;
  wait.blocked_module = L"SkyrimUpscaler.dll";
  wait.blocked_mod_name = L"houseCARL - SkyrimUpscalerAIOBuild22";
  wait.blocked_state = "engine_call_path";
  wait.blocked_top = L"SkyrimSE.exe";
  wait.blocked_path_modules = { L"SkyrimUpscaler.dll", L"ScrabsPapyrusExtender.dll" };
  SuspectItem s{};
  s.module_filename = L"hdtsmp64.dll";
  s.inferred_mod_name = L"FSMP";
  s.confidence_level = i18n::ConfidenceLevel::kLow;
  r.suspects.push_back(s);
  r.suspects_from_stackwalk = true;
  r.suspects_from_main_thread = true;
  return r;
}

void TestFreezeEngineCallPathNamesNoMod()
{
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = EngineCallPathHang();
    const auto qa = BuildQuickAnswer(r, lang, FreezeContext(en));
    Require(qa.kind == "freeze" && qa.likelihood == "none", "no lead from a plugin deep below engine code");
    Require(qa.suspect_mod.empty() && qa.suspect_file.empty(), "the upscaler is not named");

    // The same thread with the plugin on top is the lead, as before.
    r.main_thread_wait.blocked_state = "engine_call_from_plugin";
    r.main_thread_wait.blocked_path_modules.clear();
    const auto lead = BuildQuickAnswer(r, lang, FreezeContext(en));
    Require(lead.case_id == "blocked_thread" && lead.suspect_file == L"SkyrimUpscaler.dll", "a near plugin leads");
  }
}

void TestFreezeEngineSpinNan()
{
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = EngineCallPathHang();
    r.freeze_analysis.state_id = "engine_spin_nan";
    auto& spin = r.engine_spin;
    spin.loop_found = true;
    spin.nan_detected = true;
    spin.tid = 19588;
    spin.rip_module = L"SkyrimSE.exe";
    spin.rip_offset = 0xd17465;
    spin.compare = "comiss";
    spin.nan_register = "xmm0";
    spin.call_path_modules = r.main_thread_wait.blocked_path_modules;
    const auto qa = BuildQuickAnswer(r, lang, FreezeContext(en));
    Require(qa.kind == "freeze" && qa.case_id == "engine_spin_nan", "the engine NaN loop");
    Require(qa.likelihood == "medium", "medium");
    Require(qa.suspect_mod.empty() && qa.suspect_file.empty(), "no single mod");
    Require(Contains(qa.headline, en ? L"became NaN (not a number), so the engine kept repeating the same calculation"
                                     : L"게임 계산 값이 숫자가 아닌 값(NaN)이 되어, 엔진이 같은 계산을 끝없이 반복하다 멈췄습니다"),
      "plain words");
    Require(Contains(qa.action, en ? L"combat, animation, AI/pathing" : L"전투, 애니메이션, AI/길찾기"), "what to check");
    Require(Contains(qa.action, L"SkyrimUpscaler.dll, ScrabsPapyrusExtender.dll"), "the call path is said");
    Require(!qa.why_not_certain.empty(), "why not certain");
    RequirePlainWords(qa, "engine spin nan");

    // The summary sentence says the same, and names no mod as the cause.
    BuildEvidenceAndSummary(r, lang);
    Require(Contains(r.summary_sentence, L"SkyrimSE.exe+0xd17465") && Contains(r.summary_sentence, L"xmm0"),
      "the summary names the loop");
    Require(r.quick_answer.case_id == "engine_spin_nan", "built with the summary");
    bool mainThreadFirst = false;
    for (const auto& rec : r.recommendations) {
      if (rec.rfind(en ? L"[Main thread]" : L"[메인 스레드]", 0) == 0) {
        mainThreadFirst = Contains(rec, L"NaN");
        break;
      }
    }
    Require(mainThreadFirst, "the [Main thread] advice is the NaN loop");
  }
}

// The same loop at SkyrimSE.exe+0xd17465 on 1.6.1170 is a known AI pathing
// freeze (crash_signatures.json): the quick answer names its fix, or, with the
// fix already loaded, says to update it or tell its author.
void TestFreezeKnownAiPathingFix()
{
  skydiag::dump_tool::SignatureDatabase db;
  Require(db.LoadFromJson(std::filesystem::path(__FILE__).parent_path().parent_path() / "dump_tool" / "data" /
                          "crash_signatures.json"),
    "the bundled signatures load");
  for (const bool fixLoaded : { false, true }) {
    for (const auto lang : kLanguages) {
      const bool en = lang == i18n::Language::kEnglish;
      auto r = EngineCallPathHang();
      r.game_version = "1.6.1170.0";
      r.freeze_analysis.state_id = "engine_spin_nan";
      auto& spin = r.engine_spin;
      spin.loop_found = true;
      spin.nan_detected = true;
      spin.tid = 19588;
      spin.rip_module = L"SkyrimSE.exe";
      spin.rip_offset = 0xd17465;
      spin.compare = "comiss";
      spin.nan_register = "xmm0";
      spin.call_path_modules = r.main_thread_wait.blocked_path_modules;
      skydiag::dump_tool::SignatureMatchInput input{};
      input.game_version = r.game_version;
      input.freeze_state = r.freeze_analysis.state_id;
      input.freeze_loop_module = spin.rip_module;
      input.freeze_loop_offset = spin.rip_offset;
      input.loaded_modules = { L"SkyrimSE.exe", L"SkyrimUpscaler.dll" };
      if (fixLoaded) {
        input.loaded_modules.push_back(L"TMS_AIPathingFreezeFix.dll");
      }
      r.signature_match = db.Match(input, !en);
      Require(r.signature_match && r.signature_match->id == "AI_PATHING_FREEZE_1_6_1170", "the known freeze");

      const auto qa = BuildQuickAnswer(r, lang, FreezeContext(en));
      Require(qa.kind == "freeze" && qa.case_id == "known_issue", "still a freeze, a known one");
      Require(qa.likelihood == "medium" && qa.likelihood_text == (en ? L"possible" : L"가능성 중간"), "medium");
      Require(qa.headline == (en ? L"The game froze. It matches a known AI pathing freeze."
                                 : L"게임이 멈췄습니다(프리징). 알려진 AI 길찾기 프리징과 일치합니다."),
        "the headline names the known freeze");
      Require(qa.why_not_certain == (en
          ? L"Only one player has confirmed this fix so far, and other causes can produce the same loop."
          : L"이 해결 방법은 사용자 한 명이 확인했을 뿐이고, 다른 원인으로도 같은 반복이 생길 수 있습니다."),
        "other causes can make the same loop");
      if (!fixLoaded) {
        Require(qa.action == (en ? L"Install \"AI Pathing Freeze Fix\" (Nexus mod 190049) with your mod manager, then play again."
                                 : L"모드 관리자로 \"AI Pathing Freeze Fix\"(Nexus 모드 190049)를 설치한 뒤 다시 플레이해 보세요."),
          "install the fix");
      } else {
        Require(qa.action == (en ? L"AI Pathing Freeze Fix is already loaded, but this freeze still happened: check for an "
                                   L"update, or tell its author and share this report."
                                 : L"AI Pathing Freeze Fix가 이미 로드되어 있는데도 이 프리징이 일어났으니, 새 버전이 있는지 "
                                   L"확인하거나 이 리포트와 함께 제작자에게 알려 주세요."),
          "already loaded: update or report");
      }
      Require(!Contains(qa.headline, L"SkyrimUpscaler") && !Contains(qa.action, L"SkyrimUpscaler"), "no other mod named");
      RequirePlainWords(qa, "known ai pathing freeze");

      // The summary and the first recommendation say the same.
      BuildEvidenceAndSummary(r, lang);
      Require(r.quick_answer.case_id == "known_issue", "built with the summary");
      Require(Contains(r.summary_sentence, L"[AI_PATHING_FREEZE_1_6_1170]") &&
                Contains(r.summary_sentence, L"SkyrimSE.exe+0xd17465"),
        "the summary names the known freeze and the loop");
      Require(Contains(r.summary_sentence, en ? L"was already loaded" : L"이미 로드되어 있었습니다") == fixLoaded,
        "the summary says when the fix was loaded");
      Require(!r.recommendations.empty() &&
                r.recommendations[0].rfind(en ? L"[Known cause]" : L"[알려진 원인]", 0) == 0 &&
                Contains(r.recommendations[0], L"https://www.nexusmods.com/skyrimspecialedition/mods/190049"),
        "the fix with its Nexus link comes first");
    }
  }
}

AnalysisResult ModalHang(bool callerResolved)
{
  AnalysisResult r{};
  r.dump_path = L"C:\\Logs\\SkyrimDiag_Hang_20261005_155303_207.dmp";
  auto& modal = r.modal_dialog_wait;
  modal.detected = true;
  modal.window_evidence = true;
  modal.stack_evidence = true;
  modal.dialog_title = L"SmoothCam.dll";
  modal.address_library_issue = "plugin_incompatible";
  if (callerResolved) {
    modal.caller_module_filename = L"SmoothCam.dll";
    modal.caller_inferred_mod_name = L"SmoothCam";
    modal.caller_kind = "plugin";
  }
  r.freeze_analysis.has_analysis = true;
  r.freeze_analysis.state_id = "modal_dialog_wait";
  return r;
}

void TestModalDialog()
{
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    const auto unresolved = BuildQuickAnswer(ModalHang(false), lang, FreezeContext(en));
    Require(unresolved.case_id == "modal_dialog" && unresolved.likelihood == "none", "the dialog, no opener");
    Require(Contains(unresolved.headline, L"\"SmoothCam.dll\""), "the dialog's title");
    Require(unresolved.action.rfind(en ? L"Read the window the game showed" : L"게임이 띄운 오류 창", 0) == 0,
      "read the dialog first");
    Require(Contains(unresolved.action, en ? L"game version" : L"게임 버전"), "what the dialog says");
    RequirePlainWords(unresolved, "modal dialog");

    const auto resolved = BuildQuickAnswer(ModalHang(true), lang, FreezeContext(en));
    Require(resolved.suspect_mod == L"SmoothCam" && resolved.likelihood == "high", "the plugin that opened it");
  }
}

void TestLoadingFreezeWithoutSuspect()
{
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    AnalysisResult r{};
    r.freeze_analysis.has_analysis = true;
    r.freeze_analysis.state_id = "loader_stall_likely";
    r.freeze_analysis.blackbox_context.loading_window = true;
    const auto qa = BuildQuickAnswer(r, lang, FreezeContext(en));
    Require(qa.kind == "loading" && qa.case_id == "none", "a loading freeze with no suspect");
    Require(Contains(qa.headline, en ? L"loading screen" : L"로딩 화면"), "says loading");
    Require(Contains(qa.action, en ? L"Share this report" : L"리포트를 함께 공유"), "share the report");
    RequirePlainWords(qa, "loading");
  }
}

void TestFreezeStackScanIsWeak()
{
  AnalysisResult r{};
  r.freeze_analysis.has_analysis = true;
  r.freeze_analysis.state_id = "freeze_candidate";
  SuspectItem s{};
  s.module_filename = L"ExampleMod.dll";
  s.module_path = L"G:\\MO2\\mods\\Example Mod\\SKSE\\Plugins\\ExampleMod.dll";
  s.inferred_mod_name = L"Example Mod";
  s.confidence_level = i18n::ConfidenceLevel::kMedium;
  r.suspects.push_back(s);
  r.suspects_from_stackwalk = false;
  const auto qa = BuildQuickAnswer(r, i18n::Language::kEnglish, FreezeContext(true));
  Require(qa.kind == "freeze" && qa.suspect_mod == L"Example Mod", "the pointer-scan suspect");
  Require(qa.likelihood == "low", "a pointer scan is weak whatever the suspect says");
  Require(Contains(qa.why_not_certain, L"only seen nearby"), "why it is weak");
}

void TestSnapshotsAndCleanExit()
{
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    AnalysisResult r{};
    auto ctx = FreezeContext(en);
    ctx.isHangLike = false;
    ctx.isSnapshotLike = true;
    ctx.isManualCapture = true;
    const auto manual = BuildQuickAnswer(r, lang, ctx);
    Require(manual.kind == "snapshot" && manual.case_id == "manual_snapshot", "a manual snapshot");
    Require(Contains(manual.action, en ? L"snapshot you took" : L"직접 찍은 스냅샷"), "you took it");
    Require(Contains(manual.action, en ? L"does not mean anything is wrong" : L"문제가 있다는 뜻은 아닙니다"),
      "nothing is wrong by itself");
    Require(manual.likelihood == "none" && manual.suspect_mod.empty(), "no suspect");

    ctx.isSnapshotLike = false;
    ctx.isManualCapture = false;
    ctx.isFilteredCleanExit = true;
    const auto clean = BuildQuickAnswer(r, lang, ctx);
    Require(clean.kind == "clean_exit" && clean.likelihood == "none", "a clean exit names nothing");
  }
}

// End to end: the evidence builder fills it, the summary JSON carries it and
// the report starts with it, before the dump path and the metadata.
void TestSummaryAndReportStartWithIt()
{
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = CrashIn(L"ExampleMod.dll", L"Example Mod");
    r.dump_path = L"C:\\Logs\\SkyrimDiag_Crash_20261021_053954_072.dmp";
    BuildEvidenceAndSummary(r, lang);
    Require(!r.quick_answer.headline.empty() && r.quick_answer.kind == "crash", "built with the summary");
    const auto summaryBefore = r.summary_sentence;

    const auto summary = skydiag::dump_tool::BuildSummaryJson(r, std::filesystem::temp_directory_path(), L"stem", false);
    Require(summary.contains("quick_answer") && summary["quick_answer"].is_object(), "summary JSON field");
    const auto& qa = summary["quick_answer"];
    for (const char* key : { "kind", "case", "suspect_mod", "suspect_file", "likelihood", "likelihood_text", "headline",
                             "action", "why_not_certain" }) {
      Require(qa.contains(key) && qa[key].is_string(), key);
    }
    Require(qa["suspect_mod"].get<std::string>() == "Example Mod", "the mod in the JSON");
    Require(summary["summary_sentence"].get<std::string>() == WideToUtf8(summaryBefore), "summary sentence unchanged");

    const auto report = skydiag::dump_tool::BuildReportText(r, summary, false);
    std::vector<std::string> lines;
    std::istringstream in(report);
    for (std::string line; std::getline(in, line);) {
      lines.push_back(line);
    }
    Require(lines.size() > 8, "report lines");
    Require(lines[0] == (en ? "SkyrimDiag Report" : "SkyrimDiag 리포트"), "title first");
    Require(lines[1] == (en ? "Quick answer" : "빠른 답"), "the quick answer right under the title");
    Require(lines[2] == "  " + WideToUtf8(skydiag::dump_tool::QuickAnswerHeadlineLine(r.quick_answer)), "headline");
    Require(lines[3].rfind(en ? "  What to try: " : "  해볼 일: ", 0) == 0, "the action");
    Require(lines[4].rfind(en ? "  Why not certain: " : "  확실하지 않은 이유: ", 0) == 0, "the doubt line");
    Require(lines[5].empty() && lines[6] == (en ? "Details" : "자세한 분석"), "then the details divider");
    Require(lines[7].rfind(en ? "Dump: " : "덤프: ", 0) == 0, "the dump path after the quick answer");
    Require(lines[8].rfind(en ? "Summary: " : "결론: ", 0) == 0, "the summary sentence stays next");
  }

  // A manual capture found by its file name, through the whole builder.
  AnalysisResult manual{};
  manual.dump_path = L"C:\\Logs\\SkyrimDiag_Manual_20261018_190409_880.dmp";
  BuildEvidenceAndSummary(manual, i18n::Language::kEnglish);
  Require(manual.quick_answer.case_id == "manual_snapshot", "manual snapshot end to end");
}

}  // namespace

int main()
{
  try {
    TestCrashWithHighCandidate();
    TestCrashWithMediumCandidateElsewhere();
    TestCrashWithLowFaultModuleOnly();
    TestConflictingCandidatesNameBoth();
    TestCrashWithNoSuspect();
    TestExternalSoftware();
    TestKnownIssueUsesItsFix();
    TestHookStubRaceIsLikely();
    TestFreezeLockOwnerInGpuDriver();
    TestCrashLoggerThreadDumpDeadlock();
    TestFreezeEngineCallPathNamesNoMod();
    TestFreezeEngineSpinNan();
    TestFreezeKnownAiPathingFix();
    TestModalDialog();
    TestLoadingFreezeWithoutSuspect();
    TestFreezeStackScanIsWeak();
    TestSnapshotsAndCleanExit();
    TestSummaryAndReportStartWithIt();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
  std::printf("quick_answer_tests: all passed\n");
  return 0;
}
