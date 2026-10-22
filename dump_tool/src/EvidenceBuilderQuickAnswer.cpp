// The quick answer (QuickAnswer.h): the lead the summary sentence follows,
// said in everyday words for players. It mirrors the branch order of
// BuildSummarySentence so both name the same mod, and it reads the finished
// candidates and confidence without changing them.

#include "EvidenceBuilderEvidencePipeline.h"
#include "EvidenceBuilderPrivate.h"

#include <cstdint>
#include <cwctype>
#include <string>
#include <string_view>
#include <utility>

#include "DumpObjects.h"
#include "MainThreadWait.h"
#include "MinidumpUtil.h"

namespace skydiag::dump_tool::internal {
namespace {

// What the answer names.
enum class LeadType : std::uint8_t {
  kNone,
  kMod,            // a DLL or plugin of a mod
  kProgram,        // software outside the game and its mods
  kCrashLogger,    // Crash Logger's own thread dump
  kGpuDriver,      // the graphics driver side
};

// Why the lead is not certain (the why_not_certain line).
enum class Doubt : std::uint8_t {
  kNone,
  kInsideMod,        // the game crashed inside this mod
  kOtherFile,        // it crashed elsewhere; clues point here
  kConflict,         // the clues are split between two mods
  kNearbyOnly,       // only seen near the crash or freeze in memory
  kPluginData,       // the game was handling this plugin's data
  kSharedMod,        // a hook framework many mods run through
  kFreezeRunning,    // this mod's code was on the stuck thread
  kDeadlock,         // threads waiting on each other
  kProgram,          // the crash was inside external software
  kGpu,              // the game waited on the graphics driver
  kCrashLogger,      // the thread dump deadlock is inferred
  kKnownIssue,       // matches a known problem
  kModalCaller,      // the dialog's opener
  kModalNamed,       // the DLL the dialog names (its opener was not seen)
};

struct Lead
{
  LeadType type = LeadType::kNone;
  std::string case_id = "none";
  std::wstring name;  // mod name, program name or readable file name
  std::wstring file;  // file name only
  bool is_plugin_file = false;     // .esp/.esm/.esl
  bool is_hook_framework = false;  // update rather than disable
  i18n::ConfidenceLevel level = i18n::ConfidenceLevel::kUnknown;
  Doubt doubt = Doubt::kNone;
  std::wstring conflict_name;  // the other mod, for Doubt::kConflict
};

bool SameFile(std::wstring_view a, std::wstring_view b)
{
  return !a.empty() && !b.empty() && minidump::WideLower(a) == minidump::WideLower(b);
}

bool IsPluginFileName(std::wstring_view name)
{
  const std::wstring lower = minidump::WideLower(name);
  const auto endsWith = [&](std::wstring_view suffix) {
    return lower.size() >= suffix.size() && lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
  };
  return endsWith(L".esp") || endsWith(L".esm") || endsWith(L".esl");
}

// Only the file name, should a path ever reach here.
std::wstring BaseName(std::wstring_view path)
{
  const auto slash = path.find_last_of(L"\\/");
  return std::wstring(slash == std::wstring_view::npos ? path : path.substr(slash + 1));
}

// The MO2 mod a module came from, from any part of the analysis that knows it.
std::wstring ModNameForModule(const AnalysisResult& r, std::wstring_view file)
{
  if (file.empty()) {
    return {};
  }
  for (const auto& c : r.actionable_candidates) {
    if (!c.mod_name.empty() && SameFile(c.module_filename, file)) {
      return c.mod_name;
    }
  }
  for (const auto& s : r.suspects) {
    if (!s.inferred_mod_name.empty() && SameFile(s.module_filename, file)) {
      return s.inferred_mod_name;
    }
  }
  if (!r.inferred_mod_name.empty() && SameFile(r.fault_module_filename, file)) {
    return r.inferred_mod_name;
  }
  if (!r.hook_stub_race.target_mod_name.empty() && SameFile(r.hook_stub_race.target_module, file)) {
    return r.hook_stub_race.target_mod_name;
  }
  if (!r.main_thread_wait.blocked_mod_name.empty() && SameFile(r.main_thread_wait.blocked_module, file)) {
    return r.main_thread_wait.blocked_mod_name;
  }
  if (!r.main_thread_lock_owner.top_mod_name.empty() && SameFile(r.main_thread_lock_owner.top_module, file)) {
    return r.main_thread_lock_owner.top_mod_name;
  }
  if (!r.modal_dialog_wait.caller_inferred_mod_name.empty() &&
      SameFile(r.modal_dialog_wait.caller_module_filename, file)) {
    return r.modal_dialog_wait.caller_inferred_mod_name;
  }
  if (!r.modal_dialog_wait.named_mod_name.empty() && SameFile(r.modal_dialog_wait.named_module_filename, file)) {
    return r.modal_dialog_wait.named_mod_name;
  }
  return {};
}

Lead ModLead(const AnalysisResult& r, std::wstring_view file, std::wstring_view modName, i18n::ConfidenceLevel level)
{
  Lead lead;
  lead.type = LeadType::kMod;
  lead.file = BaseName(file);
  lead.name = !modName.empty() ? std::wstring(modName) : ModNameForModule(r, lead.file);
  if (lead.name.empty()) {
    lead.name = lead.file;
  }
  lead.is_plugin_file = IsPluginFileName(lead.file);
  lead.is_hook_framework = !lead.is_plugin_file && minidump::IsKnownHookFramework(lead.file);
  lead.level = level == i18n::ConfidenceLevel::kUnknown ? i18n::ConfidenceLevel::kLow : level;
  return lead;
}

Lead CandidateLead(const AnalysisResult& r, const ActionableCandidate& candidate)
{
  const std::wstring file = !candidate.module_filename.empty() ? candidate.module_filename
    : !candidate.plugin_name.empty()                          ? candidate.plugin_name
                                                               : candidate.display_name;
  Lead lead = ModLead(r, file, candidate.mod_name, candidate.confidence_level);
  lead.case_id = CandidateHasFamily(candidate, "crash_logger_object_ref") && candidate.module_filename.empty()
    ? "object_ref"
    : "candidate";
  return lead;
}

Lead SuspectLead(const AnalysisResult& r, const SuspectItem& suspect, i18n::ConfidenceLevel level)
{
  Lead lead = ModLead(r, suspect.module_filename, suspect.inferred_mod_name, level);
  lead.case_id = lead.is_hook_framework ? "hook_framework" : "suspect";
  return lead;
}

// A system DLL or the game itself is never shown to a player as a mod to disable.
bool IsModModule(const AnalysisResult& r, std::wstring_view file)
{
  return !file.empty() && !DumpModuleIsSystemish(r, file) && !minidump::IsGameExeModule(file) &&
         !IsGraphicsStackModule(file);
}

std::wstring CandidateName(const ActionableCandidate& candidate)
{
  if (!candidate.mod_name.empty()) {
    return candidate.mod_name;
  }
  return !candidate.display_name.empty() ? candidate.display_name : candidate.module_filename;
}

void SetDoubtForCrash(const AnalysisResult& r, Lead& lead)
{
  if (lead.doubt != Doubt::kNone) {
    return;
  }
  if (lead.case_id == "object_ref") {
    lead.doubt = Doubt::kPluginData;
  } else if (lead.is_hook_framework) {
    lead.doubt = Doubt::kSharedMod;
  } else if (SameFile(lead.file, r.fault_module_filename) ||
             (!r.inferred_mod_name.empty() && lead.name == r.inferred_mod_name)) {
    lead.doubt = Doubt::kInsideMod;
  } else {
    lead.doubt = Doubt::kOtherFile;
  }
}

// The crash branches of BuildSummarySentence, in the same order.
Lead CrashLead(const AnalysisResult& r, const EvidenceBuildContext& ctx)
{
  const ActionableCandidate* top = r.actionable_candidates.empty() ? nullptr : &r.actionable_candidates[0];
  const ActionableCandidate* second = r.actionable_candidates.size() > 1u ? &r.actionable_candidates[1] : nullptr;
  const SuspectItem* topSuspect = r.suspects.empty() ? nullptr : &r.suspects[0];
  const SuspectItem* nonHookSuspect = nullptr;
  for (const auto& s : r.suspects) {
    if (IsActionableSuspect(s)) {
      nonHookSuspect = &s;
      break;
    }
  }
  const auto suspectLevel = [](const SuspectItem& s) {
    return s.confidence_level == i18n::ConfidenceLevel::kUnknown ? i18n::ConfidenceLevel::kMedium : s.confidence_level;
  };
  const bool topMatchesFault = top && SameFile(top->module_filename, r.fault_module_filename);
  const bool topBackedByFrame =
    top && ((r.crash_logger_direct_fault_eligible && SameFile(top->module_filename, r.crash_logger_direct_fault_module)) ||
            (r.crash_logger_first_actionable_probable_eligible &&
             SameFile(top->module_filename, r.crash_logger_first_actionable_probable_module)) ||
            (r.crash_logger_probable_streak_eligible &&
             SameFile(top->module_filename, r.crash_logger_probable_streak_module)));
  const bool topHasFrame = top && CandidateHasFamily(*top, "crash_logger_frame");
  const auto conflictLead = [&]() {
    Lead lead = CandidateLead(r, *top);
    lead.doubt = Doubt::kConflict;
    lead.conflict_name = CandidateName(*second);
    return lead;
  };

  Lead lead;
  if (ctx.hasModule && !ctx.isSystem && !ctx.isGameExe && ctx.isHookFramework) {
    const bool topIsActionableDll = top && !top->module_filename.empty() &&
      !minidump::IsKnownHookFramework(top->module_filename) && !DumpModuleIsSystemish(r, top->module_filename) &&
      !minidump::IsGameExeModule(top->module_filename);
    if (topIsActionableDll && topBackedByFrame) {
      lead = CandidateLead(r, *top);
    } else if (nonHookSuspect) {
      lead = SuspectLead(r, *nonHookSuspect, suspectLevel(*nonHookSuspect));
    } else {
      const std::wstring fault = minidump::WideLower(r.fault_module_filename);
      if (fault == L"crashlogger.dll" || fault == L"crashloggersse.dll") {
        return lead;  // Crash Logger's own secondary fault names no mod.
      }
      lead = ModLead(r, r.fault_module_filename, r.inferred_mod_name, i18n::ConfidenceLevel::kLow);
      lead.case_id = "hook_framework";
    }
  } else if (ctx.hasModule && !ctx.isSystem && !ctx.isGameExe) {
    const bool weakFaultLocation = top && IsWeakFaultLocationActionableCandidate(r, *top, ctx);
    if (topMatchesFault && weakFaultLocation) {
      lead = CandidateLead(r, *top);
    } else if (top && top->status_id == "conflicting" && second) {
      lead = conflictLead();
    } else if (topMatchesFault &&
               (top->status_id == "cross_validated" || top->status_id == "related" ||
                top->status_id == "reference_clue")) {
      lead = CandidateLead(r, *top);
    } else if (top && (top->status_id == "cross_validated" || top->status_id == "related")) {
      lead = CandidateLead(r, *top);
    } else {
      lead = ModLead(r, r.fault_module_filename, r.inferred_mod_name, i18n::ConfidenceLevel::kLow);
      lead.case_id = "fault_module";
    }
  } else if (ctx.hasModule && ctx.isSystem) {
    if (top && top->status_id == "cross_validated") {
      lead = CandidateLead(r, *top);
    } else if (top && topHasFrame && topBackedByFrame) {
      lead = CandidateLead(r, *top);
    } else if (top && top->status_id == "related" && CandidateHasStandaloneCallstackSupport(r, *top)) {
      lead = CandidateLead(r, *top);
    } else if (nonHookSuspect) {
      lead = SuspectLead(r, *nonHookSuspect, suspectLevel(*nonHookSuspect));
    } else if (topSuspect) {
      // The summary names it at Low; a system DLL is not a mod to disable.
      if (IsModModule(r, topSuspect->module_filename)) {
        lead = SuspectLead(r, *topSuspect, i18n::ConfidenceLevel::kLow);
      }
    } else if (r.exc_code == 0xE06D7363u) {
      return lead;
    } else if (!ctx.externalSoftware.empty()) {
      lead.type = LeadType::kProgram;
      lead.case_id = "external_software";
      lead.name = ctx.externalSoftware;
      lead.file = BaseName(r.fault_module_filename);
      lead.level = i18n::ConfidenceLevel::kLow;
      lead.doubt = Doubt::kProgram;
      return lead;
    }
  } else if (ctx.hasModule && ctx.isGameExe) {
    const auto* culpritRef = FirstCulpritObjectRef(r);
    if (top && top->status_id == "conflicting" && second) {
      lead = conflictLead();
    } else if (top &&
               (top->status_id == "cross_validated" || (topHasFrame && topBackedByFrame) || top->status_id == "related" ||
                (top->status_id == "reference_clue" &&
                 (topHasFrame || CandidateHasFamily(*top, "crash_logger_object_ref"))))) {
      lead = CandidateLead(r, *top);
    } else if (nonHookSuspect && r.suspects_from_stackwalk) {
      lead = SuspectLead(r, *nonHookSuspect, suspectLevel(*nonHookSuspect));
    } else if (culpritRef) {
      lead = ModLead(r, culpritRef->esp_name, L"", i18n::ConfidenceLevel::kMedium);
      lead.case_id = "object_ref";
    } else if (topSuspect && minidump::IsKnownHookFramework(topSuspect->module_filename)) {
      lead = SuspectLead(r, *topSuspect, i18n::ConfidenceLevel::kLow);
    } else if (topSuspect && IsModModule(r, topSuspect->module_filename)) {
      lead = SuspectLead(
        r, *topSuspect, r.suspects_from_stackwalk ? suspectLevel(*topSuspect) : i18n::ConfidenceLevel::kLow);
      if (!r.suspects_from_stackwalk) {
        lead.doubt = Doubt::kNearbyOnly;
      }
    }
  } else if (topSuspect && IsModModule(r, topSuspect->module_filename)) {
    lead = SuspectLead(r, *topSuspect, suspectLevel(*topSuspect));
    if (!r.suspects_from_stackwalk) {
      lead.doubt = Doubt::kNearbyOnly;
    }
  }
  if (lead.type == LeadType::kMod && lead.is_plugin_file && lead.case_id == "candidate") {
    lead.case_id = "object_ref";
  }
  if (lead.type != LeadType::kNone) {
    SetDoubtForCrash(r, lead);
  }
  return lead;
}

Lead GpuLead(std::wstring_view driver, i18n::ConfidenceLevel level, bool en)
{
  Lead lead;
  lead.type = LeadType::kGpuDriver;
  lead.case_id = "gpu_driver";
  lead.name = en ? L"the graphics driver" : L"그래픽 드라이버";
  lead.file = BaseName(driver);
  lead.level = level;
  lead.doubt = Doubt::kGpu;
  return lead;
}

// The freeze branches of BuildSummarySentence, in the same order.
Lead FreezeLead(const AnalysisResult& r, bool en)
{
  const SuspectItem* topSuspect = r.suspects.empty() ? nullptr : &r.suspects[0];
  const SuspectItem* nonHookSuspect = nullptr;
  for (const auto& s : r.suspects) {
    if (IsActionableSuspect(s)) {
      nonHookSuspect = &s;
      break;
    }
  }
  const auto suspectLevel = [](const SuspectItem& s) {
    return s.confidence_level == i18n::ConfidenceLevel::kUnknown ? i18n::ConfidenceLevel::kMedium : s.confidence_level;
  };
  const auto& wait = r.main_thread_wait;
  const auto& owner = r.main_thread_lock_owner;

  Lead lead;
  if (r.modal_dialog_wait.detected) {
    const auto& modal = r.modal_dialog_wait;
    lead.case_id = "modal_dialog";
    if ((modal.caller_kind == "plugin" || modal.caller_kind == "hook_framework") &&
        !modal.caller_module_filename.empty()) {
      const auto level = (modal.window_evidence && modal.stack_evidence) ? i18n::ConfidenceLevel::kHigh
                                                                          : i18n::ConfidenceLevel::kMedium;
      lead = ModLead(r, modal.caller_module_filename, modal.caller_inferred_mod_name, level);
      lead.case_id = "modal_dialog";
      lead.doubt = Doubt::kModalCaller;
    } else if (!modal.named_module_filename.empty()) {
      // No opener on the stack: the dialog's own title or text names the DLL.
      lead = ModLead(r, modal.named_module_filename, modal.named_mod_name, modal.named_module_level);
      lead.case_id = "modal_dialog";
      lead.doubt = Doubt::kModalNamed;
    }
    return lead;
  }
  if (r.crash_logger_thread_dump_deadlock.detected && owner.tid != 0u) {
    lead.type = LeadType::kCrashLogger;
    lead.case_id = "crash_logger_thread_dump";
    lead.name = en ? L"Crash Logger's thread dump" : L"Crash Logger의 스레드 덤프 기능";
    lead.file = BaseName(r.crash_logger_thread_dump_deadlock.crash_logger_module);
    lead.level = CrashLoggerThreadDumpDeadlockConfidence(owner, r.crash_logger_thread_dump_deadlock);
    lead.doubt = Doubt::kCrashLogger;
    return lead;
  }
  if (r.engine_spin.nan_detected) {
    // No mod is named: the engine looped on a NaN value (BuildQuickAnswer
    // says so in its own words).
    lead.case_id = "engine_spin_nan";
    return lead;
  }
  if (r.hang_thread_module_consensus.has_consensus) {
    const auto& file = r.hang_thread_module_consensus.module_filename;
    if (IsModModule(r, file)) {
      lead = ModLead(r, file, L"", i18n::ConfidenceLevel::kMedium);
      lead.case_id = "thread_group";
      lead.doubt = Doubt::kFreezeRunning;
    }
    return lead;
  }
  if (r.freeze_analysis.state_id == "deadlock_likely" || r.freeze_analysis.state_id == "loader_stall_likely") {
    const bool nameSuspect = topSuspect && !(IsBystanderWait(wait) && r.suspects_from_main_thread);
    if (nameSuspect && IsModModule(r, topSuspect->module_filename)) {
      lead = SuspectLead(r, *topSuspect, suspectLevel(*topSuspect));
      lead.doubt = r.freeze_analysis.state_id == "deadlock_likely" ? Doubt::kDeadlock : Doubt::kFreezeRunning;
    }
    return lead;
  }
  if (owner.tid != 0u) {
    const auto level = owner.confirmed ? i18n::ConfidenceLevel::kMedium : i18n::ConfidenceLevel::kLow;
    if (owner.confirmed && IsGraphicsStackModule(owner.top_module)) {
      return GpuLead(owner.top_module, level, en);
    }
    if (IsModModule(r, owner.top_module) && !minidump::IsKnownHookFramework(owner.top_module)) {
      lead = ModLead(r, owner.top_module, owner.top_mod_name, level);
      lead.case_id = "lock_owner";
      lead.doubt = Doubt::kFreezeRunning;
    }
    return lead;
  }
  if (IsBystanderWait(wait) && (r.suspects_from_main_thread || !topSuspect || HasBlockedGameThread(wait))) {
    if (wait.kind == "graphics_driver_wait") {
      return GpuLead(wait.waiting_module, i18n::ConfidenceLevel::kLow, en);
    }
    if (HasBlockedGameThread(wait)) {
      lead = ModLead(r, wait.blocked_module, wait.blocked_mod_name, i18n::ConfidenceLevel::kMedium);
      lead.case_id = "blocked_thread";
      lead.doubt = Doubt::kFreezeRunning;
      return lead;
    }
    if (wait.engine_wait_detail == "gpu_query_poll") {
      return GpuLead(L"", i18n::ConfidenceLevel::kLow, en);
    }
    return lead;
  }
  if (topSuspect) {
    const bool topIsHookOrSystem = minidump::IsKnownHookFramework(topSuspect->module_filename) ||
      minidump::IsSystemishModuleAt(topSuspect->module_filename, topSuspect->module_path);
    if (!r.suspects_from_stackwalk) {
      if (IsModModule(r, topSuspect->module_filename)) {
        lead = SuspectLead(r, *topSuspect, i18n::ConfidenceLevel::kLow);
        lead.doubt = Doubt::kNearbyOnly;
      }
    } else if (topIsHookOrSystem && nonHookSuspect) {
      lead = SuspectLead(r, *nonHookSuspect, suspectLevel(*nonHookSuspect));
      lead.doubt = Doubt::kFreezeRunning;
    } else if (IsModModule(r, topSuspect->module_filename)) {
      lead = SuspectLead(r, *topSuspect, suspectLevel(*topSuspect));
      lead.doubt = Doubt::kFreezeRunning;
    }
  }
  return lead;
}

std::string LikelihoodId(i18n::ConfidenceLevel level)
{
  switch (level) {
    case i18n::ConfidenceLevel::kHigh: return "high";
    case i18n::ConfidenceLevel::kMedium: return "medium";
    default: return "low";
  }
}

std::wstring LikelihoodText(i18n::ConfidenceLevel level, bool en)
{
  switch (level) {
    case i18n::ConfidenceLevel::kHigh: return en ? L"likely" : L"가능성 높음";
    case i18n::ConfidenceLevel::kMedium: return en ? L"possible" : L"가능성 중간";
    default: return en ? L"weak" : L"가능성 낮음";
  }
}

std::wstring Display(const Lead& lead)
{
  if (lead.file.empty() || lead.file == lead.name) {
    return lead.name;
  }
  return lead.name + L" (" + lead.file + L")";
}

std::wstring KindPhrase(std::string_view kind, bool en)
{
  if (kind == "crash") {
    return en ? L"The game crashed to desktop (CTD)." : L"게임이 튕겼습니다(CTD).";
  }
  if (kind == "loading") {
    return en ? L"The game froze on a loading screen." : L"로딩 화면에서 게임이 멈췄습니다(무한 로딩).";
  }
  if (kind == "freeze") {
    return en ? L"The game froze." : L"게임이 멈췄습니다(프리징).";
  }
  return en ? L"The game stopped working." : L"게임에 문제가 생겼습니다.";
}

std::wstring SuspectPhrase(const Lead& lead, bool en)
{
  switch (lead.type) {
    case LeadType::kMod:
      return (en ? L"Suspected mod: " : L"의심되는 모드: ") + Display(lead);
    case LeadType::kProgram:
      return (en ? L"Suspected program: " : L"의심되는 프로그램: ") + Display(lead);
    case LeadType::kCrashLogger:
    case LeadType::kGpuDriver:
      return (en ? L"Suspected cause: " : L"의심되는 원인: ") + Display(lead);
    default:
      return en ? L"The tool could not name a mod." : L"원인 모드는 특정하지 못했습니다.";
  }
}

std::wstring ActionFor(const Lead& lead, bool en)
{
  switch (lead.type) {
    case LeadType::kMod:
      if (lead.is_hook_framework) {
        return en ? (L"Update " + lead.name +
                      L" to its latest version, then play again (many mods rely on it, so disable it only to test).")
                  : (lead.name +
                      L"을(를) 최신 버전으로 업데이트한 뒤 다시 플레이해 보세요(다른 모드들이 기대는 모드라 끄는 것은 시험할 때만).");
      }
      if (lead.is_plugin_file) {
        return en ? (L"Disable or update " + lead.name + L" (or the mod that contains it), then play again.")
                  : (lead.name + L" 플러그인(또는 그 플러그인이 든 모드)을 끄거나 업데이트한 뒤 다시 플레이해 보세요.");
      }
      return en ? (L"Disable or update " + lead.name + L", then play again.")
                : (lead.name + L" 모드를 끄거나 업데이트한 뒤 다시 플레이해 보세요.");
    case LeadType::kProgram:
      return en ? (L"Update or reinstall " + lead.name + L", or turn its overlay off, then play again.")
                : (lead.name + L"을(를) 업데이트하거나 다시 설치하고, 오버레이가 있다면 끈 뒤 다시 플레이해 보세요.");
    case LeadType::kCrashLogger:
      return en ? L"Don't press Crash Logger's thread dump hotkey (or turn that hotkey off in Crash Logger's settings)."
                : L"Crash Logger의 스레드 덤프 단축키를 누르지 마세요(또는 Crash Logger 설정에서 그 단축키를 끄세요).";
    case LeadType::kGpuDriver:
      return en ? L"Update or clean-install the graphics driver, then play again with frame generation and upscalers "
                  L"turned off."
                : L"그래픽 드라이버를 업데이트하거나 클린 설치한 뒤, 프레임 생성과 업스케일러를 끄고 다시 플레이해 보세요.";
    default:
      return en ? L"Share this report when asking for help." : L"도움을 요청할 때 이 리포트를 함께 공유하세요.";
  }
}

std::wstring WhyFor(const Lead& lead, std::string_view kind, bool en)
{
  const bool freeze = kind == "freeze" || kind == "loading";
  switch (lead.doubt) {
    case Doubt::kInsideMod:
      return en ? L"The game crashed inside this mod, but another mod can cause a crash that shows up here."
                : L"게임이 이 모드 안에서 튕겼지만, 다른 모드가 일으킨 문제가 여기서 드러나는 경우도 있습니다.";
    case Doubt::kOtherFile:
      return en ? L"The crash happened in another file and the clues point to this mod, but they are not strong enough "
                  L"to be sure."
                : L"튕긴 곳은 다른 파일이고 단서가 이 모드를 가리키지만, 확신할 만큼 강하지는 않습니다.";
    case Doubt::kConflict:
      return en ? (L"The clues are split between this mod and " + lead.conflict_name + L"; check both.")
                : (L"단서가 이 모드와 " + lead.conflict_name + L"(으)로 갈립니다. 둘 다 확인해 보세요.");
    case Doubt::kNearbyOnly:
      return freeze
        ? (en ? L"This mod was only seen nearby in memory when the game froze, which is a weak clue."
              : L"이 모드는 게임이 멈출 때 메모리 근처에서 보였을 뿐이라 약한 단서입니다.")
        : (en ? L"This mod was only seen nearby in memory when the game crashed, which is a weak clue."
              : L"이 모드는 게임이 튕길 때 메모리 근처에서 보였을 뿐이라 약한 단서입니다.");
    case Doubt::kPluginData:
      return en ? L"The game was handling something from this plugin when it crashed, but another mod working with it "
                  L"can be the real cause."
                : L"게임이 튕길 때 이 플러그인의 데이터를 처리하고 있었지만, 그 데이터를 다루던 다른 모드가 진짜 원인일 수도 있습니다.";
    case Doubt::kSharedMod:
      return en ? L"Many mods run through this one, so the problem may come from another mod."
                : L"여러 모드가 이 모드를 거쳐 동작하므로, 문제가 다른 모드에서 왔을 수도 있습니다.";
    case Doubt::kFreezeRunning:
      return en ? L"This mod was busy when the game froze, but it may only have been waiting for something else."
                : L"게임이 멈출 때 이 모드가 동작 중이었지만, 다른 무언가를 기다리고 있었을 수도 있습니다.";
    case Doubt::kDeadlock:
      return en ? L"Parts of the game were waiting on each other and this mod was one of them, but it may only have "
                  L"been waiting too."
                : L"게임의 여러 작업이 서로를 기다리다 멈췄고 이 모드도 그중 하나였지만, 이 모드 역시 기다리던 쪽일 수 있습니다.";
    case Doubt::kProgram:
      return en ? L"The game crashed inside this program, but a mod can cause a crash that shows up here."
                : L"게임이 이 프로그램 안에서 튕겼지만, 모드가 일으킨 문제가 여기서 드러나는 경우도 있습니다.";
    case Doubt::kGpu:
      return en ? L"The game was waiting on the graphics driver when it froze, but graphics mods or running out of "
                  L"video memory can look the same."
                : L"멈출 때 게임이 그래픽 드라이버를 기다리고 있었지만, 그래픽 모드나 비디오 메모리 부족도 같은 모습으로 나타납니다.";
    case Doubt::kCrashLogger:
      return en ? L"Crash Logger was taking a thread dump when the game stopped, but that it got stuck is inferred, not "
                  L"seen directly."
                : L"게임이 멈출 때 Crash Logger가 스레드 덤프를 만들고 있었지만, 그 기능이 멈춘 것은 직접 본 것이 아니라 추정입니다.";
    case Doubt::kKnownIssue:
      return en ? L"This crash looks like a known problem, but another cause can look the same."
                : L"이 튕김은 알려진 문제와 모양이 같지만, 다른 원인이 같은 모양을 만들 수도 있습니다.";
    case Doubt::kModalCaller:
      return en ? L"This mod opened the window, but the window may be about another mod."
                : L"이 모드가 창을 띄웠지만, 창의 내용은 다른 모드에 대한 것일 수도 있습니다.";
    case Doubt::kModalNamed:
      return en ? L"The error window names this mod's file, but which mod opened the window was not seen."
                : L"오류 창에 이 모드의 파일 이름이 적혀 있지만, 창을 띄운 모드를 직접 확인하지는 못했습니다.";
    default:
      return {};
  }
}

// "[Known cause] Move ... again. It can come back ..." -> "Move ... again."
std::wstring FirstSentenceWithoutTag(std::wstring text)
{
  if (!text.empty() && text.front() == L'[') {
    const auto end = text.find(L']');
    if (end != std::wstring::npos) {
      text.erase(0, end + 1);
    }
  }
  while (!text.empty() && std::iswspace(text.front())) {
    text.erase(0, 1);
  }
  const auto stop = text.find(L". ");
  if (stop != std::wstring::npos) {
    text.erase(stop + 1);
  }
  return text;
}

QuickAnswer Finish(std::string kind, const Lead& lead, bool en)
{
  QuickAnswer answer;
  answer.kind = std::move(kind);
  answer.case_id = lead.case_id;
  if (lead.type != LeadType::kNone) {
    answer.likelihood = LikelihoodId(lead.level);
    answer.likelihood_text = LikelihoodText(lead.level, en);
    answer.suspect_mod = lead.name;
    answer.suspect_file = lead.file;
    if (lead.level != i18n::ConfidenceLevel::kHigh) {
      answer.why_not_certain = WhyFor(lead, answer.kind, en);
    }
  }
  answer.headline = KindPhrase(answer.kind, en) + L" " + SuspectPhrase(lead, en);
  answer.action = ActionFor(lead, en);
  return answer;
}

// ADR-0010 addendum: the form the stuck thread held, after the doubt it
// belongs to. Never a lead: it names no mod as the cause.
void AddFreezeObjectClue(const AnalysisResult& r, bool en, QuickAnswer& answer)
{
  if (answer.why_not_certain.empty()) {
    return;
  }
  if (const auto clue = FreezeTargetObjectClue(r, en); !clue.empty()) {
    answer.why_not_certain += L" " + clue + L".";
  }
}

}  // namespace

QuickAnswer BuildQuickAnswer(const AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx)
{
  const bool en = lang == i18n::Language::kEnglish;
  QuickAnswer answer;

  if (ctx.isFilteredCleanExit) {
    answer.kind = "clean_exit";
    answer.case_id = "clean_exit";
    if (r.clean_exit_filter_context == "late_recovery") {
      answer.headline = en ? L"An error was recorded, but the game kept running afterwards, so this is not counted as "
                             L"a crash."
                           : L"오류가 기록됐지만 게임이 그 뒤에도 계속 실행됐으므로 튕긴 것으로 보지 않습니다.";
      answer.action = en ? L"Nothing to do, unless the game closed right after; then share this report when asking for "
                           L"help."
                         : L"그 직후 게임이 꺼진 것이 아니라면 따로 할 일은 없습니다. 꺼졌다면 도움을 요청할 때 이 리포트를 공유하세요.";
    } else {
      answer.headline = en ? L"The game closed normally; this is not a crash."
                           : L"게임이 정상적으로 종료됐습니다(튕긴 것이 아닙니다).";
      answer.action = en ? L"Nothing to do: no mod is blamed for a normal exit."
                         : L"따로 할 일은 없습니다. 정상 종료에는 원인 모드를 지목하지 않습니다.";
    }
    return answer;
  }
  if (ctx.isSnapshotLike) {
    answer.kind = "snapshot";
    answer.case_id = ctx.isManualCapture ? "manual_snapshot" : "snapshot";
    answer.headline = en ? L"This is a snapshot, not a crash or a freeze." : L"스냅샷입니다(튕김이나 멈춤이 아닙니다).";
    answer.action = ctx.isManualCapture
      ? (en ? L"Nothing to do: this is a snapshot you took, and it does not mean anything is wrong by itself."
            : L"따로 할 일은 없습니다. 직접 찍은 스냅샷이라 그것만으로 문제가 있다는 뜻은 아닙니다.")
      : (en ? L"Nothing to do: a snapshot records the game's state and does not mean anything is wrong by itself."
            : L"따로 할 일은 없습니다. 스냅샷은 게임 상태를 남긴 기록이라 그것만으로 문제가 있다는 뜻은 아닙니다.");
    return answer;
  }

  const bool freezePath = !ctx.hasModule && ctx.isHangLike && !r.hook_stub_race.detected;
  std::string kind = "unknown";
  if (freezePath) {
    kind = (r.freeze_analysis.state_id == "loader_stall_likely" || r.freeze_analysis.blackbox_context.loading_window)
      ? "loading"
      : "freeze";
  } else if (ctx.isCrashLike || ctx.hasModule || r.hook_stub_race.detected) {
    kind = "crash";
  } else if (ctx.isHangLike) {
    kind = "freeze";
  }

  // A known root cause with its own fix comes first, as in the summary.
  if (r.signature_match.has_value() && r.signature_match->scope == "root_cause" &&
      !r.signature_match->recommendations.empty()) {
    Lead lead;
    lead.case_id = "known_issue";
    QuickAnswer known = Finish(kind, lead, en);
    const auto level = r.signature_match->confidence_level == i18n::ConfidenceLevel::kUnknown
      ? i18n::ConfidenceLevel::kMedium
      : r.signature_match->confidence_level;
    known.likelihood = LikelihoodId(level);
    known.likelihood_text = LikelihoodText(level, en);
    // A signature may say it in its own words (a known freeze does).
    known.headline = KindPhrase(kind, en) + L" " +
      (!r.signature_match->headline.empty()
          ? r.signature_match->headline
          : (en ? L"It matches a known problem that has a known fix." : L"해결 방법이 알려진 문제와 일치합니다."));
    known.action = FirstSentenceWithoutTag(r.signature_match->recommendations.front());
    if (level != i18n::ConfidenceLevel::kHigh) {
      Lead doubt;
      doubt.doubt = Doubt::kKnownIssue;
      known.why_not_certain =
        !r.signature_match->why_not_certain.empty() ? r.signature_match->why_not_certain : WhyFor(doubt, kind, en);
    }
    if (freezePath) {
      AddFreezeObjectClue(r, en, known);
    }
    return known;
  }

  if (r.hook_stub_race.detected) {
    const auto& race = r.hook_stub_race;
    Lead lead = ModLead(r, race.target_module, race.target_mod_name, i18n::ConfidenceLevel::kHigh);
    lead.case_id = "hook_stub_race";
    return Finish(kind, lead, en);
  }

  if (!freezePath) {
    return Finish(kind, CrashLead(r, ctx), en);
  }

  const Lead lead = FreezeLead(r, en);
  QuickAnswer freeze = Finish(kind, lead, en);
  if (lead.case_id == "engine_spin_nan") {
    const auto& mods = r.engine_spin.call_path_modules;
    std::wstring callPath;
    for (const auto& mod : mods) {
      callPath += (callPath.empty() ? L"" : L", ") + mod;
    }
    freeze.likelihood = LikelihoodId(i18n::ConfidenceLevel::kMedium);
    freeze.likelihood_text = LikelihoodText(i18n::ConfidenceLevel::kMedium, en);
    freeze.headline = KindPhrase(freeze.kind, en) +
      (en ? L" A value the game was calculating became NaN (not a number), so the engine kept repeating the same "
            L"calculation and froze."
          : L" 게임 계산 값이 숫자가 아닌 값(NaN)이 되어, 엔진이 같은 계산을 끝없이 반복하다 멈췄습니다.");
    freeze.action = en
      ? (L"Check the mods you recently added or updated that change combat, animation, AI/pathing or how actors move: "
          L"disable them one at a time and see whether the freeze stops." +
          (callPath.empty() ? std::wstring{}
                            : (L" The mods seen on the stuck task (" + callPath +
                                L") only called into the game there, so there is no need to remove them for this.")))
      : (L"최근에 추가하거나 업데이트한 모드 중 전투, 애니메이션, AI/길찾기, 캐릭터 움직임을 바꾸는 모드를 하나씩 꺼 보며 "
          L"멈춤이 사라지는지 확인해 보세요." +
          (callPath.empty() ? std::wstring{}
                            : (L" 멈춘 작업에서 함께 보인 모드(" + callPath +
                                L")는 게임 기능을 불렀을 뿐이라 이것 때문에 뺄 필요는 없습니다.")));
    freeze.why_not_certain = en
      ? L"The endless loop on a NaN value is seen directly in the dump, but which mod produced that value is not."
      : L"NaN 값으로 끝없이 반복한 것은 덤프에서 직접 보이지만, 그 값을 어느 모드가 만들었는지는 보이지 않습니다.";
  }
  if (r.modal_dialog_wait.detected) {
    const auto& modal = r.modal_dialog_wait;
    const std::wstring title = modal.dialog_title.empty() ? std::wstring{} : (L"(\"" + modal.dialog_title + L"\")");
    const bool named = lead.doubt == Doubt::kModalNamed;
    std::wstring opener;
    if (named) {
      opener = L" " + SuspectPhrase(lead, en);
    } else if (lead.type != LeadType::kNone) {
      opener = en ? (L" Window opened by: " + Display(lead)) : (L" 창을 띄운 모드: " + Display(lead));
    } else if (const auto files = DescribeDialogNamedFiles(modal, en); !files.empty()) {
      opener = L" " + files;
    }
    freeze.headline = KindPhrase(freeze.kind, en) +
      (en ? (L" It is waiting for an error window it opened" + (title.empty() ? title : L" " + title) +
              L" to be closed.")
          : (L" 게임이 띄운 오류 창" + title + L"이 닫히기를 기다리고 있습니다.")) +
      opener;
    const bool versionIssue = !modal.address_library_issue.empty();
    if (named) {
      freeze.action = modal.named_module_version_issue
        ? (en ? (L"Update " + lead.name + L" to a version made for your game version, or disable it (see what the "
                  L"error window says).")
              : (lead.name + L"을(를) 지금 게임 버전에 맞는 버전으로 업데이트하거나 끄세요(오류 창에 적힌 내용 참고)."))
        : (en ? (L"Follow what the error window says (it may be behind the game window), or update or disable " +
                  lead.name + L".")
              : (L"게임이 띄운 오류 창(게임 창 뒤에 있을 수 있음)에 적힌 내용을 따르거나, " + lead.name +
                  L"을(를) 업데이트하거나 끄세요."));
    } else {
      freeze.action = versionIssue
        ? (en ? L"Read the window the game showed (it may be behind the game window): it says the mod named there does "
                L"not support your game version, so update that mod or disable it."
              : L"게임이 띄운 오류 창(게임 창 뒤에 있을 수 있음)을 읽어 보세요. 창에 적힌 모드가 지금 게임 버전을 지원하지 않는다는 "
                L"내용이니, 그 모드를 업데이트하거나 끄세요.")
        : (en ? L"Read the window the game showed (it may be behind the game window); it usually names the mod with the "
                L"problem."
              : L"게임이 띄운 오류 창(게임 창 뒤에 있을 수 있음)을 읽어 보세요. 보통 어떤 모드가 왜 문제인지 적혀 있습니다.");
    }
  }
  AddFreezeObjectClue(r, en, freeze);
  return freeze;
}

}  // namespace skydiag::dump_tool::internal
