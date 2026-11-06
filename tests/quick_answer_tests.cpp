// The quick answer (QuickAnswer.h): a Nexus user found the report "too
// complicated ... we are just playing games" (2026-10-21). Players get what
// happened, the suspect, one thing to try and why it is not certain, in plain
// words, before any technical detail; the analysis itself is unchanged.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "Analyzer.h"
#include "AnalyzerInternals.h"
#include "EvidenceBuilder.h"
#include "EvidenceBuilderPrivate.h"
#include "GpuDriverLog.h"
#include "LoadedSaves.h"
#include "ModNames.h"
#include "NextAction.h"
#include "OutputWriterPipeline.h"
#include "SignatureDatabase.h"
#include "SkyrimDiagSaveLoad.h"
#include "SkyrimDiagShared.h"
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

// ADR-0021: two worker threads deadlocked on the engine's spin locks (the
// FreezeLogger capture's shape). No mod is named; the summary JSON and the
// report carry the waits and the cycle; with the 1.5.97 pattern known, its
// fix leads.
AnalysisResult EngineLockDeadlockHang()
{
  auto r = EngineCallPathHang();
  r.main_thread_wait.blocked_thread_tid = 0;
  r.main_thread_wait.blocked_module.clear();
  r.main_thread_wait.blocked_state.clear();
  r.main_thread_wait.blocked_path_modules.clear();
  r.freeze_analysis.state_id = "engine_lock_deadlock";
  r.freeze_analysis.confidence_level = i18n::ConfidenceLevel::kMedium;
  auto& deadlock = r.engine_lock_deadlock;
  deadlock.detected = true;
  deadlock.module = L"SkyrimSE.exe";
  deadlock.cycle = {
    { 5096u, 0x2f3b8e8ull, "spin", 18456u },
    { 18456u, 0x2eff8e0ull, "spin", 5096u },
  };
  skydiag::dump_tool::GameLockWaitThread a{};
  a.tid = 5096;
  a.wait_module = L"SkyrimSE.exe";
  a.wait_offset = 0x132c5a;
  a.frames = { { L"SkyrimSE.exe", 0x132c5a }, { L"SkyrimSE.exe", 0x6d9750 } };
  a.candidates = { { L"RSP+88", 0x2f3b8e8ull, "spin", 18456u, 1u, true } };
  auto b = a;
  b.tid = 18456;
  b.frames = { { L"SkyrimSE.exe", 0x132c5a }, { L"SkyrimSE.exe", 0x296c3d } };
  b.candidates = { { L"RSP+40", 0x2eff8e0ull, "spin", 5096u, 1u, true } };
  skydiag::dump_tool::GameLockWaitThread quiet{};  // waiting, but nothing held: not listed
  quiet.tid = 777;
  quiet.wait_module = L"SkyrimSE.exe";
  quiet.wait_offset = 0x132c5a;
  r.game_lock_waits = { a, b, quiet };
  return r;
}

void TestFreezeEngineLockDeadlock()
{
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = EngineLockDeadlockHang();
    const auto qa = BuildQuickAnswer(r, lang, FreezeContext(en));
    Require(qa.kind == "freeze" && qa.case_id == "engine_lock_deadlock", "the engine lock deadlock");
    Require(qa.likelihood == "medium", "medium");
    Require(qa.suspect_mod.empty() && qa.suspect_file.empty(), "no single mod");
    Require(Contains(qa.headline, en ? L"waiting on the game engine's own locks" : L"게임 엔진 자체 잠금을 기다리다"),
      "plain words");
    Require(!Contains(qa.headline, L"hdtsmp") && !Contains(qa.action, L"hdtsmp"), "no stack mod named");
    Require(!qa.why_not_certain.empty(), "why not certain");
    RequirePlainWords(qa, "engine lock deadlock");

    BuildEvidenceAndSummary(r, lang);
    Require(Contains(r.summary_sentence, L"SkyrimSE.exe+0x2f3b8e8") && Contains(r.summary_sentence, L"5096, 18456"),
      "the summary names the threads and locks");
    Require(r.quick_answer.case_id == "engine_lock_deadlock", "built with the summary");
    bool mainThread = false;
    for (const auto& rec : r.recommendations) {
      if (rec.rfind(en ? L"[Main thread]" : L"[메인 스레드]", 0) == 0) {
        mainThread = Contains(rec, L"5096, 18456");
        break;
      }
    }
    Require(mainThread, "the [Main thread] advice is the deadlock");
    for (const auto& rec : r.recommendations) {
      Require(!Contains(rec, L"FSMP") && !Contains(rec, L"hdtsmp64.dll' "), "no stack mod advice");
    }
    bool evidence = false;
    for (const auto& e : r.evidence) {
      evidence = evidence || (Contains(e.details, L"waits at SkyrimSE.exe+0x132c5a") ||
                              Contains(e.details, L"대기 위치: SkyrimSE.exe+0x132c5a"));
    }
    Require(evidence, "the evidence lists the threads' wait sites");

    const auto summary = skydiag::dump_tool::BuildSummaryJson(r, std::filesystem::temp_directory_path(), L"stem", false);
    const auto& freeze = summary["freeze_analysis"];
    Require(freeze.contains("game_lock_waits") && freeze["game_lock_waits"].size() == 2u, "two waits listed");
    const auto& w0 = freeze["game_lock_waits"][0];
    Require(w0["tid"] == 5096 && w0["wait_site"] == "SkyrimSE.exe+0x132c5a" && w0["frames"].size() == 2u,
      "wait site and frames");
    const auto& c0 = w0["candidate_locks"][0];
    Require(c0["rva"] == "0x2f3b8e8" && c0["kind"] == "spin" && c0["owner_tid"] == 18456 && c0["count"] == 1 &&
              c0["owner_waiting"] == true && c0["location"] == "RSP+88",
      "the candidate lock");
    const auto& deadlock = freeze["engine_lock_deadlock"];
    Require(deadlock["kind"] == "cycle" && deadlock["threads"] == nlohmann::json::array({ 5096, 18456 }) &&
              deadlock["cycle"].size() == 2u && deadlock["cycle"][1]["lock"] == "SkyrimSE.exe+0x2eff8e0" &&
              deadlock["known_pattern"] == "",
      "the deadlock in the JSON");
    const auto report = skydiag::dump_tool::BuildReportText(r, summary, false);
    Require(report.find("  game_lock_wait tid=5096 main_thread=0 site=SkyrimSE.exe+0x132c5a "
                        "lock=RSP+88:0x2f3b8e8:spin:owner=18456:count=1:owner_waiting") != std::string::npos,
      "the report line");
    Require(report.find("engine_lock_deadlock threads=5096, 18456") != std::string::npos, "the report deadlock line");
    Require(report.find("tid=777") == std::string::npos, "a wait with nothing held is not listed");
  }

  // A known pattern of one thread waiting on its own lock: no "each other".
  {
    auto r = EngineLockDeadlockHang();
    r.engine_lock_deadlock.cycle.clear();
    r.engine_lock_deadlock.known_pattern = "CELL_LOAD_READ_WRITE_SELF_DEADLOCK_1_5_97";
    r.engine_lock_deadlock.pattern_tids = { 42u };
    BuildEvidenceAndSummary(r, i18n::Language::kEnglish);
    Require(Contains(r.summary_sentence, L"game threads 42 could not go on") &&
              Contains(r.summary_sentence, L"(CELL_LOAD_READ_WRITE_SELF_DEADLOCK_1_5_97)") &&
              !Contains(r.summary_sentence, L"another of them"),
      "the known pattern in the summary");
    const auto summary = skydiag::dump_tool::BuildSummaryJson(r, std::filesystem::temp_directory_path(), L"stem", false);
    Require(summary["freeze_analysis"]["engine_lock_deadlock"]["kind"] == "known_pattern" &&
              summary["freeze_analysis"]["engine_lock_deadlock"]["threads"] == nlohmann::json::array({ 42 }),
      "known_pattern in the JSON");
  }

  // Without the lock memory (dumps before this): no new fields.
  AnalysisResult old = EngineCallPathHang();
  BuildEvidenceAndSummary(old, i18n::Language::kEnglish);
  const auto oldSummary =
    skydiag::dump_tool::BuildSummaryJson(old, std::filesystem::temp_directory_path(), L"stem", false);
  Require(!oldSummary["freeze_analysis"].contains("game_lock_waits") &&
            !oldSummary["freeze_analysis"].contains("engine_lock_deadlock"),
    "additive only");

  // The known 1.5.97 pattern: its fix comes first.
  skydiag::dump_tool::SignatureDatabase db;
  Require(db.LoadFromJson(std::filesystem::path(__FILE__).parent_path().parent_path() / "dump_tool" / "data" /
                          "crash_signatures.json"),
    "the bundled signatures load");
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = EngineLockDeadlockHang();
    r.game_version = "1.5.97.0";
    skydiag::dump_tool::SignatureMatchInput input{};
    input.game_version = r.game_version;
    input.freeze_state = r.freeze_analysis.state_id;
    input.game_lock_threads = {
      { 5096u, { { L"SkyrimSE.exe", 0x132c5a }, { L"SkyrimSE.exe", 0x6d9750 }, { L"SkyrimSE.exe", 0x297ddb },
                 { L"SkyrimSE.exe", 0x2971a4 } } },
      { 18456u, { { L"SkyrimSE.exe", 0x132c5a }, { L"SkyrimSE.exe", 0x296c3d }, { L"SkyrimSE.exe", 0x6025fb },
                  { L"SkyrimSE.exe", 0x6ef480 } } },
    };
    input.game_lock_cycle_tids = { 5096u, 18456u };
    input.read_game_lock = [](std::uint64_t rva, std::uint32_t& w0, std::uint32_t& w1) {
      w1 = 1u;
      w0 = rva == 0x2eff8e0ull ? 5096u : (rva == 0x2f3b8e8ull ? 18456u : 0u);
      return w0 != 0u;
    };
    r.signature_match = db.Match(input, !en);
    Require(r.signature_match && r.signature_match->id == "WORKER_SPINLOCK_ABBA_DEADLOCK_1_5_97", "the known deadlock");
    BuildEvidenceAndSummary(r, lang);
    Require(r.quick_answer.case_id == "known_issue" && r.quick_answer.likelihood == "high", "a known one");
    Require(Contains(r.quick_answer.action, L"WorkerSpinLockFix"), "install the fix");
    Require(Contains(r.summary_sentence, L"[WORKER_SPINLOCK_ABBA_DEADLOCK_1_5_97]"), "the summary names it");
    Require(!r.recommendations.empty() && r.recommendations[0].rfind(en ? L"[Known cause]" : L"[알려진 원인]", 0) == 0 &&
              Contains(r.recommendations[0], L"[phase4_defer]"),
      "the fix and its setting come first");
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

skydiag::dump_tool::DumpObject FormObject(
  std::wstring location,
  std::wstring type,
  std::wstring name,
  std::uint32_t formId,
  std::vector<std::wstring> files)
{
  skydiag::dump_tool::DumpObject o{};
  o.location = std::move(location);
  o.type_name = std::move(type);
  o.name = std::move(name);
  o.is_form = true;
  o.form_id = formId;
  o.source_files = std::move(files);
  return o;
}

// ADR-0010 addendum: the objects the stuck thread held are a clue after the
// doubt; the lead, its likelihood and the candidates stay as they were.
void TestFreezeObjectClue()
{
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = EngineCallPathHang();
    r.freeze_analysis.state_id = "engine_spin_nan";
    r.engine_spin.loop_found = true;
    r.engine_spin.nan_detected = true;
    r.engine_spin.tid = 19588;
    r.engine_spin.rip_module = L"SkyrimSE.exe";
    r.engine_spin.rip_offset = 0xd17465;
    r.engine_spin.compare = "comiss";
    r.engine_spin.nan_register = "xmm0";
    const auto before = BuildQuickAnswer(r, lang, FreezeContext(en));

    // Objects on another thread say nothing about the loop.
    skydiag::dump_tool::FreezeThreadObjects mainThread{};
    mainThread.tid = 7;
    mainThread.roles = { "main" };
    mainThread.objects = { FormObject(L"RCX", L"PlayerCharacter", L"Prisoner", 0x14u, { L"Skyrim.esm" }) };
    r.freeze_thread_objects.push_back(mainThread);
    Require(BuildQuickAnswer(r, lang, FreezeContext(en)).why_not_certain == before.why_not_certain,
      "only the verdict thread's objects count");

    skydiag::dump_tool::FreezeThreadObjects spin{};
    spin.tid = 19588;
    spin.roles = { "blocked_thread", "engine_spin" };
    skydiag::dump_tool::DumpObject pathing{};
    pathing.location = L"RBX";
    pathing.type_name = L"BSPathingRequest";
    pathing.module = L"SkyrimSE.exe";
    spin.objects = { pathing,
      FormObject(L"R14", L"Character", L"Lydia", 0xFF0036F8u, {}),
      FormObject(L"RSP+40", L"TESObjectCELL", L"", 0x0000A2C4u, { L"Skyrim.esm" }) };
    spin.objects[1].has_base_object = true;
    spin.objects[1].base_type_name = L"TESNPC";
    spin.objects[1].base_form_id = 0x000A2C94u;
    spin.objects[1].base_source_files = { L"Skyrim.esm", L"MyFollower.esp" };
    r.freeze_thread_objects.push_back(spin);

    const auto qa = BuildQuickAnswer(r, lang, FreezeContext(en));
    const std::wstring clue = (en ? L"The stuck calculation was handling: " : L"멈춘 계산이 다루던 대상: ") +
      std::wstring(L"Character \"Lydia\" (FormID 0xFF0036F8, base TESNPC 0x000A2C94 Skyrim.esm -> MyFollower.esp).");
    Require(qa.why_not_certain == before.why_not_certain + L" " + clue, "the first form, after the doubt");
    Require(qa.case_id == before.case_id && qa.likelihood == before.likelihood && qa.headline == before.headline &&
              qa.action == before.action && qa.suspect_mod.empty() && qa.suspect_file.empty(),
      "the answer is otherwise unchanged and names no mod");

    BuildEvidenceAndSummary(r, lang);
    bool evidence = false;
    for (const auto& e : r.evidence) {
      if (Contains(e.title, en ? L"stuck thread's registers" : L"멈춘 스레드의 레지스터")) {
        evidence = e.confidence_level == i18n::ConfidenceLevel::kLow &&
          Contains(e.details, L"R14: Character \"Lydia\" [0xFF0036F8] (base TESNPC [0x000A2C94] Skyrim.esm -> MyFollower.esp)");
      }
    }
    Require(evidence, "a low evidence item lists the thread's objects");
    for (const auto& candidate : r.actionable_candidates) {
      Require(candidate.module_filename != L"MyFollower.esp", "a plugin that defines a form is not a candidate");
    }
    const auto summary = skydiag::dump_tool::BuildSummaryJson(r, std::filesystem::path(L"C:\\Logs"), L"stem", true);
    const auto& verdict = summary["freeze_analysis"]["verdict_thread_object"];
    Require(verdict["tid"] == 19588u && verdict["type"] == "Character" && verdict["form_id"] == 0xFF0036F8u,
      "the summary names the verdict thread's form");
    Require(summary["freeze_analysis"]["thread_objects"].size() == 2u, "every thread with objects");

    // The lock owner of a lock-owner wait is the thread the verdict is about.
    AnalysisResult owner{};
    owner.freeze_analysis.has_analysis = true;
    owner.freeze_analysis.state_id = "lock_owner_wait";
    owner.main_thread_wait.kind = "engine_wait";
    owner.main_thread_wait.waiting_module = L"SkyrimSE.exe";
    owner.main_thread_lock_owner.tid = 51944;
    owner.main_thread_lock_owner.confirmed = true;
    owner.main_thread_lock_owner.top_module = L"nvwgf2umx.dll";
    skydiag::dump_tool::FreezeThreadObjects held{};
    held.tid = 51944;
    held.roles = { "lock_owner" };
    held.objects = { FormObject(L"RDI", L"TESObjectREFR", L"", 0x0001A332u, { L"Skyrim.esm" }) };
    owner.freeze_thread_objects.push_back(held);
    const auto gpu = BuildQuickAnswer(owner, lang, FreezeContext(en));
    Require(gpu.case_id == "gpu_driver" &&
              Contains(gpu.why_not_certain, (en ? L"The stuck thread was handling: " : L"멈춘 스레드가 다루던 대상: ") +
                std::wstring(L"TESObjectREFR (FormID 0x0001A332, Skyrim.esm).")),
      "the lock owner's form");
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

// No opener on the stack, but the dialog's title is the plugin's file name
// (the 2026-10-05 SmoothCam hang): the quick answer, the summary, the next
// action and the recommendations name the same mod.
void TestModalDialogNamedModule()
{
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = ModalHang(false);
    r.game_version = "1.6.1170.0";
    auto& modal = r.modal_dialog_wait;
    modal.stack_evidence = false;
    modal.dialog_text = L"REL/ID.h(223): Unsupported address library format: 2 / This means this script extender "
                        L"plugin is incompatible with the address library available for this version of the game";
    modal.named_module_filename = L"SmoothCam.dll";
    modal.named_mod_name = L"SmoothCam";
    modal.named_module_source = "title";
    modal.named_module_level = i18n::ConfidenceLevel::kHigh;
    modal.named_module_version_issue = true;

    const auto qa = BuildQuickAnswer(r, lang, FreezeContext(en));
    Require(qa.case_id == "modal_dialog" && qa.likelihood == "high", "the mod the dialog names, high");
    Require(qa.suspect_mod == L"SmoothCam" && qa.suspect_file == L"SmoothCam.dll", "its mod and file");
    Require(Contains(qa.headline, en ? L"Suspected mod: SmoothCam (SmoothCam.dll)"
                                     : L"의심되는 모드: SmoothCam (SmoothCam.dll)"),
      "the headline names it");
    Require(qa.action == (en ? L"Update SmoothCam to a version made for your game version, or disable it (see what "
                               L"the error window says)."
                             : L"SmoothCam을(를) 지금 게임 버전에 맞는 버전으로 업데이트하거나 끄세요(오류 창에 적힌 내용 참고)."),
      "update or disable it");
    Require(qa.why_not_certain.empty(), "high needs no doubt line");
    RequirePlainWords(qa, "modal dialog named module");

    BuildEvidenceAndSummary(r, lang);
    Require(r.quick_answer.suspect_mod == L"SmoothCam", "built with the summary");
    Require(Contains(r.summary_sentence, en ? L"suspected mod: SmoothCam (SmoothCam.dll) — likely"
                                            : L"의심되는 모드: SmoothCam (SmoothCam.dll) — 가능성 높음"),
      "the summary names it with the same likelihood");
    const auto next = skydiag::dump_tool::SelectNextActionIndex(r.recommendations);
    Require(next < r.recommendations.size() && Contains(r.recommendations[next], L"SmoothCam (SmoothCam.dll)") &&
              Contains(r.recommendations[next], en ? L"Update" : L"업데이트하거나 끄세요"),
      "the next action updates or disables it");
    bool textKept = false;
    for (const auto& rec : r.recommendations) {
      textKept = textKept || Contains(rec, en ? L"Captured dialog text" : L"캡처된 대화상자 내용");
    }
    Require(textKept, "the dialog text is still pointed at");

    // Named only in the text, with nothing about the game version: medium.
    modal.named_module_source = "text";
    modal.named_module_level = i18n::ConfidenceLevel::kMedium;
    modal.named_module_version_issue = false;
    modal.address_library_issue.clear();
    const auto medium = BuildQuickAnswer(r, lang, FreezeContext(en));
    Require(medium.likelihood == "medium" && medium.suspect_mod == L"SmoothCam", "medium from the text");
    Require(!medium.why_not_certain.empty(), "says why it is not certain");
    Require(Contains(medium.action, en ? L"update or disable SmoothCam" : L"SmoothCam을(를) 업데이트하거나 끄세요"),
      "follow the window or update or disable it");
    RequirePlainWords(medium, "modal dialog named in text");
  }
}

// Several loaded mod DLLs, or only DLLs the game did not load: no mod is
// picked; the names are listed in plain words.
void TestModalDialogNamesNoSingleModule()
{
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    auto several = ModalHang(false);
    several.modal_dialog_wait.dialog_title = L"Error";
    several.modal_dialog_wait.named_loaded_modules = { L"FooPlugin.dll", L"BarPlugin.dll" };
    const auto qa = BuildQuickAnswer(several, lang, FreezeContext(en));
    Require(qa.likelihood == "none" && qa.suspect_mod.empty(), "no single mod");
    Require(Contains(qa.headline, L"FooPlugin.dll, BarPlugin.dll"), "both names listed");
    RequirePlainWords(qa, "several named modules");

    auto unloaded = ModalHang(false);
    unloaded.modal_dialog_wait.dialog_title = L"Error";
    unloaded.modal_dialog_wait.named_unloaded_modules = { L"MissingDep.dll" };
    const auto missing = BuildQuickAnswer(unloaded, lang, FreezeContext(en));
    Require(missing.likelihood == "none", "a file the game did not load is no suspect");
    Require(Contains(missing.headline, en ? L"MissingDep.dll, which the game did not load"
                                          : L"MissingDep.dll은(는) 게임에 로드되지 않은 파일"),
      "says it was not loaded");
    BuildEvidenceAndSummary(unloaded, lang);
    Require(Contains(unloaded.summary_sentence, L"MissingDep.dll"), "the summary lists it too");
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
    // The viewer analyzes a summary again when its analyzer was older.
    Require(summary.contains("analyzer_version") && summary["analyzer_version"].is_string() &&
              summary["analyzer_version"].get<std::string>().find('.') != std::string::npos,
      "the analyzer's product version");
    const auto& qa = summary["quick_answer"];
    for (const char* key : { "kind", "case", "suspect_mod", "suspect_file", "likelihood", "likelihood_text", "headline",
                             "action", "why_not_certain", "how_it_crashed" }) {
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

// ADR-0019, in the shape of the 2026-09-18 crash: "mov rbx, [rax+0x10]" in
// SkyrimUpscaler.dll with rax = 0 (read at 0x10). The evidence, the quick
// answer's how-it-crashed line, the summary JSON and the report say what it
// did; the lead, the doubt line, the candidates and the summary sentence do
// not change.
void TestFaultInstructionClue()
{
  using skydiag::dump_tool::DescribeFaultInstruction;
  using skydiag::dump_tool::DescribeFaultInstructionPlain;
  using skydiag::dump_tool::ExplainFaultInstruction;
  using skydiag::dump_tool::FaultRecord;
  const std::vector<std::uint8_t> code = { 0x48, 0x8B, 0x58, 0x10, 0x48, 0x8B, 0x35 };
  const auto crash = []() {
    auto r = CrashIn(L"SkyrimUpscaler.dll", L"SkyrimUpscalerAIOBuild16-Hotfix1");
    r.exc_addr = 0x7FFA3CB46144ull;
    r.exc_info = { 0, 0x10 };
    r.fault_module_plus_offset = L"SkyrimUpscaler.dll+0x156144";
    return r;
  };
  FaultRecord fault{};
  fault.exception_address = 0x7FFA3CB46144ull;
  fault.access_kind = 0;
  fault.address = 0x10;
  fault.has_registers = true;
  fault.registers[3] = 0x7FFA3CD29780ull;  // rbx, the destination
  const auto decoded = ExplainFaultInstruction(fault, code.data(), code.size());
  Require(decoded.verdict == "null_pointer", "decoded");

  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = crash();
    const auto before = BuildQuickAnswer(r, lang, CrashContext(en, r.fault_module_filename));
    r.fault_instruction = decoded;
    const auto qa = BuildQuickAnswer(r, lang, CrashContext(en, r.fault_module_filename));
    Require(before.how_it_crashed.empty() && qa.how_it_crashed == DescribeFaultInstructionPlain(decoded, en) &&
              !qa.how_it_crashed.empty(),
      "the plain clue on its own line");
    Require(qa.case_id == before.case_id && qa.likelihood == before.likelihood && qa.headline == before.headline &&
              qa.action == before.action && qa.suspect_mod == before.suspect_mod &&
              qa.why_not_certain == before.why_not_certain,
      "the answer is otherwise unchanged");
    RequirePlainWords(qa, "fault instruction clue");

    // A likely lead has no doubt line, and still says how it crashed.
    auto likely = r;
    likely.actionable_candidates.push_back(Candidate("cross_validated", i18n::ConfidenceLevel::kHigh,
      L"SkyrimUpscaler.dll", L"SkyrimUpscalerAIOBuild16-Hotfix1", { "actionable_stack", "crash_logger_frame" }));
    const auto likelyQa = BuildQuickAnswer(likely, lang, CrashContext(en, r.fault_module_filename));
    Require(likelyQa.why_not_certain.empty() && likelyQa.how_it_crashed == qa.how_it_crashed,
      "how it crashed without a doubt line");

    auto plain = crash();
    BuildEvidenceAndSummary(plain, lang);
    BuildEvidenceAndSummary(r, lang);
    Require(r.summary_sentence == plain.summary_sentence, "summary sentence unchanged");
    Require(r.actionable_candidates.size() == plain.actionable_candidates.size(), "candidates unchanged");
    Require(r.evidence.size() == plain.evidence.size() + 1u, "one evidence item more");
    const std::wstring sentence = DescribeFaultInstruction(decoded, L"SkyrimUpscaler.dll+0x156144", en);
    Require(Contains(sentence, en ? L"(mov) tried to read 8 bytes from [rax+0x10], and rax was 0"
                                  : L"(mov)가 [rax+0x10]에서 8바이트를 읽으려 했는데 rax가 0이었습니다."),
      "the sentence");
    bool afterParameters = false;
    for (std::size_t i = 1; i < r.evidence.size(); ++i) {
      if (Contains(r.evidence[i].title, en ? L"What the faulting instruction did" : L"예외를 낸 명령어가 한 일")) {
        afterParameters = r.evidence[i].details == sentence &&
          r.evidence[i].confidence_level == i18n::ConfidenceLevel::kHigh &&
          Contains(r.evidence[i - 1].title, en ? L"Exception parameter analysis" : L"예외 파라미터 분석");
      }
    }
    Require(afterParameters, "a high evidence item right after the exception parameters");

    const auto summary = skydiag::dump_tool::BuildSummaryJson(r, std::filesystem::temp_directory_path(), L"stem", true);
    Require(summary.contains("fault_instruction"), "summary JSON field");
    const auto& fi = summary["fault_instruction"];
    Require(fi["verdict"] == "null_pointer" && fi["access"] == "read" && fi["address"] == "0x10" &&
              fi["mnemonic"] == "mov" && fi["operand"] == "qword ptr [rax+0x10]" && fi["bytes"] == "48 8B 58 10" &&
              fi["access_size"] == 8 && fi["address_matches"] == true && fi["effective_address"] == "0x10" &&
              fi["base_register"] == "rax" && fi["base_value"] == "0x0" && !fi.contains("index_register") &&
              !fi.contains("index_value") && !fi.contains("scale") &&
              fi["text"] == WideToUtf8(sentence),
      "summary JSON fault_instruction");
    Require(!summary.contains("fault_instruction") || !fi.contains("call_site"), "no call site for a data fault");
    Require(!plain.evidence.empty() &&
              !skydiag::dump_tool::BuildSummaryJson(plain, std::filesystem::temp_directory_path(), L"stem", true)
                 .contains("fault_instruction"),
      "no field without a decoded instruction");

    const auto report = skydiag::dump_tool::BuildReportText(r, summary, true);
    Require(report.find(std::string(en ? "FaultInstruction: " : "예외 명령어: ") +
                        "mov qword ptr [rax+0x10] (48 8B 58 10), read 0x10, rax=0x0 -> null_pointer\n") !=
              std::string::npos,
      "the report line");
    Require(report.find(std::string(en ? "  How it crashed: " : "  어떻게 튕겼나: ") +
                        WideToUtf8(DescribeFaultInstructionPlain(decoded, en)) + "\n") != std::string::npos,
      "the quick answer block's how-it-crashed line");
    Require(summary["quick_answer"]["how_it_crashed"] == WideToUtf8(DescribeFaultInstructionPlain(decoded, en)),
      "summary JSON how_it_crashed");
  }

  // Registers that do not give the address: a medium item that only states
  // the read and the address, and no quick-answer clue.
  auto unproven = crash();
  auto noRegisters = fault;
  noRegisters.has_registers = false;
  unproven.fault_instruction = ExplainFaultInstruction(noRegisters, code.data(), code.size());
  const auto before = BuildQuickAnswer(crash(), i18n::Language::kEnglish, CrashContext(true, L"SkyrimUpscaler.dll"));
  const auto unprovenQa = BuildQuickAnswer(unproven, i18n::Language::kEnglish, CrashContext(true, L"SkyrimUpscaler.dll"));
  Require(unprovenQa.why_not_certain == before.why_not_certain && unprovenQa.how_it_crashed.empty(),
    "no clue without the registers");
  BuildEvidenceAndSummary(unproven, i18n::Language::kEnglish);
  bool medium = false;
  for (const auto& e : unproven.evidence) {
    if (Contains(e.title, L"What the faulting instruction did")) {
      medium = e.confidence_level == i18n::ConfidenceLevel::kMedium &&
        e.details == L"The code at SkyrimUpscaler.dll+0x156144 tried to read memory at 0x10 and could not. An address "
                     L"this close to 0 usually means a null pointer: an object that was already gone or never created.";
    }
  }
  Require(medium, "address-only evidence");
}

// ADR-0022, in the shape of the 2026-10-26 case: Alt+F4, and 0.8 s later
// PrismaUI.dll's present hook called a freed hook trampoline. The player reads
// that this was the forced close, not a CTD, and that there is nothing to do;
// the analysis below stays for mod authors. Never called a CTD.
// ADR-0025: a save that keeps failing to load in different places.
void TestDamagedSave()
{
  using skydiag::dump_tool::EventRow;
  namespace internal = skydiag::dump_tool::internal;
  constexpr wchar_t kSave[] = L"Save76_FC70D793_0_646F7";

  // From the blackbox: the newest SaveLoadStart with no SaveLoadEnd after it.
  {
    // The plugin's own payload (SkyrimDiagSaveLoad.h).
    const auto start = [](double t, std::string_view name) {
      EventRow e{};
      e.t_ms = t;
      e.type = static_cast<std::uint16_t>(skydiag::EventType::kSaveLoadStart);
      const auto payload = skydiag::MakeSaveLoadStartPayload(name);
      e.a = payload.a;
      e.b = payload.b;
      e.c = payload.c;
      e.d = payload.d;
      return e;
    };
    const auto at = [](double t, skydiag::EventType type, std::uint64_t a = 0) {
      EventRow e{};
      e.t_ms = t;
      e.type = static_cast<std::uint16_t>(type);
      e.a = a;
      return e;
    };
    const std::string_view name76 = "Save76_FC70D793_0_646F726F_Tamriel_000843_20261101071101_14_1";
    const std::string_view name75 = "Save75_FC70D793_0_646F726F_Tamriel_000839_20261101064910_14_1";
    auto info = internal::DetectSaveLoad({ at(1000.0, skydiag::EventType::kHeartbeat), start(240100.0, name76),
      at(259600.0, skydiag::EventType::kCrash, 0xC0000005u) });
    char key[17]{};
    std::snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(skydiag::SaveNameKey(name76)));
    Require(info.seen && info.in_progress && info.name == kSave && info.key == key && info.timed_to_crash &&
              info.seconds_loading > 19.49 && info.seconds_loading < 19.51,
      "a crash while loading a save");
    // A freeze has no Crash event: the newest event is all there is.
    info = internal::DetectSaveLoad({ start(1000.0, name76), at(41000.0, skydiag::EventType::kHeartbeat) });
    Require(info.in_progress && !info.timed_to_crash && info.seconds_loading == 40.0, "a load with no crash");
    // A new game leaves no save in play.
    info = internal::DetectSaveLoad({ start(1000.0, name76), at(9000.0, skydiag::EventType::kSaveLoadEnd, 1),
      at(20000.0, skydiag::EventType::kSaveLoadEnd, 2), at(60000.0, skydiag::EventType::kCrash, 0xC0000005u) });
    Require(!info.seen && !info.in_progress && info.name.empty() && info.key.empty(), "a new game after a load");
    // No name: no key, so unnamed loads never match each other.
    info = internal::DetectSaveLoad({ start(1000.0, ""), at(2000.0, skydiag::EventType::kCrash, 0xC0000005u) });
    Require(info.seen && info.in_progress && info.key.empty(), "a load with no name");
    // A UTF-8 name is read as UTF-8 and cut before a character it would split
    // (seven 3-byte letters fit in 23 bytes; /utf-8 makes this literal UTF-8).
    info = internal::DetectSaveLoad(
      { start(1000.0, "한글세이브이름입니다"), at(2000.0, skydiag::EventType::kCrash, 0xC0000005u) });
    Require(info.name == L"한글세이브이름", "a UTF-8 save name");
    info = internal::DetectSaveLoad({ start(1000.0, name75), at(9000.0, skydiag::EventType::kSaveLoadEnd, 1),
      at(60000.0, skydiag::EventType::kCrash, 0xC0000005u) });
    Require(info.seen && !info.in_progress && info.seconds_loading == 0.0 && info.name == L"Save75_FC70D793_0_646F7",
      "a crash after the load finished");
    info = internal::DetectSaveLoad({ start(1000.0, name75), at(9000.0, skydiag::EventType::kSaveLoadEnd, 1),
      start(20000.0, name76), at(25000.0, skydiag::EventType::kCrash, 0xC0000005u) });
    Require(info.in_progress && info.name == kSave, "the newest load counts");
    // ADR-0025 (2026-11-06): another save this session loaded, newest first.
    Require(info.other_loaded_name == L"Save75_FC70D793_0_646F7" && info.other_loaded_this_session &&
              info.other_loaded_unix == 0u,
      "another save this session loaded");
    info = internal::DetectSaveLoad({ start(1000.0, name75), at(9000.0, skydiag::EventType::kSaveLoadEnd, 1),
      start(10000.0, name76), at(15000.0, skydiag::EventType::kSaveLoadEnd, 1), start(20000.0, name76),
      at(25000.0, skydiag::EventType::kCrash, 0xC0000005u) });
    Require(info.in_progress && info.other_loaded_name == L"Save75_FC70D793_0_646F7",
      "the same save loading before is not another save");
    info = internal::DetectSaveLoad({ start(1000.0, name75), at(9000.0, skydiag::EventType::kSaveLoadEnd, 0),
      start(20000.0, name76), at(25000.0, skydiag::EventType::kCrash, 0xC0000005u) });
    Require(info.in_progress && info.other_loaded_name.empty() && !info.other_loaded_this_session,
      "a failed load of another save is no load");
    info = internal::DetectSaveLoad({ start(1000.0, name75), at(9000.0, skydiag::EventType::kSaveLoadEnd, 1),
      start(20000.0, ""), at(25000.0, skydiag::EventType::kCrash, 0xC0000005u) });
    Require(info.in_progress && info.other_loaded_name.empty(), "an unnamed load cannot tell another save");
    info = internal::DetectSaveLoad({ start(1000.0, name75), at(9000.0, skydiag::EventType::kSaveLoadEnd, 1),
      at(60000.0, skydiag::EventType::kCrash, 0xC0000005u) });
    Require(!info.in_progress && info.other_loaded_name.empty(), "only a load in progress asks");
    Require(!internal::DetectSaveLoad({ at(1000.0, skydiag::EventType::kCrash, 0xC0000005u) }).seen, "no load seen");

    Require(internal::EventTypeName(start(0.0, name76).type) == L"SaveLoadStart" &&
              internal::EventTypeName(static_cast<std::uint16_t>(skydiag::EventType::kSaveLoadEnd)) == L"SaveLoadEnd",
      "the events' names");
    const auto e = start(0.0, name76);
    Require(internal::FormatEventDetail(e.type, e.a, e.b, e.c, e.d) == std::wstring(L"save=") + kSave &&
              internal::FormatEventDetail(static_cast<std::uint16_t>(skydiag::EventType::kSaveLoadEnd), 1, 0, 0, 0) ==
                L"loaded" &&
              internal::FormatEventDetail(static_cast<std::uint16_t>(skydiag::EventType::kSaveLoadEnd), 0, 0, 0, 0) ==
                L"failed" &&
              internal::FormatEventDetail(static_cast<std::uint16_t>(skydiag::EventType::kSaveLoadEnd), 2, 0, 0, 0) ==
                L"new_game",
      "the events' details");
  }

  const auto crash = [&](std::size_t earlier, std::size_t places, std::size_t otherSaves) {
    auto r = CrashIn(L"LocationalEncounterZones.dll", L"Locational Encounter Zones");
    r.suspects.push_back(SuspectItem{});
    r.suspects.back().module_filename = L"LocationalEncounterZones.dll";
    r.suspects.back().confidence_level = i18n::ConfidenceLevel::kMedium;
    r.state_flags = skydiag::kState_Loading;
    r.save_load.seen = true;
    r.save_load.in_progress = true;
    r.save_load.name = kSave;
    r.save_load.key = "0123456789abcdef";
    r.save_load.seconds_loading = 19.4;
    r.save_load.timed_to_crash = true;
    r.save_load.earlier_crashes = earlier;
    r.save_load.crash_places = places;
    r.save_load.other_save_crashes = otherSaves;
    return r;
  };
  const auto saveLine = [](const AnalysisResult& r, std::wstring_view tag) {
    for (std::size_t i = 0; i < r.recommendations.size(); ++i) {
      if (r.recommendations[i].rfind(tag, 0) == 0) {
        return i;
      }
    }
    return r.recommendations.size();
  };
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    const std::wstring damagedTag = en ? L"[Damaged save] " : L"[세이브 손상] ";
    const std::wstring saveTag = en ? L"[Save] " : L"[세이브] ";

    // Two failed loads of one save, in two places: the save leads.
    auto plain = crash(0, 0, 0);
    plain.save_load = {};
    BuildEvidenceAndSummary(plain, lang);
    auto r = crash(1, 2, 0);
    BuildEvidenceAndSummary(r, lang);
    const auto& qa = r.quick_answer;
    const std::wstring lead = en
      ? std::wstring(L"Loading the same save (") + kSave + L") has crashed or frozen 2 times so far, in 2 different "
          L"places. The save's data may be damaged; one mod is less likely."
      : std::wstring(L"같은 세이브(") + kSave + L")를 불러오다 이번까지 2번 튕기거나 멈췄고, 난 곳이 2곳으로 서로 "
          L"달랐습니다. 모드 하나의 문제보다 이 세이브의 데이터가 깨졌을 가능성이 있습니다.";
    Require(qa.kind == "crash" && qa.case_id == "damaged_save" && qa.likelihood == "medium" && qa.suspect_mod.empty(),
      "the damaged save leads");
    Require(qa.headline == (en ? L"The game crashed to desktop (CTD). " : L"게임이 튕겼습니다(CTD). ") + lead,
      "the damaged-save headline");
    Require(Contains(qa.action, en ? L"Load a save made before this one." : L"이 세이브보다 먼저 만든 세이브를 불러와 보세요.") &&
              Contains(qa.action, en ? L"check whether the problem comes back only with the save that failed"
                                     : L"문제가 난 세이브에서만 반복되는지 확인해 보세요") &&
              !Contains(qa.action, en ? L"this save only" : L"이 세이브만의 문제"),
      "load an earlier save, and see whether only this one fails");
    Require(!qa.why_not_certain.empty(), "why it is not certain");
    RequirePlainWords(qa, "damaged save");
    Require(r.summary_sentence == lead + L" " + plain.summary_sentence, "summary prefix");
    Require(!r.recommendations.empty() && r.recommendations[0].rfind(damagedTag, 0) == 0 &&
              skydiag::dump_tool::SelectNextActionIndex(r.recommendations) == 0u,
      "the damaged-save line is the next action");
    Require(saveLine(r, saveTag) == r.recommendations.size(), "no one-off save line beside it");
    bool evidence = false;
    for (const auto& item : r.evidence) {
      if (item.title == (en ? L"Captured while a save was loading" : L"캡처 당시 세이브를 불러오는 중")) {
        evidence = item.confidence_level == i18n::ConfidenceLevel::kMedium &&
                   Contains(item.details, en ? L"19.4 s after its load started" : L"19.4초 뒤") &&
                   Contains(item.details, en ? L"Earlier failed loads of the same save: 1 (in 2 places with this one)."
                                             : L"같은 세이브를 불러오다 난 이전 사고: 1번(이번까지 난 곳 2곳).");
      }
    }
    Require(evidence, "the save-load evidence item");

    Require(!skydiag::dump_tool::BuildSummaryJson(r, std::filesystem::temp_directory_path(), L"stem", true)["save_load"]
               .contains("history_fingerprint"),
      "no fingerprint when no history was read");
    r.save_load.history_fingerprint = "9509785e8fa8add1";
    const auto summary = skydiag::dump_tool::BuildSummaryJson(r, std::filesystem::temp_directory_path(), L"stem", true);
    Require(summary["save_load"]["history_fingerprint"] == "9509785e8fa8add1",
      "the history the judgement read, for the viewer to tell it is stale");
    Require(summary["save_load"]["name"] == WideToUtf8(kSave) && summary["save_load"]["in_progress"] == true &&
              summary["save_load"]["seconds_loading"] == 19.4 && summary["save_load"]["earlier_failed_loads"] == 1 &&
              summary["save_load"]["failed_load_places"] == 2 && summary["save_load"]["other_save_failed_loads"] == 0 &&
              summary["save_load"]["damaged_save_suspected"] == true && summary["quick_answer"]["case"] == "damaged_save",
      "summary JSON save_load");
    Require(!skydiag::dump_tool::BuildSummaryJson(plain, std::filesystem::temp_directory_path(), L"stem", true)
               .contains("save_load"),
      "no field without a save load");
    const auto report = skydiag::dump_tool::BuildReportText(r, summary, true);
    Require(report.find(en ? "Save: Save76_FC70D793_0_646F7 (loading, 19.4 s in; earlier failed loads 1, places 2)\n"
                           : "세이브: Save76_FC70D793_0_646F7 (불러오는 중, 19.4초째; 이전에 불러오다 난 사고 1번, 난 곳 2곳)\n") !=
              std::string::npos,
      "the report's save line");

    // The same place again may be one mod's bug with this save: the mod leads.
    auto samePlace = crash(1, 1, 0);
    BuildEvidenceAndSummary(samePlace, lang);
    Require(samePlace.quick_answer.case_id == plain.quick_answer.case_id &&
              samePlace.summary_sentence == plain.summary_sentence,
      "same place: the analysis leads");
    const auto once = saveLine(samePlace, saveTag);
    Require(once < samePlace.recommendations.size() && saveLine(samePlace, damagedTag) == samePlace.recommendations.size() &&
              Contains(samePlace.recommendations[once],
                en ? L"Loading this save (Save76_FC70D793_0_646F7) has crashed or frozen 2 times so far, in the same "
                     L"place each time"
                   : L"세이브(Save76_FC70D793_0_646F7)를 불러오다 이번까지 2번 튕기거나 멈췄습니다. 매번 같은 곳이라") &&
              skydiag::dump_tool::SelectNextActionIndex(samePlace.recommendations) != once,
      "same place: a save line, not the next action");

    auto unknownPlace = crash(1, 1, 0);
    unknownPlace.save_load.all_places_known = false;
    BuildEvidenceAndSummary(unknownPlace, lang);
    const auto advice = saveLine(unknownPlace, saveTag);
    Require(unknownPlace.quick_answer.case_id == plain.quick_answer.case_id &&
              advice < unknownPlace.recommendations.size() &&
              !Contains(unknownPlace.recommendations[advice], en ? L"same place" : L"매번 같은 곳"),
      "An earlier load with no recorded location proves neither the same place nor a different one");

    // The first failed load of a save: what would tell.
    auto first = crash(0, 0, 0);
    BuildEvidenceAndSummary(first, lang);
    const auto firstLine = saveLine(first, saveTag);
    Require(first.quick_answer.case_id == plain.quick_answer.case_id && firstLine < first.recommendations.size() &&
              Contains(first.recommendations[firstLine],
                en ? L"crashed while loading a save (Save76_FC70D793_0_646F7). If loading it fails again"
                   : L"세이브(Save76_FC70D793_0_646F7)를 불러오다 튕겼습니다. 같은 세이브에서 또 그러면"),
      "the first failed load");

    // ADR-0025 (2026-11-06): another save that loaded, told right after what
    // happened and before the advice.
    {
      Require(r.quick_answer.why_not_certain ==
                (en ? L"There is no record of another save loading fine, so whether only this save fails is not known."
                    : L"다른 세이브를 정상적으로 불러온 기록이 없어서, 이 세이브에서만 실패하는지는 알 수 없습니다."),
        "damaged save with no other load known");
      auto withOther = crash(0, 0, 0);
      withOther.save_load.other_loaded_name = L"Save75_FC70D793_0_646F7";
      withOther.save_load.other_loaded_this_session = true;
      BuildEvidenceAndSummary(withOther, lang);
      const std::wstring sessionSentence = en
        ? L"Another save (Save75_FC70D793_0_646F7) loaded fine earlier in this game session."
        : L"이번 게임에서 먼저 불러온 다른 세이브(Save75_FC70D793_0_646F7)는 정상적으로 열렸습니다.";
      const auto otherLine = saveLine(withOther, saveTag);
      Require(otherLine < withOther.recommendations.size() &&
                Contains(withOther.recommendations[otherLine],
                  (en ? L"while loading a save (Save76_FC70D793_0_646F7). " : L"튕겼습니다. ") + sessionSentence +
                    (en ? L" If loading it fails again" : L" 같은 세이브에서 또 그러면")),
        "the save line tells another save loaded");
      bool told = false;
      for (const auto& item : withOther.evidence) {
        told = told || Contains(item.details, sessionSentence);
      }
      Require(told, "the evidence item tells it too");
      const auto otherSummary =
        skydiag::dump_tool::BuildSummaryJson(withOther, std::filesystem::temp_directory_path(), L"stem", true);
      Require(otherSummary["save_load"]["other_save_loaded"]["name"] == "Save75_FC70D793_0_646F7" &&
                otherSummary["save_load"]["other_save_loaded"]["this_session"] == true &&
                otherSummary["save_load"]["other_save_loaded"]["loaded_unix"] == 0 &&
                !skydiag::dump_tool::BuildSummaryJson(first, std::filesystem::temp_directory_path(), L"stem", true)
                   ["save_load"].contains("other_save_loaded"),
        "summary JSON other_save_loaded");
      Require(skydiag::dump_tool::BuildReportText(withOther, otherSummary, true)
                  .find(en ? "(loading, 19.4 s in; another save loaded: Save75_FC70D793_0_646F7)"
                           : "(불러오는 중, 19.4초째; 정상적으로 열린 다른 세이브: Save75_FC70D793_0_646F7)") !=
                std::string::npos,
        "the report's save line names it");

      // Recorded by the helper in an earlier session: when, in local time.
      auto damagedOther = crash(1, 2, 0);
      damagedOther.save_load.other_loaded_name = L"Save75_FC70D793_0_646F7";
      damagedOther.save_load.other_loaded_unix = 1793520300u;
      BuildEvidenceAndSummary(damagedOther, lang);
      const auto seconds = static_cast<std::time_t>(1793520300);
      std::tm local{};
      wchar_t when[32]{};
      Require(localtime_s(&local, &seconds) == 0 && std::wcsftime(when, 32, L"%m/%d %H:%M", &local) > 0u, "local time");
      Require(damagedOther.quick_answer.case_id == "damaged_save" &&
                damagedOther.quick_answer.why_not_certain ==
                  (en ? (std::wstring(L"Another save (Save75_FC70D793_0_646F7) loaded fine at ") + when +
                          L". A mod may still fail only on this save's data, so it is not certain.")
                      : (std::wstring(L"다른 세이브(Save75_FC70D793_0_646F7)는 ") + when +
                          L"에 정상적으로 열렸습니다. 그래도 어떤 모드가 이 세이브의 데이터에서만 실패하는 것일 수 있어 단정할 수는 없습니다.")),
        "a damaged save tells when another save loaded");
    }

    // A capture with no Crash event says when the last record was, not the crash.
    auto stuck = crash(0, 0, 0);
    stuck.save_load.timed_to_crash = false;
    BuildEvidenceAndSummary(stuck, lang);
    bool lastRecord = false;
    for (const auto& item : stuck.evidence) {
      lastRecord = lastRecord || Contains(item.details, en ? L"the last record is 19.4 s after its load started"
                                                           : L"마지막 기록은 불러오기를 시작한 지 19.4초 뒤입니다");
    }
    const auto stuckSummary =
      skydiag::dump_tool::BuildSummaryJson(stuck, std::filesystem::temp_directory_path(), L"stem", true);
    Require(lastRecord && stuckSummary["save_load"]["timed_to_crash"] == false &&
              skydiag::dump_tool::BuildReportText(stuck, stuckSummary, true)
                  .find(en ? "(loading, last record 19.4 s in)" : "(불러오는 중, 마지막 기록 19.4초째)") != std::string::npos,
      "no Crash event: the last record");

    // Which captures count as a failed load in the crash history.
    {
      namespace internal = skydiag::dump_tool::internal;
      auto counted = crash(0, 0, 0);
      counted.is_crash_like = true;
      Require(internal::CountsAsFailedLoad(counted), "a crash while loading counts");
      auto autoFreeze = crash(0, 0, 0);
      autoFreeze.is_hang_like = true;
      Require(internal::CountsAsFailedLoad(autoFreeze), "an automatic freeze capture counts");
      auto manual = autoFreeze;
      manual.is_manual_capture = true;
      Require(!internal::CountsAsFailedLoad(manual), "a manual capture does not");
      auto snapshot = crash(0, 0, 0);
      snapshot.is_snapshot_like = true;
      Require(!internal::CountsAsFailedLoad(snapshot), "a snapshot does not");
      auto closing = counted;
      closing.forced_close.detected = true;
      Require(!internal::CountsAsFailedLoad(closing), "a forced close does not");
      auto unnamed = counted;
      unnamed.save_load.key.clear();
      Require(!internal::CountsAsFailedLoad(unnamed), "a load with no name does not");
      auto finished = counted;
      finished.save_load.in_progress = false;
      Require(!internal::CountsAsFailedLoad(finished), "a finished load does not");
    }

    // Where a failed load failed: the faulting module and offset, else what the
    // faulting instruction did; not the crash bucket, which holds the callers.
    {
      namespace internal = skydiag::dump_tool::internal;
      auto inModule = crash(0, 0, 0);
      inModule.fault_module_filename = L"SkyrimSE.exe";
      inModule.fault_module_offset = 0x172a1e;
      inModule.crash_bucket_key = L"CTD3-7dbaf39d10226dde";
      auto otherCaller = inModule;
      otherCaller.crash_bucket_key = L"CTD3-0000000000000001";
      Require(internal::SaveLoadFailurePlace(inModule) == "0xc0000005|skyrimse.exe+0x172a1e" &&
                internal::SaveLoadFailurePlace(otherCaller) == internal::SaveLoadFailurePlace(inModule),
        "one fault from two call paths is one place");
      auto noModule = crash(0, 0, 0);
      noModule.fault_module_filename.clear();
      noModule.fault_instruction = {};
      Require(internal::SaveLoadFailurePlace(noModule).empty(), "an exception code alone has no known place");
      noModule.fault_instruction.verdict = "bad_jump";
      noModule.fault_instruction.access = "execute";
      noModule.fault_instruction.target_state = "not_executable";
      Require(internal::SaveLoadFailurePlace(noModule) == "0xc0000005|bad_jump|execute|not_executable",
        "a jump into data has a place without a module");
      auto freeze = crash(0, 0, 0);
      freeze.exc_code = 0;
      freeze.crash_bucket_key = L"HANG1-a29167f39cb6f9b0";
      Require(internal::SaveLoadFailurePlace(freeze) == "freeze|HANG1-a29167f39cb6f9b0", "a freeze's place");
      freeze.crash_bucket_key.clear();
      Require(internal::SaveLoadFailurePlace(freeze).empty(), "a freeze without a bucket has no known place");
    }

    // A known cause with its own fix leads everywhere: the quick answer, the
    // next action and the summary sentence.
    {
      auto known = crash(1, 2, 0);
      skydiag::dump_tool::SignatureMatch sig{};
      sig.id = "CONTENT_CATALOG_FORMAT_MISMATCH";
      sig.scope = "root_cause";
      sig.confidence_level = i18n::ConfidenceLevel::kMedium;
      sig.recommendations.push_back(en ? L"[Known cause] Move ContentCatalog.txt to the Recycle Bin."
                                       : L"[알려진 원인] ContentCatalog.txt를 휴지통으로 옮기세요.");
      known.signature_match = sig;
      BuildEvidenceAndSummary(known, lang);
      Require(known.quick_answer.case_id == "known_issue" &&
                !Contains(known.summary_sentence, en ? L"Loading the same save" : L"같은 세이브(") &&
                saveLine(known, damagedTag) == known.recommendations.size(),
        "a known cause leads, not the save");
    }

    // Other saves failed too: the mod setup, not one save.
    auto otherSaves = crash(1, 2, 1);
    BuildEvidenceAndSummary(otherSaves, lang);
    const auto setup = saveLine(otherSaves, saveTag);
    Require(otherSaves.quick_answer.case_id != "damaged_save" && setup < otherSaves.recommendations.size() &&
              Contains(otherSaves.recommendations[setup], en ? L"the mod setup is more likely" : L"모드 구성 쪽일 가능성이 큽니다"),
      "other saves failed too");

    // A load that finished before the crash says nothing about the save.
    auto loaded = crash(1, 2, 0);
    loaded.save_load.in_progress = false;
    BuildEvidenceAndSummary(loaded, lang);
    Require(loaded.quick_answer.case_id != "damaged_save" && saveLine(loaded, saveTag) == loaded.recommendations.size(),
      "a finished load");
    const auto loadedSummary =
      skydiag::dump_tool::BuildSummaryJson(loaded, std::filesystem::temp_directory_path(), L"stem", true);
    Require(skydiag::dump_tool::BuildReportText(loaded, loadedSummary, true)
                .find(en ? "Save: Save76_FC70D793_0_646F7 (loaded before the capture)\n"
                         : "세이브: Save76_FC70D793_0_646F7 (캡처 전에 불러오기 끝남)\n") != std::string::npos,
      "the report's finished-load line");

    // A forced close stays a forced close.
    auto closing = crash(1, 2, 0);
    closing.forced_close.detected = true;
    closing.forced_close.kind = "alt_f4";
    BuildEvidenceAndSummary(closing, lang);
    Require(closing.quick_answer.case_id == "forced_close" && saveLine(closing, damagedTag) == closing.recommendations.size() &&
              saveLine(closing, saveTag) == closing.recommendations.size(),
      "a forced close");
  }
}

// The summary, the checklist and the share text name a candidate's signal
// families with one set of words.
void TestCandidateFamilyLabels()
{
  namespace internal = skydiag::dump_tool::internal;
  skydiag::dump_tool::ActionableCandidate candidate{};
  candidate.supporting_families = { "crash_logger_frame", "history_repeat", "actionable_stack", "resource_provider" };
  Require(internal::JoinCandidateFamilies(candidate, false) ==
            L"Crash Logger 프레임 + 반복 이력 + 스택 분석 + 근처 리소스 제공 모드",
    "Korean family labels");
  Require(internal::JoinCandidateFamilies(candidate, true) ==
            L"Crash Logger frame + history repeat + actionable stack + near resource provider",
    "English family labels");
  Require(internal::CandidateHasFamily(candidate, "history_repeat") &&
            !internal::CandidateHasFamily(candidate, "hang_thread_group"),
    "family lookup");
  Require(internal::DescribeCandidateFamily("not_a_family", false) == L"기타 신호", "an unknown family");
  candidate.supporting_families.clear();
  Require(internal::JoinCandidateFamilies(candidate, false) == L"제한된 근거", "no family");
}

// Batch D: what a player reads first in the text report, and no noise in it.
struct GpuDriverLogFixture
{
  std::string json = R"({"version":1,"since_unix":1000,"until_unix":2000,"total":3,"events":[
    {"provider":"nvlddmkm","id":153,"level":2,"unix":1500,"message":"Error occurred on GPUID: 100"},
    {"provider":"nvlddmkm","id":153,"level":2,"unix":1700,"message":"Error occurred on GPUID: 100"},
    {"provider":"Display","id":4101,"level":3,"unix":1900,
     "message":"디스플레이 드라이버 nvlddmkm이(가) 응답을 멈추었으며 복구되었습니다."}]})";
};

// ADR-0026: the display drivers' events the helper put in a freeze capture.
void TestGpuDriverLog()
{
  namespace dt = skydiag::dump_tool;
  GpuDriverLogFixture fixture;
  dt::GpuDriverLog log{};
  Require(!dt::ApplyGpuDriverLogJson("{ damaged", log) && !log.recorded, "a damaged stream is ignored");
  Require(!dt::ApplyGpuDriverLogJson(R"({"version":2,"events":[]})", log) && !log.recorded, "another version");
  Require(dt::ApplyGpuDriverLogJson(fixture.json, log) && log.recorded && log.total == 3u && log.events.size() == 3u &&
            log.events[0].provider == L"nvlddmkm" && log.events[2].id == 4101u && dt::GpuDriverProblemCount(log) == 3u,
    "the helper's stream is read");
  Require(dt::DescribeGpuDriverLog(log, 2000u, false) ==
              L"게임 시작 이후 3건: Display 4101 1번(디스플레이 드라이버 nvlddmkm이(가) 응답을 멈추었으며 복구되었습니다.), "
              L"nvlddmkm 153 2번(Error occurred on GPUID: 100), 마지막은 캡처 100초 전" &&
            dt::DescribeGpuDriverLog(log, 2000u, true) ==
              L"3 since the game started: Display 4101 x1 (디스플레이 드라이버 nvlddmkm이(가) 응답을 멈추었으며 "
              L"복구되었습니다.), nvlddmkm 153 x2 (Error occurred on GPUID: 100), the last 100 s before the capture",
    "grouped by driver and event, the newest first");
  dt::GpuDriverLog none{};
  Require(dt::ApplyGpuDriverLogJson(R"({"version":1,"since_unix":10,"until_unix":20,"total":0,"events":[]})", none) &&
            dt::DescribeGpuDriverLog(none, 20u, false) == L"게임 시작 이후 없음" &&
            dt::DescribeGpuDriverLog(dt::GpuDriverLog{}, 20u, false).empty(),
    "none, and nothing when the log was not read");

  // An information event (from a stream that kept them) is neither counted,
  // grouped nor the last one: the error was 1 h 40 min before the capture.
  dt::GpuDriverLog mixed{};
  Require(dt::ApplyGpuDriverLogJson(R"({"version":1,"since_unix":1000,"until_unix":8000,"total":3,"events":[
    {"provider":"nvlddmkm","id":153,"level":2,"unix":2000,"message":"Error occurred on GPUID: 100"},
    {"provider":"igfx","id":1,"level":4,"unix":7900,"message":"info"},
    {"provider":"igfx","id":1,"level":4,"unix":7950,"message":"info"}]})", mixed) &&
            dt::GpuDriverProblemCount(mixed) == 1u,
    "one error among information events");
  Require(dt::DescribeGpuDriverLog(mixed, 8000u, false) ==
            L"게임 시작 이후 1건: nvlddmkm 153 1번(Error occurred on GPUID: 100), 마지막은 캡처 100분 전",
    "errors and warnings only, and the time of the last of them");

  // The helper could not read the log to the end: none found is not none.
  dt::GpuDriverLog unfinished{};
  Require(dt::ApplyGpuDriverLogJson(
            R"({"version":1,"since_unix":10,"until_unix":20,"total":0,"complete":false,"events":[]})", unfinished) &&
            !unfinished.complete && dt::DescribeGpuDriverLog(unfinished, 20u, false) == L"찾은 것 없음(기록을 끝까지 읽지 못함)" &&
            dt::DescribeGpuDriverLog(unfinished, 20u, true) == L"none found, but the log could not be read to the end",
    "an unfinished read is not none");
  Require(log.complete && Contains(dt::DescribeGpuDriverLog(log, 2000u, false), L"마지막은 캡처") &&
            !Contains(dt::DescribeGpuDriverLog(log, 2000u, false), L"끝까지"),
    "a stream without the field was read to the end");
  dt::GpuDriverLog partial = log;
  partial.complete = false;
  Require(Contains(dt::DescribeGpuDriverLog(partial, 2000u, false), L" (기록을 끝까지 읽지 못함), 마지막은 캡처 100초 전"),
    "events found before the read stopped say it stopped");

  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    const std::wstring tag = en ? L"[GPU driver] " : L"[GPU 드라이버] ";
    AnalysisResult hang{};
    hang.dump_path = L"C:\\out\\SkyrimDiag_Hang_20261106_170000_000.dmp";
    hang.freeze_analysis.has_analysis = true;
    hang.freeze_analysis.state_id = "freeze_candidate";
    hang.dump_capture_unix = 2000u;
    hang.gpu_driver_log = log;
    BuildEvidenceAndSummary(hang, lang);
    bool line = false;
    for (const auto& rec : hang.recommendations) {
      line = line || (rec.rfind(tag, 0) == 0 && Contains(rec, en ? L"reinstall the graphics driver" : L"그래픽 드라이버를 깨끗이 다시 설치"));
    }
    Require(hang.is_hang_like && line, "a freeze with driver errors: the GPU line");
    bool evidence = false;
    for (const auto& item : hang.evidence) {
      evidence = evidence || (item.title == (en ? L"The graphics driver logged errors while the game ran"
                                                : L"게임 중 그래픽 드라이버 오류가 기록됨") &&
                               item.confidence_level == i18n::ConfidenceLevel::kMedium &&
                               Contains(item.details, en ? L"does not make it the cause" : L"단정할 수는 없습니다"));
    }
    Require(evidence, "the evidence item");
    const auto summary = skydiag::dump_tool::BuildSummaryJson(hang, std::filesystem::temp_directory_path(), L"stem", true);
    Require(summary["gpu_driver_events"]["total"] == 3 && summary["gpu_driver_events"]["problems"] == 3 &&
              summary["gpu_driver_events"]["complete"] == true &&
              summary["gpu_driver_events"]["events"].size() == 3u &&
              summary["gpu_driver_events"]["events"][0]["message"] == "Error occurred on GPUID: 100",
      "summary JSON gpu_driver_events");
    Require(skydiag::dump_tool::BuildReportText(hang, summary, true)
                .find(en ? "GPU driver events: 3 since the game started" : "그래픽 드라이버 기록: 게임 시작 이후 3건") !=
              std::string::npos,
      "the report line");

    // Informational events only, or none: no GPU line.
    AnalysisResult quiet = hang;
    quiet.recommendations.clear();
    quiet.evidence.clear();
    quiet.gpu_driver_log = none;
    BuildEvidenceAndSummary(quiet, lang);
    bool quietLine = false;
    bool quietEvidence = false;
    for (const auto& rec : quiet.recommendations) {
      quietLine = quietLine || rec.rfind(tag, 0) == 0;
    }
    for (const auto& item : quiet.evidence) {
      quietEvidence = quietEvidence || (item.title == (en ? L"Graphics driver events while the game ran" : L"게임 중 그래픽 드라이버 기록") &&
                                        item.confidence_level == i18n::ConfidenceLevel::kLow);
    }
    Require(!quietLine && quietEvidence, "no driver errors: said, and no GPU line");

    // A crash is not told to look at the GPU from this alone.
    auto crash = CrashIn(L"SkyrimSE.exe", L"");
    crash.gpu_driver_log = log;
    BuildEvidenceAndSummary(crash, lang);
    bool crashLine = false;
    for (const auto& rec : crash.recommendations) {
      crashLine = crashLine || rec.rfind(tag, 0) == 0;
    }
    Require(!crashLine, "only a freeze gets the GPU line");
  }
}

// ADR-0025 (2026-11-06): the loads the helper records, as the analyzer reads them.
void TestLoadedSavesFile()
{
  namespace dt = skydiag::dump_tool;
  const auto dir = std::filesystem::temp_directory_path() / "skydiag_quick_answer_loaded_saves";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  const auto path = dir / skydiag::kLoadedSavesFileName;
  const auto write = [&](std::string_view text) {
    std::ofstream(path, std::ios::binary) << text;
  };
  write(R"({"version":1,"loads":[
    {"save_key":"aaaaaaaaaaaaaaaa","save_name":"Save75","loaded_unix":1000,"attempt_key":"10-1-5"},
    {"save_key":"bbbbbbbbbbbbbbbb","save_name":"Save76","loaded_unix":5000,"attempt_key":"10-1-9"},
    {"save_key":"cccccccccccccccc","save_name":"Save77","loaded_unix":200000,"attempt_key":"20-2-3"},
    {"save_name":"no key","loaded_unix":3000},
    {"save_key":"dddddddddddddddd","save_name":"no time"},
    "not an object"]})");
  const auto loads = dt::ReadLoadedSaves(path);
  Require(loads.size() == 3u && loads[0].name_utf8 == "Save75" && loads[1].loaded_unix == 5000u &&
            loads[2].attempt_key == "20-2-3",
    "rows without a key or a time are left out");

  auto other = dt::NewestOtherLoadedSave(loads, "bbbbbbbbbbbbbbbb", 6000u);
  Require(other && other->name_utf8 == "Save75", "another save, not the one that failed");
  other = dt::NewestOtherLoadedSave(loads, "cccccccccccccccc", 6000u);
  Require(other && other->name_utf8 == "Save76", "the newest other save");
  other = dt::NewestOtherLoadedSave(loads, "aaaaaaaaaaaaaaaa", 4000u);
  Require(!other, "a load after the capture is left out");
  other = dt::NewestOtherLoadedSave(loads, "aaaaaaaaaaaaaaaa", 5000u + 86400u);
  Require(other && other->name_utf8 == "Save76", "a day before the capture counts");
  other = dt::NewestOtherLoadedSave(loads, "aaaaaaaaaaaaaaaa", 5001u + 86400u);
  Require(!other, "older than a day is left out");
  Require(!dt::NewestOtherLoadedSave(loads, "", 6000u) && !dt::NewestOtherLoadedSave(loads, "aaaaaaaaaaaaaaaa", 0u),
    "no save key or no capture time: nothing");

  write(R"({"version":2,"loads":[{"save_key":"aaaaaaaaaaaaaaaa","loaded_unix":1000}]})");
  Require(dt::ReadLoadedSaves(path).empty(), "another version is not read");
  write("{ damaged");
  Require(dt::ReadLoadedSaves(path).empty(), "a damaged file tells nothing");
  std::filesystem::remove(path, ec);
  Require(dt::ReadLoadedSaves(path).empty(), "no file");
  std::filesystem::remove_all(dir, ec);
}

void TestReportReadability()
{
  using skydiag::dump_tool::EvidenceItem;
  using skydiag::dump_tool::EventRow;
  using skydiag::dump_tool::ReadableModName;
  using skydiag::dump_tool::ResourceRow;

  Require(ReadableModName(L"[NoDelete] 0444 Refraction on Critical Hit") == L"Refraction on Critical Hit" &&
            ReadableModName(L"[NoDelete] Echoes of Skyrim") == L"Echoes of Skyrim" &&
            ReadableModName(L"Val Serano - Pirate Follower") == L"Val Serano - Pirate Follower" &&
            ReadableModName(L"1000 Words") == L"1000 Words" && ReadableModName(L"[NoDelete]") == L"[NoDelete]" &&
            ReadableModName(L"[A] [B] 0137 Mod") == L"Mod",
    "modlist tags and the order number go, a name stays");

  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    const auto build = [&](bool crash) {
      auto r = CrashIn(L"SmoothCam.dll", L"[NoDelete] 0137 SmoothCam");
      if (!crash) {
        r = AnalysisResult{};
        r.dump_path = L"C:\\Logs\\SkyrimDiag_Manual_20261101_193046_489.dmp";
      }
      BuildEvidenceAndSummary(r, lang);  // the quick answer; the lists below replace the rest
      EvidenceItem slot{};
      slot.title = L"slot limit";
      slot.details = L"near the limit";
      slot.setup_note = true;
      EvidenceItem history{};
      history.title = L"history";
      history.details = L"first line\nsecond line";
      r.evidence = { slot, history };
      for (int i = 0; i < 50; ++i) {
        ResourceRow rr{};
        rr.t_ms = 1000.0 + i;
        rr.kind = L"nif";
        rr.path = L"meshes\\m" + std::to_wstring(i) + L".nif";
        rr.providers = { L"[NoDelete] 0444 Refraction on Critical Hit" };
        r.resources.push_back(rr);
      }
      const auto event = [](std::uint32_t i, skydiag::EventType type, std::wstring detail) {
        EventRow e{};
        e.i = i;
        e.t_ms = 2000.0 + i;
        e.type = static_cast<std::uint16_t>(type);
        e.type_name = skydiag::dump_tool::internal::EventTypeName(e.type);
        e.detail = std::move(detail);
        e.a = 7;
        return e;
      };
      for (std::uint32_t i = 0; i < 5; ++i) {
        r.events.push_back(event(i, skydiag::EventType::kThreadCreate, L"tid=1"));
      }
      r.events.push_back(event(5, skydiag::EventType::kMenuOpen, L"Loading Menu"));
      r.events.push_back(event(6, skydiag::EventType::kNote, L""));
      r.events.push_back(event(7, skydiag::EventType::kModuleLoad, L"DNSAPI.dll"));
      r.events.push_back(event(8, skydiag::EventType::kModuleLoad, L"NSI.dll"));
      r.recommendations = { L"[Check] do this first" };
      const auto summary = skydiag::dump_tool::BuildSummaryJson(r, std::filesystem::temp_directory_path(), L"stem", true);
      return skydiag::dump_tool::BuildReportText(r, summary, true);
    };

    const auto crash = build(true);
    const auto manual = build(false);
    Require(crash.find("ExceptionCode: 0x") != std::string::npos && manual.find("ExceptionCode:") == std::string::npos &&
              manual.find("ThreadId: 0") == std::string::npos,
      "no exception block without an exception");

    const auto at = [&](const std::string& text, const char* what) { return text.find(what); };
    const auto checklist = at(crash, en ? "Recommendations (checklist):" : "권장 조치(체크리스트):");
    const auto resources = at(crash, en ? "Recent resources" : "최근 리소스(");
    const auto events = at(crash, en ? "Last events" : "최근 이벤트(");
    const auto setup = at(crash, en ? "Setup notes" : "설치 환경 메모");
    Require(checklist != std::string::npos && checklist < resources && checklist < events && setup != std::string::npos &&
              checklist < setup,
      "the checklist comes before the long lists, and setup notes after it");
    Require(crash.find("slot limit") > setup && crash.find("history") < checklist, "a setup note is listed apart");
    Require(crash.find("  first line\n  second line\n") != std::string::npos, "every detail line is indented");
    Require(crash.find(en ? "(last 40 of 50; all of them are in *_SkyrimDiagSummary.json)"
                          : "(마지막 40개 / 전체 50개. 전체 목록은 *_SkyrimDiagSummary.json에 있습니다)") != std::string::npos &&
              crash.find("meshes\\m9.nif") == std::string::npos && crash.find("meshes\\m10.nif") != std::string::npos,
      "the newest 40 resources");
    // Text for people loses the tags; paths and the analysis keep the folder name.
    const std::string quick = crash.substr(0, at(crash, en ? "Details" : "자세한 분석"));
    Require(crash.find("providers=Refraction on Critical Hit") != std::string::npos &&
              crash.find("providers=[NoDelete]") == std::string::npos && quick.find("SmoothCam") != std::string::npos &&
              quick.find("[NoDelete]") == std::string::npos,
      "providers and the quick answer without modlist tags");
    Require(crash.find("[0-4] t_ms=2000.0-2004.0 ThreadCreate x5\n") != std::string::npos &&
              crash.find("[7-8] t_ms=2007.0-2008.0 ModuleLoad x2: DNSAPI.dll, NSI.dll\n") != std::string::npos,
      "a run of thread churn on one line, and a run of module loads with their names");
    Require(crash.find("Loading Menu\n") != std::string::npos && crash.find("Loading Menu a=") == std::string::npos &&
              crash.find("Note a=7 b=0 c=0 d=0\n") != std::string::npos,
      "raw words only for an event with no detail");
  }
}

void TestForcedClose()
{
  using skydiag::dump_tool::ExplainFaultInstruction;
  using skydiag::dump_tool::FaultRecord;
  const auto crash = []() {
    auto r = CrashIn(L"PrismaUI.dll", L"PrismaUI");
    r.exc_addr = 0x7FF7055F0000ull;
    r.exc_info = { 8, 0x7FF7055F0000ull };
    FaultRecord fault{};
    fault.exception_address = r.exc_addr;
    fault.access_kind = 8;
    fault.address = r.exc_addr;
    r.fault_instruction = ExplainFaultInstruction(fault, nullptr, 0);
    r.fault_instruction.target_state = "freed";
    r.suspects.push_back(SuspectItem{});
    r.suspects.back().module_filename = L"PrismaUI.dll";
    r.suspects.back().confidence_level = i18n::ConfidenceLevel::kMedium;
    return r;
  };
  for (const auto lang : kLanguages) {
    const bool en = lang == i18n::Language::kEnglish;
    auto plain = crash();
    BuildEvidenceAndSummary(plain, lang);
    auto r = crash();
    r.forced_close.detected = true;
    r.forced_close.kind = "alt_f4";
    r.forced_close.seconds_before_crash = 0.8;
    BuildEvidenceAndSummary(r, lang);

    const auto& qa = r.quick_answer;
    Require(qa.kind == "forced_close" && qa.case_id == "forced_close" && qa.likelihood == "none" &&
              qa.suspect_mod.empty() && qa.why_not_certain.empty() && qa.how_it_crashed.empty(),
      "a forced close names no suspect");
    Require(qa.headline == (en ? L"This error happened while you force-quit the game (Alt+F4). It is not a crash during "
                                 L"play. Saves are usually unaffected."
                               : L"Alt+F4로 게임을 강제 종료하는 과정에서 난 오류입니다. 플레이 중에 튕긴 CTD가 "
                                 L"아닙니다. 세이브에는 보통 영향이 없습니다."),
      "the forced-close headline");
    Require(qa.action == (en ? L"Nothing to do." : L"따로 할 일은 없습니다."), "nothing to do");
    RequirePlainWords(qa, "forced close");

    // The summary sentence leads with it and keeps the analysis after it.
    const std::wstring lead = en ? L"This error happened while you force-quit the game (Alt+F4). It is not a crash "
                                   L"during play. "
                                 : L"Alt+F4로 게임을 강제 종료하는 과정에서 난 오류입니다. 플레이 중에 튕긴 CTD가 아닙니다. ";
    Require(r.summary_sentence == lead + plain.summary_sentence, "summary prefix");
    Require(r.suspects.size() == plain.suspects.size() && r.fault_instruction.verdict == "bad_jump",
      "the analysis stays");

    Require(!r.evidence.empty() &&
              r.evidence[0].title == (en ? L"Error during a forced close (not a CTD)" : L"강제 종료 중 오류(CTD 아님)") &&
              r.evidence[0].confidence_level == i18n::ConfidenceLevel::kHigh &&
              Contains(r.evidence[0].details, en ? L"Alt+F4 0.8 s before this error" : L"0.8초 전 Alt+F4 입력"),
      "the forced-close evidence item first");
    Require(r.evidence.size() == plain.evidence.size() + 1u, "the rest of the evidence stays");
    Require(!r.recommendations.empty() &&
              r.recommendations[0].rfind(en ? L"[Forced close] " : L"[강제 종료] ", 0) == 0 &&
              skydiag::dump_tool::SelectNextActionIndex(r.recommendations) == 0u,
      "the forced-close line is the next action");
    for (const auto& text : r.recommendations) {
      Require(!Contains(text, L"real CTD") && !Contains(text, L"실제 CTD"), "never called a real CTD");
    }

    const auto summary = skydiag::dump_tool::BuildSummaryJson(r, std::filesystem::temp_directory_path(), L"stem", true);
    Require(summary["analysis"]["is_forced_close"] == true && summary["analysis"]["is_crash_like"] == true,
      "summary JSON analysis flags");
    Require(summary["forced_close"]["kind"] == "alt_f4" && summary["forced_close"]["seconds_before_crash"] == 0.8 &&
              summary["forced_close"]["window_seconds"] == 10.0,
      "summary JSON forced_close");
    Require(summary["quick_answer"]["kind"] == "forced_close", "summary JSON quick answer");
    Require(summary["fault_instruction"]["target_state"] == "freed", "summary JSON target_state");
    Require(!skydiag::dump_tool::BuildSummaryJson(plain, std::filesystem::temp_directory_path(), L"stem", true)
               .contains("forced_close"),
      "no field for an ordinary crash");

    const auto report = skydiag::dump_tool::BuildReportText(r, summary, true);
    Require(report.find(en ? "Classification: FORCED_CLOSE (not a CTD; alt_f4, 0.8 s before the error)\n"
                           : "분류: 강제 종료 (CTD 아님; alt_f4, 오류 0.8초 전)\n") != std::string::npos,
      "the report line");
    Require(report.find(std::string(en ? "FaultInstruction: " : "예외 명령어: ") + "execute 0x7FF7055F0000 (freed) -> bad_jump") !=
              std::string::npos,
      "the report's fault instruction says freed");
  }

  // The other kinds keep to the player's own action.
  struct KindCase
  {
    const char* kind;
    const wchar_t* en;
    const wchar_t* ko;
  };
  for (const auto& c : { KindCase{ "window_close", L"This error happened while you closed the game window.",
                                   L"게임 창을 닫아 게임을 종료하는 과정에서 난 오류입니다." },
                         KindCase{ "close_message", L"This error happened while you closed the game window.",
                                   L"게임 창을 닫아 게임을 종료하는 과정에서 난 오류입니다." },
                         KindCase{ "game_quit", L"This error happened while the game was closing after you chose to quit.",
                                   L"게임 종료를 고른 뒤 게임이 닫히는 과정에서 난 오류입니다." },
                         KindCase{ "forced_quit",
                                   L"This error happened while the game was being force-quit (Alt+F4 or similar). It is "
                                   L"not a crash during play. Saves are usually unaffected.",
                                   L"게임을 강제로 끄는 과정(Alt+F4 등)에서 난 오류입니다. 플레이 중에 튕긴 CTD가 아닙니다. "
                                   L"세이브에는 보통 영향이 없습니다." } }) {
    for (const auto lang : kLanguages) {
      const bool en = lang == i18n::Language::kEnglish;
      auto r = crash();
      r.forced_close.detected = true;
      r.forced_close.kind = c.kind;
      r.forced_close.seconds_before_crash = 2.0;
      BuildEvidenceAndSummary(r, lang);
      Require(r.quick_answer.headline.rfind(en ? c.en : c.ko, 0) == 0, c.kind);
    }
  }

  // From the blackbox: the newest Crash event 0.8 s after a CloseRequest.
  {
    using skydiag::dump_tool::EventRow;
    namespace internal = skydiag::dump_tool::internal;
    const auto row = [](double t, skydiag::EventType type, std::uint64_t a, std::uint64_t b = 0, std::uint64_t c = 0) {
      EventRow e{};
      e.t_ms = t;
      e.type = static_cast<std::uint16_t>(type);
      e.a = a;
      e.b = b;
      e.c = c;
      return e;
    };
    const std::vector<EventRow> events = {
      row(1000.0, skydiag::EventType::kHeartbeat, 0),
      row(120000.0, skydiag::EventType::kCloseRequest, 1, 0x0112, 0xF060),
      row(120800.0, skydiag::EventType::kCrash, 0xC0000005u),
    };
    const auto fc = internal::DetectForcedClose(events);
    Require(fc.detected && fc.kind == "alt_f4" && fc.seconds_before_crash > 0.79 && fc.seconds_before_crash < 0.81,
      "detected from the blackbox");
    Require(!internal::DetectForcedClose({ events[0], events[2] }).detected, "no close request, no forced close");
    Require(internal::EventTypeName(events[1].type) == L"CloseRequest", "the event's name");
    Require(internal::FormatEventDetail(events[1].type, 1, 0x0112, 0xF060, 0) ==
              L"kind=alt_f4 msg=0x0112 wparam=0xF060 sent",
      "the event's detail");
    Require(internal::FormatEventDetail(events[1].type, 1, 0x0104, 0x73, 1) == L"kind=alt_f4 msg=0x0104 wparam=0x0073 posted",
      "a posted key-down");
    Require(internal::FormatEventDetail(events[1].type, 4, 0, 1, 0) == L"kind=game_quit quit_flag reason=menu_open",
      "game quit detail");
    Require(internal::FormatEventDetail(events[1].type, 1, 0, 0, 3) == L"kind=alt_f4 keys_held", "the key sampler");
    Require(internal::FormatEventDetail(events[1].type, 1, 0, 0, 4) == L"kind=alt_f4 keys_held_at_crash",
      "keys held at the crash");
    Require(internal::FormatEventDetail(events[1].type, 5, 0, 3, 2) ==
              L"kind=forced_quit quit_flag reason=no_menu_no_keys",
      "quit flag source");
    Require(internal::FormatEventDetail(events[1].type, 5, 0, 3, 0) ==
              L"kind=forced_quit quit_flag reason=no_menu_no_keys",
      "forced quit detail");
  }

  // A validated clean exit says more (code 0) and wins.
  auto clean = crash();
  clean.forced_close.detected = true;
  clean.forced_close.kind = "alt_f4";
  clean.is_filtered_clean_exit = true;
  BuildEvidenceAndSummary(clean, i18n::Language::kEnglish);
  Require(clean.quick_answer.kind == "clean_exit", "clean exit first");
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
    TestFreezeEngineLockDeadlock();
    TestFreezeKnownAiPathingFix();
    TestFreezeObjectClue();
    TestModalDialog();
    TestModalDialogNamedModule();
    TestModalDialogNamesNoSingleModule();
    TestLoadingFreezeWithoutSuspect();
    TestFreezeStackScanIsWeak();
    TestSnapshotsAndCleanExit();
    TestSummaryAndReportStartWithIt();
    TestFaultInstructionClue();
    TestForcedClose();
    TestDamagedSave();
    TestLoadedSavesFile();
    TestGpuDriverLog();
    TestReportReadability();
    TestCandidateFamilyLabels();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
  std::printf("quick_answer_tests: all passed\n");
  return 0;
}
