#include "EvidenceBuilderPrivate.h"
#include "EvidenceBuilderEvidencePipeline.h"

#include <algorithm>
#include <cwchar>

#include "MainThreadWait.h"
#include "MinidumpUtil.h"

namespace skydiag::dump_tool::internal {
namespace {

std::wstring DescribeCandidate(const ActionableCandidate& candidate)
{
  if (!candidate.display_name.empty()) {
    return candidate.display_name;
  }
  if (!candidate.plugin_name.empty()) {
    return candidate.plugin_name;
  }
  if (!candidate.mod_name.empty()) {
    return candidate.mod_name;
  }
  if (!candidate.module_filename.empty()) {
    return candidate.module_filename;
  }
  return L"(unknown)";
}

bool LocalCandidateHasFamily(const ActionableCandidate& candidate, std::string_view familyId)
{
  return std::find(candidate.supporting_families.begin(), candidate.supporting_families.end(), familyId) !=
         candidate.supporting_families.end();
}

bool CandidateMatchesModule(const ActionableCandidate& candidate, std::wstring_view module)
{
  if (candidate.module_filename.empty() || module.empty()) {
    return false;
  }
  return minidump::WideLower(candidate.module_filename) == minidump::WideLower(module);
}

bool CandidateHasFrameSupport(const ActionableCandidate& candidate)
{
  return LocalCandidateHasFamily(candidate, "crash_logger_frame");
}

bool CandidateHasObjectRefSupport(const ActionableCandidate& candidate)
{
  return LocalCandidateHasFamily(candidate, "crash_logger_object_ref");
}

bool CandidateHasScorableFirstChanceSupport(const AnalysisResult& r, const ActionableCandidate& candidate)
{
  return LocalCandidateHasFamily(candidate, "first_chance_context") &&
         HasScorableFirstChanceContext(r.first_chance_summary);
}

bool CandidateHasScorableHistorySupport(const AnalysisResult& r, const ActionableCandidate& candidate)
{
  return LocalCandidateHasFamily(candidate, "history_repeat") &&
         (r.history_correlation.count > 1 || !r.bucket_candidate_repeats.empty());
}

bool CandidateHasResourceSupport(const ActionableCandidate& candidate)
{
  return LocalCandidateHasFamily(candidate, "resource_provider");
}

std::wstring DescribeFrameSupport(const AnalysisResult& r, const ActionableCandidate& candidate, bool en)
{
  if (r.crash_logger_direct_fault_eligible &&
      CandidateMatchesModule(candidate, r.crash_logger_direct_fault_module)) {
    return en ? L"Crash Logger frame first (direct DLL fault)" : L"Crash Logger 프레임(DLL 안에서 바로 난 예외)";
  }
  if (r.crash_logger_first_actionable_probable_eligible &&
      CandidateMatchesModule(candidate, r.crash_logger_first_actionable_probable_module)) {
    return en ? L"Crash Logger frame first (first actionable probable DLL frame)"
              : L"Crash Logger 프레임(첫 번째 유력 DLL 프레임)";
  }
  if (r.crash_logger_probable_streak_eligible &&
      CandidateMatchesModule(candidate, r.crash_logger_probable_streak_module)) {
    return en ? L"Crash Logger frame first (probable frame streak)" : L"Crash Logger 프레임(유력 DLL 프레임 연속)";
  }
  // Korean particles after this label assume it ends in "프레임" (이/과).
  return en ? L"Crash Logger frame first" : L"Crash Logger 프레임";
}

std::wstring JoinCandidateFamilies(const ActionableCandidate& candidate, bool en)
{
  std::vector<std::wstring> labels;
  labels.reserve(candidate.supporting_families.size());
  for (const auto& family : candidate.supporting_families) {
    if (family == "crash_logger_frame") {
      labels.push_back(en ? L"Crash Logger frame" : L"Crash Logger 프레임");
    } else if (family == "crash_logger_object_ref") {
      labels.push_back(en ? L"CrashLogger object ref" : L"CrashLogger 오브젝트 참조");
    } else if (family == "actionable_stack") {
      labels.push_back(en ? L"actionable stack" : L"실행 가능한 스택");
    } else if (family == "resource_provider") {
      labels.push_back(en ? L"near resource provider" : L"근처 리소스 제공 모드");
    } else if (family == "history_repeat") {
      labels.push_back(en ? L"history repeat" : L"반복 기록");
    } else if (family == "first_chance_context") {
      labels.push_back(en ? L"repeated first-chance context" : L"반복 선행 예외(first-chance)");
    } else if (family == "hang_thread_group") {
      labels.push_back(en ? L"stable same-module thread group" : L"동일 모듈 정지 스레드 그룹");
    } else if (family == "modal_dialog_owner") {
      labels.push_back(en ? L"modal dialog owner" : L"modal 대화상자 호출 모듈");
    } else if (family == "hook_stub_owner") {
      labels.push_back(en ? L"hook stub owner" : L"후킹 점프 코드 주인");
    }
  }
  return labels.empty() ? (en ? L"limited evidence" : L"제한된 근거") : JoinList(labels, labels.size(), L" + ");
}

std::wstring JoinWaitPath(const std::vector<std::wstring>& names)
{
  std::wstring out;
  for (const auto& name : names) {
    if (!out.empty()) {
      out += L", ";
    }
    out += name;
  }
  return out;
}

std::wstring DescribeBlockedPlugin(const MainThreadWaitInfo& wait)
{
  return wait.blocked_mod_name.empty() ? wait.blocked_module : (wait.blocked_mod_name + L" (" + wait.blocked_module + L")");
}

// "was waiting inside" / "코드 안에서 대기 중이었습니다" and so on.
std::wstring DescribeBlockedState(const MainThreadWaitInfo& wait, bool en)
{
  if (wait.blocked_state == "waiting_in_plugin") {
    return en ? L"was waiting inside" : L" 코드 안에서 대기 중이었습니다";
  }
  if (wait.blocked_state == "engine_call_from_plugin") {
    return en ? L"was running game engine code called by" : L"이(가) 부른 게임 엔진 코드를 실행하고 있었습니다";
  }
  return en ? L"was running the code of" : L" 코드를 실행하고 있었습니다";
}

// ADR-0009: the main thread sat in a wait the plugins on its stack did not
// perform, so the sentence says where it waited instead of naming a module.
std::wstring DescribeBystanderWait(const AnalysisResult& r, i18n::Language lang, bool en)
{
  const auto& wait = r.main_thread_wait;
  const std::wstring conf = ConfidenceText(lang, i18n::ConfidenceLevel::kLow);
  const std::wstring api = wait.wait_api.empty() ? std::wstring{} : (L", " + wait.wait_api);
  if (wait.kind == "graphics_driver_wait") {
    return en
      ? (L"The game main thread was waiting inside the graphics driver (" + wait.waiting_module + api +
          L"). That points to the GPU side - the driver itself, graphics mods (ENB, ReShade, upscalers, shader mods) or "
          L"video memory - rather than to modules found on the stack. (Confidence: " + conf + L")")
      : (L"게임 메인 스레드가 그래픽 드라이버(" + wait.waiting_module + api +
          L") 안에서 대기 중이었습니다. 스택에서 보이는 모듈보다는 GPU 쪽(드라이버 자체, ENB·ReShade·업스케일러·셰이더 모드 같은 "
          L"그래픽 모드, 비디오 메모리)을 가리킵니다. (신뢰도: " + conf + L")");
  }
  const std::wstring path = wait.path_modules.empty() ? std::wstring{} : JoinWaitPath(wait.path_modules);
  if (HasBlockedGameThread(wait)) {
    const std::wstring medium = ConfidenceText(lang, i18n::ConfidenceLevel::kMedium);
    const std::wstring who = DescribeBlockedPlugin(wait);
    const std::wstring tid = std::to_wstring(wait.blocked_thread_tid);
    return en
      ? (L"The game main thread was waiting inside the game engine (" + wait.waiting_module + api +
          L") for another game thread to finish, and that thread (tid " + tid + L") " + DescribeBlockedState(wait, en) +
          L" " + who + L" (top: " + wait.blocked_top + L"). That plugin is the main lead for this freeze. (Confidence: " +
          medium + L")")
      : (L"게임 메인 스레드는 게임 엔진(" + wait.waiting_module + api + L") 안에서 다른 게임 스레드의 작업이 끝나기를 기다리고 "
          L"있었고, 그 스레드(tid " + tid + L")는 " + who + DescribeBlockedState(wait, en) + L"(맨 위: " + wait.blocked_top +
          L"). 이 플러그인이 이번 프리징의 가장 유력한 단서입니다. (신뢰도: " + medium + L")");
  }
  if (wait.engine_wait_detail == "gpu_query_poll") {
    return en
      ? (L"The game main thread was waiting for the GPU: the engine was polling a Direct3D query "
          L"(ID3D11DeviceContext::GetData) in a Sleep loop (" + wait.waiting_module + api +
          L") until the GPU finished its work. That points to the GPU side - the driver, graphics mods (ENB, ReShade, "
          L"upscalers, shader mods) or video memory - rather than to the plugins on the stack" +
          (path.empty() ? std::wstring{} : (L" (" + path + L")")) + L". (Confidence: " + conf + L")")
      : (L"게임 메인 스레드는 GPU를 기다리고 있었습니다. 엔진이 GPU 작업이 끝나기를 Sleep 루프에서 Direct3D 쿼리"
          L"(ID3D11DeviceContext::GetData)로 확인하던 중이었습니다(" + wait.waiting_module + api +
          L"). 스택의 플러그인" + (path.empty() ? std::wstring{} : (L"(" + path + L")")) +
          L"보다는 GPU 쪽(드라이버, ENB·ReShade·업스케일러·셰이더 모드 같은 그래픽 모드, 비디오 메모리)을 가리킵니다. (신뢰도: " +
          conf + L")");
  }
  return en
    ? (L"The game main thread was not running mod code: it was waiting inside the game engine (" + wait.waiting_module +
        api + L")." +
        (path.empty() ? std::wstring{}
                      : (L" Plugins further down its stack (" + path + L") are on the call path, not shown to be the cause.")) +
        L" This dump alone cannot tell what the engine was waiting for. (Confidence: " + conf + L")")
    : (L"게임 메인 스레드는 모드 코드를 실행하던 것이 아니라 게임 엔진(" + wait.waiting_module + api +
        L") 안에서 대기 중이었습니다." +
        (path.empty() ? std::wstring{}
                      : (L" 스택 아래쪽 플러그인(" + path + L")은 호출 경로일 뿐 원인으로 확인된 것이 아닙니다.")) +
        L" 엔진이 무엇을 기다렸는지는 이 덤프만으로 알 수 없습니다. (신뢰도: " + conf + L")");
}

// WCT named the thread holding the lock the main thread waits on, so the
// sentence points at that thread, not at the module the main thread waited in.
std::wstring DescribeLockOwnerWait(const AnalysisResult& r, i18n::Language lang, bool en)
{
  const auto& wait = r.main_thread_wait;
  const auto& owner = r.main_thread_lock_owner;
  const std::wstring conf =
    ConfidenceText(lang, owner.confirmed ? i18n::ConfidenceLevel::kMedium : i18n::ConfidenceLevel::kLow);
  const std::wstring where = wait.waiting_module.empty()
    ? wait.wait_api
    : (wait.waiting_module + (wait.wait_api.empty() ? std::wstring{} : (L", " + wait.wait_api)));
  const std::wstring path = wait.path_modules.empty() ? std::wstring{} : JoinWaitPath(wait.path_modules);
  return en
    ? (L"The game main thread was waiting for a " + DescribeLockType(owner, en) + L" held by " +
        DescribeLockOwnerThread(owner, en) + (where.empty() ? std::wstring{} : (L" (in " + where + L")")) +
        L". The stall is on the thread holding the lock." +
        (path.empty() ? std::wstring{}
                      : (L" Plugins further down the main thread's stack (" + path +
                          L") are on the call path, not shown to be the cause.")) +
        (owner.confirmed ? std::wstring{} : std::wstring(L" Only one WCT pass named this owner.")) +
        L" (Confidence: " + conf + L")")
    : (L"게임 메인 스레드는 " + DescribeLockOwnerThread(owner, en) + L"이(가) 쥐고 있는 " + DescribeLockType(owner, en) +
        L" 잠금을 기다리고 있었습니다" + (where.empty() ? std::wstring{} : (L"(대기 위치: " + where + L")")) +
        L". 멈춘 곳은 그 잠금을 쥔 스레드 쪽입니다." +
        (path.empty() ? std::wstring{}
                      : (L" 메인 스레드 스택 아래쪽 플러그인(" + path + L")은 호출 경로일 뿐 원인으로 확인된 것이 아닙니다.")) +
        (owner.confirmed ? std::wstring{} : std::wstring(L" 이 소유 스레드는 WCT 한 번에서만 확인됐습니다.")) +
        L" (신뢰도: " + conf + L")");
}

// crash_logger_thread_dump_deadlock (FreezeCandidateConsensus): Crash
// Logger's thread dump, not a mod, stopped the game.
std::wstring DescribeCrashLoggerThreadDumpDeadlock(const AnalysisResult& r, i18n::Language lang, bool en)
{
  const auto& owner = r.main_thread_lock_owner;
  const std::wstring conf =
    ConfidenceText(lang, CrashLoggerThreadDumpDeadlockConfidence(owner, r.crash_logger_thread_dump_deadlock));
  const auto& path = r.main_thread_wait.path_modules;
  const std::wstring mods = path.empty() ? std::wstring{} : JoinWaitPath(path);
  return en
    ? (L"Crash Logger's thread dump deadlocked and froze the game. It suspended " + DescribeLockOwnerThread(owner, en) +
        L" and then loaded PDB symbols, which needed a lock that suspended thread held, so it never resumed it. "
        L"The main thread was only waiting for a " + DescribeLockType(owner, en) + L" that thread holds; the mods on "
        L"the main thread's stack" + (mods.empty() ? std::wstring{} : (L" (" + mods + L")")) +
        L" are not the cause. (Confidence: " + conf + L")")
    : (L"Crash Logger의 스레드 덤프가 교착 상태(deadlock)에 빠져 게임이 멈췄습니다. 스레드 덤프가 " +
        DescribeLockOwnerThread(owner, en) +
        L"을(를) 일시 정지시킨 채 PDB(디버그 심볼)를 읽으려 했는데, 그 작업에 필요한 잠금을 정지된 스레드가 쥐고 있어서 "
        L"덤프가 끝나지 못했고 그 스레드도 재개되지 못했습니다. 메인 스레드는 그 스레드가 쥔 " +
        DescribeLockType(owner, en) + L" 잠금을 기다리다 멈췄을 뿐이며, 메인 스레드 스택의 모드" +
        (mods.empty() ? std::wstring{} : (L"(" + mods + L")")) + L"는 원인이 아닙니다. (신뢰도: " + conf + L")");
}

// A WCT cycle or a loader stall explains the freeze (FreezeCandidateConsensus).
// A stack module is named only when it does not come from a main thread that
// sat in an engine or driver wait (ADR-0009).
std::wstring DescribeExplainedStall(
  const AnalysisResult& r,
  i18n::Language lang,
  bool en,
  const std::wstring& suspectWho,
  const std::wstring& suspectConf)
{
  const auto level = r.freeze_analysis.confidence_level == i18n::ConfidenceLevel::kUnknown
    ? i18n::ConfidenceLevel::kMedium
    : r.freeze_analysis.confidence_level;
  const std::wstring conf = ConfidenceText(lang, level);
  const bool nameSuspect =
    !suspectWho.empty() && !(IsBystanderWait(r.main_thread_wait) && r.suspects_from_main_thread);
  if (r.freeze_analysis.state_id == "deadlock_likely") {
    const std::wstring lead = nameSuspect
      ? (en ? (L" The stack of a waiting thread points to " + suspectWho + L" (" + suspectConf + L").")
            : (L" 기다리던 스레드의 스택은 " + suspectWho + L"을(를) 가리킵니다(" + suspectConf + L")."))
      : std::wstring{};
    return en
      ? (L"Windows Wait Chain Traversal found threads waiting on each other in a cycle, so a deadlock is the main "
          L"interpretation." + lead + L" (Confidence: " + conf + L")")
      : (L"Windows 대기 체인(WCT)에서 스레드끼리 서로를 기다리는 순환이 확인되어 데드락이 가장 유력한 해석입니다." + lead +
          L" (신뢰도: " + conf + L")");
  }
  const std::wstring lead = nameSuspect
    ? (en ? (L" Stack candidate: " + suspectWho + L" (" + suspectConf + L").")
          : (L" 스택 후보: " + suspectWho + L"(" + suspectConf + L")."))
    : std::wstring{};
  return en
    ? (L"It froze while loading, so a loader stall (meshes, animations, physics or precaching) is the main "
        L"interpretation rather than a generic DLL." + lead + L" (Confidence: " + conf + L")")
    : (L"로딩 중에 멈춰서, 일반 DLL보다 로딩 정지(메시·애니메이션·물리·프리캐시)가 가장 유력한 해석입니다." + lead +
        L" (신뢰도: " + conf + L")");
}

std::wstring DescribeModalDialogWait(const ModalDialogWaitInfo& modal, i18n::Language lang, bool en)
{
  const auto level = (modal.window_evidence && modal.stack_evidence)
    ? i18n::ConfidenceLevel::kHigh
    : i18n::ConfidenceLevel::kMedium;
  const std::wstring conf = ConfidenceText(lang, level);
  const std::wstring title = modal.dialog_title.empty() ? std::wstring{} : (L" \"" + modal.dialog_title + L"\"");
  const std::wstring caller = !modal.caller_inferred_mod_name.empty()
    ? (modal.caller_inferred_mod_name + L" (" + modal.caller_module_filename + L")")
    : modal.caller_module_filename;

  std::wstring owner;
  if (modal.caller_kind == "plugin" && !caller.empty()) {
    owner = en ? (L" It was opened by " + caller + L".") : (L" 대화상자를 연 모듈: " + caller + L".");
  } else if (modal.caller_kind == "skse_runtime") {
    owner = en
      ? L" It was opened by the SKSE runtime itself, which usually reports a plugin load or version error."
      : L" 대화상자를 연 주체는 SKSE 런타임 자체이며, 보통 플러그인 로드/버전 오류를 알리는 창입니다.";
  } else if (modal.caller_kind == "hook_framework" && !caller.empty()) {
    owner = en ? (L" It was opened by hook framework " + caller + L".") : (L" 대화상자를 연 모듈: 훅 프레임워크 " + caller + L".");
  } else if (modal.caller_kind == "game_exe") {
    owner = en ? L" It was opened by the game executable." : L" 대화상자를 연 주체는 게임 실행 파일입니다.";
  }
  if (modal.address_library_issue == "plugin_incompatible") {
    owner += en
      ? L" The dialog says this plugin could not use the Address Library of this game version (format or address missing)."
      : L" 대화상자 내용으로 보아 이 플러그인이 현재 게임 버전의 Address Library를 쓰지 못했습니다(형식이 맞지 않거나 필요한 주소가 없음).";
  } else if (modal.address_library_issue == "address_library_missing") {
    owner += en
      ? L" The dialog says the plugin could not find the Address Library file for this game version."
      : L" 대화상자 내용으로 보아 플러그인이 현재 게임 버전용 Address Library 파일을 찾지 못했습니다.";
  }

  return en
    ? (L"The game main thread is waiting in a modal dialog" + title +
        L", so the game stays stopped until that dialog is closed." + owner +
        L" Look for an error window behind the game window; modules on the waiting stack are not the cause. (Confidence: " +
        conf + L")")
    : (L"게임 메인 스레드가 modal 대화상자" + title +
        L"에서 대기 중이어서, 이 창을 닫기 전까지 게임이 멈춰 있습니다." + owner +
        L" 게임 창 뒤에 오류 창이 있는지 확인하세요. 대기 중인 스택에 보이는 모듈은 원인이 아닙니다. (신뢰도: " +
        conf + L")");
}

}  // namespace

std::wstring BuildSummarySentence(const AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx)
{
  const bool en = ctx.en;
  const bool isSnapshotLike = ctx.isSnapshotLike;
  const bool isHangLike = ctx.isHangLike;
  const bool isManualCapture = ctx.isManualCapture;
  const bool hasModule = ctx.hasModule;
  const bool isSystem = ctx.isSystem;
  const bool isGameExe = ctx.isGameExe;
  const auto& wct = ctx.wct;
  const std::wstring& suspectBasis = ctx.suspectBasis;

  const bool hasSuspect = !r.suspects.empty();
  const SuspectItem* topSuspect = hasSuspect ? &r.suspects[0] : nullptr;
  const SuspectItem* firstNonHookSuspect = nullptr;
  if (hasSuspect) {
    for (const auto& s : r.suspects) {
      if (IsActionableSuspect(s)) {
        firstNonHookSuspect = &s;
        break;
      }
    }
  }

  auto suspectDisplayName = [&](const SuspectItem& s) {
    if (!s.inferred_mod_name.empty()) {
      return s.inferred_mod_name + L" (" + s.module_filename + L")";
    }
    return s.module_filename;
  };

  std::wstring suspectWho;
  std::wstring suspectConf;
  if (topSuspect) {
    suspectConf = topSuspect->confidence.empty() ? ConfidenceText(lang, i18n::ConfidenceLevel::kMedium) : topSuspect->confidence;
    suspectWho = suspectDisplayName(*topSuspect);
  }

  std::wstring nonHookSuspectWho;
  std::wstring nonHookSuspectConf;
  if (firstNonHookSuspect) {
    nonHookSuspectConf =
      firstNonHookSuspect->confidence.empty() ? ConfidenceText(lang, i18n::ConfidenceLevel::kMedium) : firstNonHookSuspect->confidence;
    nonHookSuspectWho = suspectDisplayName(*firstNonHookSuspect);
  }
  const bool hasNonHookSuspect = (firstNonHookSuspect != nullptr);
  const bool topSuspectIsHookFramework = (topSuspect != nullptr) && minidump::IsKnownHookFramework(topSuspect->module_filename);
  const bool topSuspectIsSystem = (topSuspect != nullptr) &&
    minidump::IsSystemishModuleAt(topSuspect->module_filename, topSuspect->module_path);
  const ActionableCandidate* topCandidate = !r.actionable_candidates.empty() ? &r.actionable_candidates[0] : nullptr;
  const ActionableCandidate* secondCandidate = (r.actionable_candidates.size() > 1u) ? &r.actionable_candidates[1] : nullptr;
  const std::wstring topCandidateConf = topCandidate && !topCandidate->confidence.empty()
    ? topCandidate->confidence
    : ConfidenceText(lang, topCandidate ? topCandidate->confidence_level : i18n::ConfidenceLevel::kUnknown);
  const bool topCandidateHasFrame = topCandidate && CandidateHasFrameSupport(*topCandidate);
  const bool topCandidateHasObjectRef = topCandidate && CandidateHasFamily(*topCandidate, "crash_logger_object_ref");
  const bool topCandidateBackedByFrame =
    topCandidate &&
    ((r.crash_logger_direct_fault_eligible &&
      CandidateMatchesModule(*topCandidate, r.crash_logger_direct_fault_module)) ||
     (r.crash_logger_first_actionable_probable_eligible &&
      CandidateMatchesModule(*topCandidate, r.crash_logger_first_actionable_probable_module)) ||
     (r.crash_logger_probable_streak_eligible &&
      CandidateMatchesModule(*topCandidate, r.crash_logger_probable_streak_module)));
  const bool topCandidateIsActionableNonHookDll =
    topCandidate &&
    !topCandidate->module_filename.empty() &&
    !minidump::IsKnownHookFramework(topCandidate->module_filename) &&
    !DumpModuleIsSystemish(r, topCandidate->module_filename) &&
    !minidump::IsGameExeModule(topCandidate->module_filename);

  std::wstring who;
  if (!r.inferred_mod_name.empty()) {
    who = r.inferred_mod_name + L" (" + r.fault_module_filename + L")";
  } else if (!r.fault_module_filename.empty()) {
    who = r.fault_module_filename;
  } else {
    who = en ? L"(unknown)" : L"(알 수 없음)";
  }

  std::wstring summary;

  if (ctx.isFilteredCleanExit && r.clean_exit_filter_context == "late_recovery") {
    // The heartbeat shows the main thread kept running, not that the faulting
    // thread recovered, so this stays below High.
    summary = en
      ? L"Validated helper metadata matches this preserved dump. The game kept running for over a minute after this exception was captured, so it is not classified as a CTD. If the game closed shortly after, review this dump anyway. (Confidence: Medium)"
      : L"검증된 헬퍼 메타데이터가 이 보존 덤프와 일치합니다. 이 예외가 잡힌 뒤에도 게임이 1분 넘게 계속 실행됐으므로 CTD로 분류하지 않습니다. 그 직후 게임이 꺼졌다면 이 덤프도 확인해 보세요. (신뢰도: 중간)";
  } else if (ctx.isFilteredCleanExit) {
    summary = en
      ? L"Validated clean-exit quarantine metadata matches this preserved dump. The process exited with code 0, so this is not classified as a CTD. (Confidence: High)"
      : L"검증된 정상 종료 격리 메타데이터가 이 보존 덤프와 일치합니다. 프로세스가 종료 코드 0으로 끝났으므로 CTD로 분류하지 않습니다. (신뢰도: 높음)";
  } else if (isSnapshotLike) {
    summary = isManualCapture
      ? (en
          ? L"Looks like a manual snapshot. This alone does not prove a problem. (Confidence: High)"
          : L"수동 캡처 스냅샷으로 보입니다. 이 결과만으로 '문제가 있다'고 단정할 수 없습니다. (신뢰도: 높음)")
      : (en
          ? L"Looks like a snapshot dump (not a crash/hang). Useful for state inspection, not root cause. (Confidence: High)"
          : L"스냅샷 덤프(크래시/행 아님)로 보입니다. 원인 판정용이 아니라 '상태 확인'에 유용합니다. (신뢰도: 높음)");
  } else if (r.hook_stub_race.detected) {
    // ADR-0016: the dump shows the hook stub complete where the CPU ran int3.
    const auto& race = r.hook_stub_race;
    const std::wstring owner = race.target_mod_name.empty()
      ? race.target_module
      : (race.target_mod_name + L" (" + race.target_module + L")");
    summary = en
      ? (L"A hook into " + owner + L" was being installed while the game ran: another thread took the patched "
         L"call before the jump code was complete and hit its unwritten bytes (int3). The mod that jump goes to "
         L"usually installed it. (Confidence: High)")
      : (owner + L"(으)로 가는 후킹이 게임 실행 중에 설치되던 순간, 다른 스레드가 바뀐 호출을 지나다 아직 다 써지지 않은 "
         L"점프 코드(int3)를 실행했습니다. 그 점프가 가는 모드가 대개 후킹을 설치한 모드입니다. (신뢰도: 높음)");
  } else if (hasModule && !isSystem && !isGameExe && ctx.isHookFramework) {
    // CrashLogger is on the hook-framework list, but a fault inside it is a
    // secondary fault from its memory probes while it reported another crash
    // (the [Interpretation] recommendation says the same).
    const std::wstring faultLower = minidump::WideLower(r.fault_module_filename);
    const bool faultIsCrashLogger = faultLower == L"crashlogger.dll" || faultLower == L"crashloggersse.dll";
    const std::wstring frameworkLabel = faultIsCrashLogger
      ? (en ? L"Crash Logger itself" : L"Crash Logger 자체")
      : (en ? L"known hook framework" : L"알려진 훅 프레임워크");
    if (topCandidateIsActionableNonHookDll && topCandidateBackedByFrame) {
      const auto candidateName = DescribeCandidate(*topCandidate);
      const auto frameSupport = DescribeFrameSupport(r, *topCandidate, en);
      summary = en
        ? (L"Crash is reported in " + who + L" (" + frameworkLabel + L"), but the paired CrashLogger log's " +
            frameSupport + L" points to DLL candidate " + candidateName +
            L". Check this candidate first, but do not treat it as a confirmed root cause. (Confidence: " +
            topCandidateConf + L")")
        : (L"크래시 덤프 위치는 " + who + L"(" + frameworkLabel + L")이지만, 짝지어진 CrashLogger 로그의 " +
            frameSupport + L"이 DLL 후보 " + candidateName +
            L"을(를) 가리킵니다. 이 후보를 먼저 점검하되 확정 원인으로 단정하지 마세요. (신뢰도: " +
            topCandidateConf + L")");
    } else if (hasNonHookSuspect && !nonHookSuspectWho.empty()) {
      summary = en
        ? (L"Crash is reported in " + who + L" (" + frameworkLabel + L"), but " + suspectBasis + L" points to " + nonHookSuspectWho +
            L". (Confidence: " + nonHookSuspectConf + L")")
        : (L"크래시 위치가 " + who + L"(" + frameworkLabel + L")로 보고되었지만, " + suspectBasis + L"에서는 " + nonHookSuspectWho +
            L" 가 유력합니다. (신뢰도: " + nonHookSuspectConf + L")");
    } else if (faultIsCrashLogger) {
      summary = en
        ? (L"Crash is reported in " + who + L", most likely a secondary fault from Crash Logger probing memory while it reported another crash. "
           L"Judge the crash by the original exception and call stack in the paired Crash Logger log. (Confidence: Low)")
        : (L"크래시 위치가 " + who + L"로 보고되었지만, Crash Logger가 다른 크래시를 기록하면서 메모리를 조사하다 낸 2차 예외일 가능성이 큽니다. "
           L"짝지어진 Crash Logger 로그의 원래 예외와 콜스택을 기준으로 판단하세요. (신뢰도: 낮음)");
    } else {
      summary = en
        ? (L"Crash is reported in " + who + L" (known hook framework). This is often a victim location, so avoid treating it as a standalone root cause. (Confidence: Low)")
        : (L"크래시 위치가 " + who + L"(알려진 훅 프레임워크)로 보고되었습니다. 이 경우 피해 위치로 잡히는 일이 많아 단독 원인으로 단정하기 어렵습니다. (신뢰도: 낮음)");
    }
  } else if (hasModule && !isSystem && !isGameExe) {
    const bool topCandidateMatchesFaultModule =
      topCandidate &&
      CandidateMatchesModule(*topCandidate, r.fault_module_filename);
    const bool weakFaultLocationTopCandidate =
      topCandidate &&
      IsWeakFaultLocationActionableCandidate(r, *topCandidate, ctx);
    const bool topCandidateHasStandaloneCallstack =
      topCandidate &&
      CandidateHasStandaloneCallstackSupport(r, *topCandidate);
    const bool topCandidateHasFirstChanceSupport =
      topCandidate &&
      CandidateHasScorableFirstChanceSupport(r, *topCandidate);
    const bool topCandidateHasHistorySupport =
      topCandidate &&
      CandidateHasScorableHistorySupport(r, *topCandidate);
    const bool topCandidateHasResourceSupport =
      topCandidate &&
      CandidateHasResourceSupport(*topCandidate);
    const bool topCandidateHasObjectRefSupport =
      topCandidate &&
      CandidateHasObjectRefSupport(*topCandidate);

    if (topCandidateMatchesFaultModule && weakFaultLocationTopCandidate) {
      const auto frameSupport = DescribeFrameSupport(r, *topCandidate, en);
      const bool nearby = HasNearbyProbableDll(r);
      const std::wstring compareEn = nearby
        ? std::wstring(L" Compare nearby probable DLLs too.")
        : std::wstring(L" Crash Logger names no other DLL nearby.");
      const std::wstring compareKo = nearby
        ? std::wstring(L" Crash Logger가 함께 지목한 근처 DLL도 비교하세요.")
        : std::wstring(L" Crash Logger가 함께 지목한 다른 DLL은 없습니다.");
      if (topCandidateHasStandaloneCallstack) {
        summary = en
          ? (L"Crash is reported in " + who + L", and " + frameSupport +
              L" plus the same-dump stack both land in this DLL, but that still reflects the current fault-location cluster rather than an independent confirmation." +
              compareEn + L" (Confidence: " + topCandidateConf + L")")
          : (L"크래시 위치가 " + who + L"로 보고되었고, " + frameSupport +
              L"과 같은 덤프의 스택이 모두 이 DLL을 가리키지만, 둘 다 예외가 난 위치를 보는 것이라 독립 검증은 아닙니다." +
              compareKo + L" (신뢰도: " + topCandidateConf + L")");
      } else {
        summary = en
          ? (L"Crash is reported in " + who + L", but current actionable support still stays on the fault-location DLL." +
              (nearby ? std::wstring(L" Compare nearby probable DLLs before treating it as the root cause.") : compareEn) +
              L" (Confidence: " + topCandidateConf + L")")
          : (L"크래시 위치가 " + who + L"로 보고되었지만, 지금 근거는 예외가 난 DLL이라는 것뿐입니다." +
              (nearby ? std::wstring(L" 근본 원인으로 단정하기 전에 Crash Logger가 함께 지목한 근처 DLL도 비교하세요.") : compareKo) +
              L" (신뢰도: " + topCandidateConf + L")");
      }
    } else if (topCandidate && topCandidate->status_id == "conflicting" && secondCandidate) {
      const auto firstName = DescribeCandidate(*topCandidate);
      const auto secondName = DescribeCandidate(*secondCandidate);
      const auto firstFamilies = JoinCandidateFamilies(*topCandidate, en);
      const auto secondFamilies = JoinCandidateFamilies(*secondCandidate, en);
      summary = en
        ? (L"Crash is reported in " + who + L", but actionable signals disagree between " + firstName + L" (" + firstFamilies +
            L") and " + secondName + L" (" + secondFamilies +
            L"). Treat the faulting DLL as a candidate, not a confirmed root cause. (Confidence: " + topCandidateConf + L")")
        : (L"크래시 위치가 " + who + L"로 보고되었지만, 실행 우선 신호가 " + firstName + L" (" + firstFamilies +
            L") 과(와) " + secondName + L" (" + secondFamilies +
            L") 사이에서 갈립니다. faulting DLL을 단일 확정 원인으로 보지 말고 후보 중 하나로 보세요. (신뢰도: " + topCandidateConf + L")");
    } else if (topCandidateMatchesFaultModule && topCandidate->status_id == "cross_validated") {
      const auto families = JoinCandidateFamilies(*topCandidate, en);
      if (topCandidateBackedByFrame) {
        const auto frameSupport = DescribeFrameSupport(r, *topCandidate, en);
        summary = en
          ? (L"Top suspect: " + who + L" — " + frameSupport + L" and another signal agree on this DLL (" + families +
              L"). (Confidence: " + topCandidateConf + L")")
          : (L"유력 후보: " + who + L" — " + frameSupport + L"과 다른 신호가 이 DLL을 함께 가리킵니다. (" + families +
              L", 신뢰도: " + topCandidateConf + L")");
      } else {
        summary = en
          ? (L"Top suspect: " + who + L" — cross-validated actionable signals agree on this DLL (" + families +
              L"). (Confidence: " + topCandidateConf + L")")
          : (L"유력 후보: " + who + L" — 교차검증된 실행 우선 신호가 이 DLL에서 합의합니다. (" + families +
              L", 신뢰도: " + topCandidateConf + L")");
      }
    } else if (topCandidateMatchesFaultModule && topCandidate->status_id == "related") {
      if (topCandidateHasStandaloneCallstack) {
        summary = en
          ? (L"Crash is reported in " + who + L", and Tullius callstack first also points to this DLL (actionable stack). "
              L"This is stronger than a standalone frame clue, but it can still be a victim location. (Confidence: " + topCandidateConf + L")")
          : (L"크래시 위치가 " + who + L"로 보고되었고, Tullius 콜스택도 이 DLL을 가리킵니다. "
              L"이는 프레임 단서 하나보다 강하지만, 여전히 피해 위치일 수 있습니다. (신뢰도: " + topCandidateConf + L")");
      } else if (topCandidateBackedByFrame) {
        const auto frameSupport = DescribeFrameSupport(r, *topCandidate, en);
        const auto families = JoinCandidateFamilies(*topCandidate, en);
        if (topCandidateHasFirstChanceSupport || topCandidateHasHistorySupport || topCandidateHasResourceSupport || topCandidateHasObjectRefSupport) {
          summary = en
            ? (L"Crash is reported in " + who + L", and " + frameSupport + L" points inside this DLL. Another supporting signal agrees (" +
                families + L"), but this can still be a victim location. (Confidence: " + topCandidateConf + L")")
            : (L"크래시 위치가 " + who + L"로 보고되었고, " + frameSupport + L"이 이 DLL 내부를 가리킵니다. 다른 보조 신호도 맞지만 (" +
                families + L"), 여전히 피해 위치일 수 있습니다. (신뢰도: " + topCandidateConf + L")");
        } else {
          summary = en
            ? (L"Crash is reported in " + who + L", and " + frameSupport +
                L" points inside this DLL, but no second independent signal agrees yet. This can still be a victim location. (Confidence: " + topCandidateConf + L")")
            : (L"크래시 위치가 " + who + L"로 보고되었고, " + frameSupport +
                L"이 이 DLL 내부를 가리키지만 이를 뒷받침하는 다른 독립 신호는 아직 없습니다. 여전히 피해 위치일 수 있습니다. (신뢰도: " + topCandidateConf + L")");
        }
      } else {
        const auto candidateName = DescribeCandidate(*topCandidate);
        const auto families = JoinCandidateFamilies(*topCandidate, en);
        summary = en
          ? (L"Crash is reported in " + who + L", and partial actionable support still points to this DLL candidate " + candidateName +
              L" (" + families + L"). Treat it as a candidate first, not a confirmed root cause. (Confidence: " + topCandidateConf + L")")
          : (L"크래시 위치가 " + who + L"로 보고되었고, 부분적인 실행 우선 신호도 이 DLL 후보 " + candidateName +
              L"을(를) 가리킵니다. (" + families + L", 우선 후보로 보되 확정 원인으로 단정하지 마세요. 신뢰도: " + topCandidateConf + L")");
      }
    } else if (topCandidateMatchesFaultModule && topCandidate->status_id == "reference_clue") {
      const auto candidateName = DescribeCandidate(*topCandidate);
      if (topCandidateBackedByFrame) {
        const auto frameSupport = DescribeFrameSupport(r, *topCandidate, en);
        summary = en
          ? (L"Crash is reported in " + who + L", and " + frameSupport +
              L" points inside this DLL, but no second independent signal agrees yet. Treat it as a clue first. (Confidence: " + topCandidateConf + L")")
          : (L"크래시 위치가 " + who + L"로 보고되었고, " + frameSupport +
              L"이 이 DLL 내부를 가리키지만 이를 뒷받침하는 다른 독립 신호는 아직 없습니다. 우선 단서로 보세요. (신뢰도: " + topCandidateConf + L")");
      } else {
        summary = en
          ? (L"Crash is reported in " + who + L", but the current actionable clue for this DLL candidate is still limited to " +
              candidateName + L". Treat it as a clue first. (Confidence: " + topCandidateConf + L")")
          : (L"크래시 위치가 " + who + L"로 보고되었지만, 현재 이 DLL 후보 " + candidateName +
              L" 에 대한 실행 우선 단서는 아직 제한적입니다. 우선 단서로 보세요. (신뢰도: " + topCandidateConf + L")");
      }
    } else if (topCandidate && topCandidate->status_id == "cross_validated") {
      const auto candidateName = DescribeCandidate(*topCandidate);
      const auto families = JoinCandidateFamilies(*topCandidate, en);
      summary = en
        ? (L"Crash is reported in " + who + L", but the strongest actionable candidate is " + candidateName +
            L" (" + families + L"). The faulting DLL can still be a victim location. (Confidence: " + topCandidateConf + L")")
        : (L"크래시 위치가 " + who + L"로 보고되었지만, 가장 강한 실행 우선 후보는 " + candidateName +
            L" 입니다. (" + families + L", 예외가 난 DLL은 피해 위치일 수 있습니다. 신뢰도: " + topCandidateConf + L")");
    } else if (topCandidate && topCandidate->status_id == "related") {
      const auto candidateName = DescribeCandidate(*topCandidate);
      const auto families = JoinCandidateFamilies(*topCandidate, en);
      if (topCandidateHasStandaloneCallstack) {
        summary = en
          ? (L"Crash is reported in " + who + L", but Tullius callstack first points to DLL candidate " + candidateName +
              L" (actionable stack). The faulting DLL can still be a victim location. (Confidence: " + topCandidateConf + L")")
          : (L"크래시 위치가 " + who + L"로 보고되었지만, Tullius 콜스택은 DLL 후보 " + candidateName +
              L"을(를) 가리킵니다. 예외가 난 DLL은 피해 위치일 수 있습니다. (신뢰도: " + topCandidateConf + L")");
      } else if (topCandidateBackedByFrame) {
        const auto frameSupport = DescribeFrameSupport(r, *topCandidate, en);
        summary = en
          ? (L"Crash is reported in " + who + L", but " + frameSupport + L" points to DLL candidate " + candidateName +
              L" (" + families + L"). The faulting DLL can still be a victim location. (Confidence: " + topCandidateConf + L")")
          : (L"크래시 위치가 " + who + L"로 보고되었지만, " + frameSupport + L"이 DLL 후보 " + candidateName +
              L"을(를) 가리킵니다. (" + families + L", 예외가 난 DLL은 피해 위치일 수 있습니다. 신뢰도: " + topCandidateConf + L")");
      } else {
        summary = en
          ? (L"Crash is reported in " + who + L", but partial actionable support points to DLL candidate " + candidateName +
              L" (" + families + L"). The faulting DLL can still be a victim location. (Confidence: " + topCandidateConf + L")")
          : (L"크래시 위치가 " + who + L"로 보고되었지만, 부분적인 실행 우선 신호가 DLL 후보 " + candidateName +
              L"을(를) 가리킵니다. (" + families + L", 예외가 난 DLL은 피해 위치일 수 있습니다. 신뢰도: " + topCandidateConf + L")");
      }
    } else {
      summary = en
        ? (L"Crash is reported in " + who +
            L", but this currently rests on fault-location evidence only. This can still be a victim location. (Confidence: Low)")
        : (L"크래시 위치가 " + who +
            L"로 보고되었지만, 지금은 예외가 난 위치라는 단서만 있습니다. 여전히 피해 위치일 수 있습니다. (신뢰도: 낮음)");
    }
  } else if (hasModule && isSystem) {
    const std::wstring place = FaultPlace(r, ctx);
    if (topCandidate && topCandidate->status_id == "cross_validated") {
      const auto candidateName = DescribeCandidate(*topCandidate);
      const auto families = JoinCandidateFamilies(*topCandidate, en);
      summary = en
        ? (L"Crash is reported in " + place + L", but the strongest actionable candidate is " + candidateName +
            L" (" + families + L"). (Confidence: " + topCandidateConf + L")")
        : (L"크래시가 " + place + L"에서 보고되었지만, 가장 강한 실행 우선 후보는 " + candidateName +
            L" 입니다. (" + families + L", 신뢰도: " + topCandidateConf + L")");
    } else if (topCandidate && topCandidateHasFrame && topCandidateBackedByFrame) {
      const auto candidateName = DescribeCandidate(*topCandidate);
      const auto frameSupport = DescribeFrameSupport(r, *topCandidate, en);
      const bool hasFirstChanceSupport = CandidateHasScorableFirstChanceSupport(r, *topCandidate);
      const bool hasHistorySupport = CandidateHasScorableHistorySupport(r, *topCandidate);
      const bool hasResourceSupport = CandidateHasResourceSupport(*topCandidate);
      summary = en
        ? (L"Crash is reported in " + place + L", but " + frameSupport + L" points to DLL candidate " + candidateName +
            L". This is stronger than an isolated object ref." +
            (hasFirstChanceSupport ? L" Repeated suspicious first-chance context also matched this candidate." : L"") +
            (hasHistorySupport ? L" Repeated crash bucket history also matched this candidate." : L"") +
            (hasResourceSupport ? L" Nearby resource provider activity also matched this candidate." : L"") +
            L" (Confidence: " + topCandidateConf + L")")
        : (L"크래시가 " + place + L"에서 보고되었지만, " + frameSupport + L"이 DLL 후보 " + candidateName +
            L"을(를) 가리킵니다. 이는 오브젝트 참조 하나보다 강한 신호입니다." +
            (hasFirstChanceSupport ? L" 반복된 의심스러운 선행 예외(first-chance)도 이 후보와 맞습니다." : L"") +
            (hasHistorySupport ? L" 반복 크래시 버킷 이력도 이 후보와 맞습니다." : L"") +
            (hasResourceSupport ? L" 근처 리소스를 제공한 모드도 이 후보와 맞습니다." : L"") +
            L" (신뢰도: " + topCandidateConf + L")");
    } else if (topCandidate && topCandidate->status_id == "related" && CandidateHasStandaloneCallstackSupport(r, *topCandidate)) {
      const auto candidateName = DescribeCandidate(*topCandidate);
      summary = en
        ? (L"Crash is reported in " + place + L", but Tullius callstack first points to DLL candidate " + candidateName +
            L" (actionable stack). This is stronger than stack scan only. (Confidence: " + topCandidateConf + L")")
        : (L"크래시가 " + place + L"에서 보고되었지만, Tullius 콜스택이 DLL 후보 " + candidateName +
            L"을(를) 가리킵니다. 이는 스택 스캔만으로 얻은 단서보다 강한 신호입니다. (신뢰도: " + topCandidateConf + L")");
    } else if (hasNonHookSuspect && !nonHookSuspectWho.empty()) {
      summary = en
        ? (L"Crash is reported in " + place + L", but " + suspectBasis + L" points to " + nonHookSuspectWho +
            L". (Confidence: " + nonHookSuspectConf + L")")
        : (L"크래시가 " + place + L"에서 보고되었지만, " + suspectBasis + L"에서는 " + nonHookSuspectWho +
            L" 가 유력합니다. (신뢰도: " + nonHookSuspectConf + L")");
    } else if (hasSuspect && !suspectWho.empty()) {
      summary = en
        ? (L"Crash is reported in " + place + L", and the top stack candidate is " + suspectWho +
            L". This can still be a victim location rather than the root cause. (Confidence: Low)")
        : (L"크래시가 " + place + L"에서 보고되었고, 스택 후보 1순위는 " + suspectWho +
            L" 입니다. 이 경우에도 실제 원인은 다른 DLL/모드일 수 있습니다. (신뢰도: 낮음)");
    } else if (r.exc_code == 0xE06D7363u && IsCrashLoggerConfirmedCppException(r)) {
      // Crash Logger saw it end the game, so it is no caught throw (crash test,
      // 2026-10-12); with no plugin on the throwing path there is no suspect.
      summary = en
        ? L"A C++ exception (0xE06D7363) that Crash Logger also logged as unhandled ended the game; the call stack "
          L"shows no plugin to suspect. (Confidence: Low)"
        : L"Crash Logger도 처리되지 않은 예외로 기록한 C++ 예외(0xE06D7363)로 게임이 종료되었습니다. 콜스택에서 "
          L"원인으로 볼 플러그인은 찾지 못했습니다. (신뢰도: 낮음)";
    } else if (r.exc_code == 0xE06D7363u) {
      // With the default CrashHookMode=1 the plugin records a C++ exception only
      // when nothing handled it (Crash Logger's report, the unhandled-exception
      // filter or abort()), so a missing Crash Logger log is no reason to doubt it.
      summary = en
        ? (L"Reported in " + place + L" with 0xE06D7363 (C++ exception). With the default CrashHookMode=1 a C++ exception "
           L"is recorded only when nothing handled it (Crash Logger's report, the unhandled-exception filter, or abort()), so "
           L"this usually ended the game; the call stack shows no plugin to suspect. (Confidence: Low)")
        : (L"0xE06D7363(C++ 예외)로 " + place + L"에서 보고되었습니다. 기본값 CrashHookMode=1에서는 어디에서도 처리되지 "
           L"않은 C++ 예외만(Crash Logger 보고, 처리되지 않은 예외 필터, abort()) 기록되므로 대개 이 예외로 게임이 "
           L"종료된 것입니다. 다만 콜스택에서 원인으로 볼 플러그인은 찾지 못했습니다. (신뢰도: 낮음)");
    } else if (!ctx.externalSoftware.empty()) {
      summary = en
        ? (L"Crash is reported in " + place + L", a program outside the game and its mods. Update or reinstall it, or "
            L"turn off its overlay, and retest; it can also be a victim of another DLL. (Confidence: Low)")
        : (L"크래시가 " + place + L"에서 보고되었습니다. 게임과 모드 밖의 프로그램입니다. 업데이트·재설치하거나 "
            L"오버레이를 끄고 다시 시험해 보세요. 다른 DLL 때문에 피해를 본 위치일 수도 있습니다. (신뢰도: 낮음)");
    } else {
      summary = en
        ? L"Crash is reported in a Windows system DLL. The real culprit may be another mod/DLL. (Confidence: Low)"
        : L"크래시가 Windows 시스템 DLL에서 보고되었습니다. 실제 원인은 다른 모드/DLL일 수 있습니다. (신뢰도: 낮음)";
    }
  } else if (hasModule && isGameExe) {
    // A cell or worldspace only says where the player was (see FirstCulpritObjectRef).
    const auto* culpritRef = FirstCulpritObjectRef(r);
    const bool hasObjectRefs = culpritRef != nullptr;
    const bool suspectsAreStackwalk = r.suspects_from_stackwalk;

    if (topCandidate && topCandidate->status_id == "conflicting" && secondCandidate) {
      const auto firstName = DescribeCandidate(*topCandidate);
      const auto secondName = DescribeCandidate(*secondCandidate);
      const auto firstFamilies = JoinCandidateFamilies(*topCandidate, en);
      const auto secondFamilies = JoinCandidateFamilies(*secondCandidate, en);
      summary = en
        ? (L"Crash is reported in the game executable. Signals disagree between " + firstName + L" (" + firstFamilies + L") and " + secondName +
            L" (" + secondFamilies + L")" +
            L", so treat them as conflicting candidates rather than a single root cause. (Confidence: " + topCandidateConf + L")")
        : (L"크래시 위치가 게임 본체(EXE)이며, " + firstName + L" (" + firstFamilies + L") 과(와) " + secondName +
            L" (" + secondFamilies + L")" +
            L" 사이에 강한 신호 충돌이 있습니다. 단일 원인으로 확정하지 말고 복수 후보로 보세요. (신뢰도: " + topCandidateConf + L")");
    } else if (topCandidate && topCandidate->status_id == "cross_validated") {
      const auto candidateName = DescribeCandidate(*topCandidate);
      const auto families = JoinCandidateFamilies(*topCandidate, en);
      summary = en
        ? (L"Crash is reported in the game executable. Cross-validated actionable candidate: " + candidateName +
            L" (" + families + L"). (Confidence: " + topCandidateConf + L")")
        : (L"크래시 위치가 게임 본체(EXE)이며, 교차검증된 실행 우선 후보는 " + candidateName +
            L" 입니다. (" + families + L", 신뢰도: " + topCandidateConf + L")");
    } else if (topCandidate && topCandidateHasFrame && topCandidateBackedByFrame) {
      const auto candidateName = DescribeCandidate(*topCandidate);
      const auto families = JoinCandidateFamilies(*topCandidate, en);
      const auto frameSupport = DescribeFrameSupport(r, *topCandidate, en);
      const bool hasFirstChanceSupport = CandidateHasScorableFirstChanceSupport(r, *topCandidate);
      const bool hasHistorySupport = CandidateHasScorableHistorySupport(r, *topCandidate);
      const bool hasResourceSupport = CandidateHasResourceSupport(*topCandidate);
      summary = en
        ? (L"Crash is reported in the game executable. " + frameSupport + L" points to DLL candidate " + candidateName +
            L" (" + families + L"). This is stronger than an isolated object ref." +
            (hasFirstChanceSupport ? L" Repeated suspicious first-chance context also matched this candidate." : L"") +
            (hasHistorySupport ? L" Repeated crash bucket history also matched this candidate." : L"") +
            (hasResourceSupport ? L" Nearby resource provider activity also matched this candidate." : L"") +
            L" (Confidence: " + topCandidateConf + L")")
        : (L"크래시 위치가 게임 본체(EXE)이며, " + frameSupport + L"이 DLL 후보 " + candidateName +
            L"을(를) 가리킵니다. (" + families + L", 오브젝트 참조 하나보다 강한 신호" +
            (hasFirstChanceSupport ? L", 반복된 의심스러운 선행 예외도 이 후보와 맞음" : L"") +
            (hasHistorySupport ? L", 반복 크래시 버킷 이력도 이 후보와 맞음" : L"") +
            (hasResourceSupport ? L", 근처 리소스를 제공한 모드도 이 후보와 맞음" : L"") +
            L", 신뢰도: " + topCandidateConf + L")");
    } else if (topCandidate && topCandidate->status_id == "related") {
      const auto candidateName = DescribeCandidate(*topCandidate);
      const auto families = JoinCandidateFamilies(*topCandidate, en);
      if (CandidateHasStandaloneCallstackSupport(r, *topCandidate)) {
        summary = en
          ? (L"Crash is reported in the game executable. Tullius callstack first points to DLL candidate " + candidateName +
              L" (actionable stack). This is stronger than stack scan only. (Confidence: " + topCandidateConf + L")")
          : (L"크래시 위치가 게임 본체(EXE)이며, Tullius 콜스택이 DLL 후보 " + candidateName +
              L"을(를) 가리킵니다. 이는 스택 스캔만으로 얻은 단서보다 강한 신호입니다. (신뢰도: " + topCandidateConf + L")");
      } else {
        summary = en
          ? (L"Crash is reported in the game executable. Actionable candidate: " + candidateName +
              L" (" + families + L"). (Confidence: " + topCandidateConf + L")")
          : (L"크래시 위치가 게임 본체(EXE)이며, 실행 우선 후보는 " + candidateName +
              L" 입니다. (" + families + L", 신뢰도: " + topCandidateConf + L")");
      }
    } else if (topCandidate && topCandidate->status_id == "reference_clue" && topCandidateHasFrame) {
      const auto candidateName = DescribeCandidate(*topCandidate);
      const auto frameSupport = DescribeFrameSupport(r, *topCandidate, en);
      summary = en
        ? (L"Crash is reported in the game executable, and " + frameSupport + L" points to DLL candidate " + candidateName +
            L". No second independent signal agrees yet. (Confidence: " + topCandidateConf + L")")
        : (L"크래시 위치가 게임 본체(EXE)이며, " + frameSupport + L"이 DLL 후보 " + candidateName +
            L"을(를) 가리킵니다. 이를 뒷받침하는 다른 독립 신호는 아직 없습니다. (신뢰도: " + topCandidateConf + L")");
    } else if (topCandidate && topCandidate->status_id == "reference_clue" && topCandidateHasObjectRef) {
      const auto candidateName = DescribeCandidate(*topCandidate);
      summary = en
        ? (L"Crash is reported in the game executable, and CrashLogger shows the game was processing " + candidateName +
            L". No second independent signal agrees yet. (Confidence: " + topCandidateConf + L")")
        : (L"크래시 위치가 게임 본체(EXE)이며, CrashLogger 기준으로 " + candidateName +
            L" 처리 중이었습니다. 이를 뒷받침하는 다른 독립 신호는 아직 없습니다. (신뢰도: " + topCandidateConf + L")");
    // Priority 1: stackwalk-based non-hook suspect (high reliability)
    } else if (hasNonHookSuspect && suspectsAreStackwalk && !nonHookSuspectWho.empty()) {
      if (hasObjectRefs) {
        const auto& topRef = *culpritRef;
        summary = en
          ? (L"Crash is reported in the game executable. Callstack points to " + nonHookSuspectWho +
              L", while processing " + topRef.esp_name + L" object. (Confidence: " + nonHookSuspectConf + L")")
          : (L"크래시 위치가 게임 본체(EXE)이며, 콜스택은 " + nonHookSuspectWho +
              L"을(를) 가리킵니다. " + topRef.esp_name + L"의 오브젝트 처리 중이었습니다. (신뢰도: " + nonHookSuspectConf + L")");
      } else {
        summary = en
          ? (L"Crash is reported in the game executable, but " + suspectBasis + L" points to " + nonHookSuspectWho +
              L". (Confidence: " + nonHookSuspectConf + L")")
          : (L"크래시 위치가 게임 본체(EXE)로 보고되었지만, " + suspectBasis + L"에서는 " + nonHookSuspectWho +
              L" 가 유력합니다. (신뢰도: " + nonHookSuspectConf + L")");
      }
    // Priority 2: CrashLogger ESP/ESM object refs (more actionable than stack scan)
    } else if (hasObjectRefs) {
      const auto& topRef = *culpritRef;
      if (hasSuspect && !suspectsAreStackwalk && !suspectWho.empty()) {
        // Stack scan suspect exists but is low reliability — mention as supplementary
        summary = en
          ? (L"Crash is reported in the game executable, and was processing an object from " + topRef.esp_name +
              L". Stack scan also shows " + suspectWho + L" nearby. (Confidence: Medium)")
          : (L"크래시 위치가 게임 본체(EXE)이며, " + topRef.esp_name +
              L"의 오브젝트를 처리 중이었습니다. 스택 스캔에서 " + suspectWho + L"도 인근에 감지되었습니다. (신뢰도: 중간)");
      } else {
        summary = en
          ? (L"Crash is reported in the game executable, and was processing an object from " + topRef.esp_name +
              L". (Confidence: Medium)")
          : (L"크래시 위치가 게임 본체(EXE)로 보고되었으며, " + topRef.esp_name +
              L"의 오브젝트를 처리 중이었습니다. (신뢰도: 중간)");
      }
    // Priority 3: hook framework suspect
    } else if (topSuspectIsHookFramework && !suspectWho.empty()) {
      summary = en
        ? (L"Crash is reported in the game executable, and the top stack candidate is " + suspectWho +
            L" (known hook framework). This DLL is often a victim frame owner, so avoid treating it as root cause by itself. (Confidence: Low)")
        : (L"크래시 위치가 게임 본체(EXE)로 보고되었고, 스택 후보 1순위는 " + suspectWho +
            L"(알려진 훅 프레임워크)입니다. 이 DLL은 피해 프레임 소유자로 자주 나타나므로 단독 원인으로 단정하기 어렵습니다. (신뢰도: 낮음)");
    // Priority 4: stackwalk-based suspect (no non-hook candidate)
    } else if (hasSuspect && suspectsAreStackwalk && !suspectWho.empty()) {
      summary = en
        ? (L"Crash is reported in the game executable, and " + suspectBasis + L" points to " + suspectWho +
            L". (Confidence: " + suspectConf + L")")
        : (L"크래시 위치가 게임 본체(EXE)로 보고되었고, " + suspectBasis + L"에서는 " + suspectWho +
            L" 가 유력합니다. (신뢰도: " + suspectConf + L")");
    // Priority 5: stack scan only (low reliability, no ESP refs)
    } else if (hasSuspect && !suspectsAreStackwalk && !suspectWho.empty()) {
      summary = en
        ? (L"Crash is reported in the game executable. Stack scan shows " + suspectWho +
            L" nearby, but this is a weak signal. (Confidence: Low)")
        : (L"크래시 위치가 게임 본체(EXE)로 보고되었습니다. 스택 스캔에서 " + suspectWho +
            L"이(가) 감지되었으나 신뢰도가 낮습니다. (신뢰도: 낮음)");
    } else {
      summary = en
        ? L"Crash is reported in the game executable. This dump alone cannot tell plugin data, a version mismatch, or a hook apart. (Confidence: Low)"
        : L"크래시 위치가 게임 본체(EXE)로 보고되었습니다. 플러그인 데이터 문제, 버전 불일치, 후킹 충돌 중 무엇인지 이 덤프만으로는 가릴 수 없습니다. (신뢰도: 낮음)";
    }
  } else {
    if (isHangLike) {
      std::wstring hangPrefix = en ? L"Likely a freeze/infinite loading." : L"프리징/무한로딩으로 추정됩니다.";
      if (wct && wct->has_capture && wct->thresholdSec > 0u) {
        const std::wstring kindW = ToWideAscii(wct->capture_kind);
        wchar_t hb[256]{};
        swprintf_s(
          hb,
          en ? L"Hang detected (capture=%s, heartbeatAge=%.1fs >= %us)." : L"프리징 감지(capture=%s, heartbeatAge=%.1fs >= %us).",
          kindW.c_str(),
          wct->secondsSinceHeartbeat,
          wct->thresholdSec);
        hangPrefix = hb;
      }
      if (!r.modal_dialog_wait.detected) {
        // ADR-0009: what the player had open (the Console in most of the
        // field freezes) and a recent long pause frame the stall.
        if (const auto gameState = DescribeGameStateAtCapture(r.blackbox_freeze_summary, en); !gameState.empty()) {
          hangPrefix += L" " + gameState;
        }
      }

      if (r.modal_dialog_wait.detected) {
        summary = hangPrefix + L" " + DescribeModalDialogWait(r.modal_dialog_wait, lang, en);
      } else if (r.crash_logger_thread_dump_deadlock.detected && r.main_thread_lock_owner.tid != 0u) {
        summary = hangPrefix + L" " + DescribeCrashLoggerThreadDumpDeadlock(r, lang, en);
      } else if (r.hang_thread_module_consensus.has_consensus) {
        const auto& consensus = r.hang_thread_module_consensus;
        summary = en
          ? (hangPrefix + L" " + consensus.module_filename + L" appears on the game main thread and " +
              std::to_wstring(consensus.matching_thread_count - 1u) +
              L" other non-progressing threads, supporting a module-level synchronization stall. WCT did not prove an OS lock cycle. (Confidence: Medium)")
          : (hangPrefix + L" 게임 메인 스레드와 진행이 멈춘 다른 " +
              std::to_wstring(consensus.matching_thread_count - 1u) + L"개 스레드에서 " +
              consensus.module_filename +
              L"이(가) 반복되어 모듈 내부 동기화 정지 가능성이 높습니다. WCT가 OS 잠금 사이클을 입증한 것은 아닙니다. (신뢰도: 중간)");
      } else if (r.freeze_analysis.state_id == "deadlock_likely" || r.freeze_analysis.state_id == "loader_stall_likely") {
        // The recommendations and the freeze reasons put these first; the
        // engine wait on the main thread does not overrule them.
        summary = hangPrefix + L" " + DescribeExplainedStall(r, lang, en, hasSuspect ? suspectWho : std::wstring{}, suspectConf);
      } else if (r.main_thread_lock_owner.tid != 0u) {
        summary = hangPrefix + L" " + DescribeLockOwnerWait(r, lang, en);
      } else if (IsBystanderWait(r.main_thread_wait) &&
                 (r.suspects_from_main_thread || !hasSuspect || HasBlockedGameThread(r.main_thread_wait))) {
        // Suspects from another thread (a WCT cycle member) are not the
        // main thread's call path; they keep their candidate below.
        summary = hangPrefix + L" " + DescribeBystanderWait(r, lang, en);
      } else if (hasSuspect && !suspectWho.empty()) {
        if (!r.suspects_from_stackwalk) {
          summary = en
            ? (hangPrefix + L" Main-thread pointer scan found " + suspectWho +
                L" nearby, but the formal stack walk did not identify an actionable module. Treat this as a weak investigation clue, not a confirmed cause. (Confidence: Low)")
            : (hangPrefix + L" 메인 스레드 포인터 스캔에서 " + suspectWho +
                L"이(가) 감지됐지만 정식 스택 분석으로는 실행 가능한 모듈을 특정하지 못했습니다. 확정 원인이 아닌 약한 조사 단서입니다. (신뢰도: 낮음)");
        } else if ((topSuspectIsHookFramework || topSuspectIsSystem) && hasNonHookSuspect && !nonHookSuspectWho.empty()) {
          summary = en
            ? (hangPrefix + L" Top stack candidate is likely a victim location; actionable candidate: " + nonHookSuspectWho
                + L" — based on " + suspectBasis + L" heuristic. (Confidence: " + nonHookSuspectConf + L")")
            : (hangPrefix + L" 스택 1순위 후보는 피해 위치일 가능성이 높아, 실행 우선 후보를 " + nonHookSuspectWho
                + L" 로 제시합니다. (" + suspectBasis + L" 기반, 신뢰도: " + nonHookSuspectConf + L")");
        } else if (topSuspectIsSystem) {
          summary = en
            ? (hangPrefix + L" Top stack candidate is a Windows system DLL (" + suspectWho +
                L"), which is often a waiting/victim location. Dump alone is not enough to identify a root cause. (Confidence: Low)")
            : (hangPrefix + L" 스택 후보 1순위가 Windows 시스템 DLL(" + suspectWho +
                L")입니다. 이 경우 대기/피해 위치일 가능성이 높아 덤프만으로 원인을 단정하기 어렵습니다. (신뢰도: 낮음)");
        } else {
          summary = en
            ? (hangPrefix + L" Candidate: " + suspectWho + L" — based on " + suspectBasis + L" heuristic. (Confidence: " + suspectConf + L")")
            : (hangPrefix + L" 후보: " + suspectWho + L" — " + suspectBasis + L" 기반 추정입니다. (신뢰도: " + suspectConf + L")");
        }
      } else {
        summary = en
          ? (hangPrefix + L" Dump alone isn't enough to identify a candidate. (Confidence: Low)")
          : (hangPrefix + L" 덤프만으로 후보를 특정하기 어렵습니다. (신뢰도: 낮음)");
      }
    } else {
      if (hasSuspect && !suspectWho.empty()) {
        summary = en
          ? (L"Top suspect: " + suspectWho + L" — based on " + suspectBasis + L" heuristic. (Confidence: " + suspectConf + L")")
          : (L"유력 후보: " + suspectWho + L" — " + suspectBasis + L" 기반 추정입니다. (신뢰도: " + suspectConf + L")");
      } else {
        summary = en
          ? L"Dump alone isn't enough to identify a top suspect. (Confidence: Low)"
          : L"덤프만으로 유력 후보를 특정하기 어렵습니다. (신뢰도: 낮음)";
      }
    }
  }

  if (r.signature_match.has_value()) {
    const auto& sig = *r.signature_match;
    if (sig.scope == "root_cause") {
      return en
        ? (L"Known root-cause pattern [" + ToWideAscii(sig.id) + L"]: " + sig.cause +
           L" (Root-cause confidence: " + sig.confidence + L"). " + summary)
        : (L"알려진 근본 원인 패턴 [" + ToWideAscii(sig.id) + L"]: " + sig.cause +
           L" (근본 원인 신뢰도: " + sig.confidence + L"). " + summary);
    }
    return en
      ? (L"Known crash mechanism [" + ToWideAscii(sig.id) + L"]: " + sig.cause +
         L" (Pattern-match confidence: " + sig.confidence + L"). Root-cause assessment: " + summary)
      : (L"알려진 크래시 메커니즘 [" + ToWideAscii(sig.id) + L"]: " + sig.cause +
         L" (패턴 일치 신뢰도: " + sig.confidence + L"). 근본 원인 후보 판단: " + summary);
  }

  return summary;
}

}  // namespace skydiag::dump_tool::internal
