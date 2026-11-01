#include "EvidenceBuilderPrivate.h"

#include <algorithm>
#include <cwchar>

#include "GameLockWait.h"
#include "MainThreadWait.h"
#include "MemorySnapshot.h"
#include "ProblematicModules.h"
#include "MinidumpUtil.h"
#include "SkyrimDiagShared.h"
#include "WctTypes.h"

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
  return candidate.module_filename;
}

std::wstring DescribeFamily(std::string_view familyId, bool en)
{
  if (familyId == "crash_logger_frame") {
    return en ? L"Crash Logger frame" : L"Crash Logger 프레임";
  }
  if (familyId == "crash_logger_object_ref") {
    return en ? L"CrashLogger object ref" : L"CrashLogger 오브젝트 참조";
  }
  if (familyId == "actionable_stack") {
    return en ? L"actionable stack" : L"실행 가능한 스택";
  }
  if (familyId == "resource_provider") {
    return en ? L"near resource provider" : L"근처 리소스 제공 모드";
  }
  if (familyId == "history_repeat") {
    return en ? L"history repeat" : L"버킷 반복";
  }
  if (familyId == "first_chance_context") {
    return en ? L"repeated first-chance context" : L"반복 선행 예외(first-chance)";
  }
  if (familyId == "hang_thread_group") {
    return en ? L"stable same-module thread group" : L"동일 모듈 정지 스레드 그룹";
  }
  if (familyId == "modal_dialog_owner") {
    return en ? L"modal dialog owner" : L"modal 대화상자 호출 모듈";
  }
  if (familyId == "hook_stub_owner") {
    return en ? L"hook stub owner" : L"후킹 점프 코드 주인";
  }
  if (familyId == "freed_hook_owner") {
    return en ? L"freed hook owner" : L"해제된 후킹 코드 주인";
  }
  return en ? L"other signal" : L"기타 신호";
}

std::wstring JoinFamilies(const ActionableCandidate& candidate, bool en)
{
  std::vector<std::wstring> labels;
  labels.reserve(candidate.supporting_families.size());
  for (const auto& family : candidate.supporting_families) {
    labels.push_back(DescribeFamily(family, en));
  }
  return labels.empty() ? (en ? L"limited evidence" : L"제한된 근거") : JoinList(labels, labels.size(), L" + ");
}

bool CandidateHasFamily(const ActionableCandidate& candidate, std::string_view familyId)
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

std::wstring DescribeCrashLoggerFrameSupport(const AnalysisResult& r, const ActionableCandidate& candidate, bool en)
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

bool HasDenseFirstChanceLoadingWindow(const FirstChanceSummary& summary)
{
  return summary.recent_count >= 3u &&
         summary.loading_window_count >= 2u &&
         summary.loading_window_count * 2u >= summary.recent_count;
}

std::wstring DescribeCaptureProfileStrength(const AnalysisResult& r, bool en)
{
  (void)en;  // Capture-profile field names are stable technical terms in both locales.
  std::vector<std::wstring> parts;
  if (r.incident_capture_profile_process_thread_data) {
    parts.emplace_back(L"process/thread data");
  }
  if (r.incident_capture_profile_full_memory_info) {
    parts.emplace_back(L"full memory info");
  }
  if (r.incident_capture_profile_module_headers) {
    parts.emplace_back(L"module headers");
  }
  if (r.incident_capture_profile_indirect_memory) {
    parts.emplace_back(L"indirect memory");
  }
  if (r.incident_capture_profile_ignore_inaccessible_memory) {
    parts.emplace_back(L"inaccessible-memory tolerance");
  }
  return JoinList(parts, parts.size(), L", ");
}

bool HasScorableFirstChanceContext(const FirstChanceSummary& summary)
{
  return summary.has_context &&
         !summary.recent_non_system_modules.empty() &&
         (summary.repeated_signature_count > 0u || HasDenseFirstChanceLoadingWindow(summary));
}

std::wstring DescribeFirstChanceContext(const FirstChanceSummary& summary, bool en)
{
  if (!HasScorableFirstChanceContext(summary)) {
    return {};
  }

  std::wstring detail = en
    ? L"Repeated suspicious first-chance context was observed"
    : L"반복 suspicious first-chance 문맥이 관측되었습니다";
  detail += en
    ? (L" (repeated=" + std::to_wstring(summary.repeated_signature_count) +
        L", loading-window=" + std::to_wstring(summary.loading_window_count) + L")")
    : (L" (반복=" + std::to_wstring(summary.repeated_signature_count) +
        L", 로딩창=" + std::to_wstring(summary.loading_window_count) + L")");
  if (!summary.recent_non_system_modules.empty()) {
    detail += L": " + JoinList(summary.recent_non_system_modules, 3, L", ");
  }
  return detail;
}

std::wstring DescribeRecaptureReasons(const AnalysisResult& r, bool en)
{
  std::vector<std::wstring> labels;
  labels.reserve(r.incident_recapture_reasons.size());
  for (const auto& reason : r.incident_recapture_reasons) {
    if (reason == "unknown_fault_module") {
      labels.push_back(en ? L"unknown fault module" : L"fault module 미확정");
    } else if (reason == "candidate_conflict") {
      labels.push_back(en ? L"candidate conflict" : L"후보 충돌");
    } else if (reason == "reference_clue_only") {
      labels.push_back(en ? L"reference clue only" : L"참조 단서 단독");
    } else if (reason == "stackwalk_degraded") {
      labels.push_back(en ? L"stackwalk degraded" : L"stackwalk 저하");
    } else if (reason == "symbol_runtime_degraded") {
      labels.push_back(en ? L"symbol runtime degraded" : L"심볼 런타임 저하");
    } else if (reason == "first_chance_candidate_weak") {
      labels.push_back(en ? L"first-chance candidate weak" : L"first-chance 후보 약함");
    } else if (reason == "freeze_ambiguous") {
      labels.push_back(en ? L"freeze ambiguous" : L"프리징 해석 애매");
    } else if (reason == "freeze_snapshot_fallback") {
      labels.push_back(en ? L"freeze snapshot fallback" : L"프리징 snapshot fallback");
    } else if (reason == "freeze_candidate_weak") {
      labels.push_back(en ? L"freeze candidate weak" : L"프리징 후보 약함");
    } else {
      labels.push_back(ToWideAscii(reason));
    }
  }
  return labels.empty() ? (en ? L"weak analysis context" : L"약한 분석 문맥")
                        : JoinList(labels, labels.size(), L", ");
}

void AddActionableCandidateRecommendations(
    AnalysisResult& r,
    bool en,
    const ActionableCandidate* topCandidate,
    const ActionableCandidate* secondCandidate,
    bool weakFaultLocationTopCandidate)
{
  if (!topCandidate) {
    return;
  }

  const auto candidateName = DescribeCandidate(*topCandidate);
  const bool hasFrameFamily = CandidateHasFamily(*topCandidate, "crash_logger_frame");
  const bool hasFirstChanceFamily = CandidateHasFamily(*topCandidate, "first_chance_context");
  const bool hasHistoryFamily = CandidateHasFamily(*topCandidate, "history_repeat");
  const bool hasResourceFamily = CandidateHasFamily(*topCandidate, "resource_provider");
  const bool hasHangThreadGroupFamily = CandidateHasFamily(*topCandidate, "hang_thread_group");
  // The same test as the summary sentence, so both name the Tullius callstack
  // only when a real stack walk carries the candidate.
  const bool hasStandaloneCallstackFamily = CandidateHasStandaloneCallstackSupport(r, *topCandidate);
  const bool hasScorableFirstChance = hasFirstChanceFamily && HasScorableFirstChanceContext(r.first_chance_summary);
  const bool hasScorableHistory = hasHistoryFamily && (r.history_correlation.count > 1 || !r.bucket_candidate_repeats.empty());
  const auto frameSupport = hasFrameFamily ? DescribeCrashLoggerFrameSupport(r, *topCandidate, en) : std::wstring{};
  if (weakFaultLocationTopCandidate && hasFrameFamily) {
    const bool nearby = HasNearbyProbableDll(r);
    // Only a candidate the stack also supports may say "both"; a frame alone
    // is the reference clue the candidate list already calls it.
    const bool hasStackFamily = CandidateHasFamily(*topCandidate, "actionable_stack");
    const std::wstring nextEn = nearby
      ? std::wstring(L"Compare nearby Crash Logger probable DLLs before isolating this mod.")
      : std::wstring(L"Crash Logger names no other DLL nearby, so disable or update this mod and see whether the crash still happens.");
    const std::wstring nextKo = nearby
      ? std::wstring(L"이 모드를 바로 빼기 전에 Crash Logger가 함께 지목한 근처 DLL도 비교하세요.")
      : std::wstring(L"Crash Logger가 함께 지목한 다른 DLL은 없으니, 이 모드를 끄거나 업데이트해 재현 여부를 확인하세요.");
    r.recommendations.push_back(en
      ? (hasStackFamily
          ? (L"[Actionable candidate] " + frameSupport + L" and the same-dump stack both land in " + candidateName +
              L", but this is still only the current fault-location cluster. " + nextEn)
          : (L"[Actionable candidate] " + frameSupport + L" lands in " + candidateName +
              L", but this is still only the current fault location. " + nextEn))
      : (hasStackFamily
          ? (L"[행동 우선 후보] " + frameSupport + L"과 같은 덤프의 스택이 모두 " + candidateName +
              L"을(를) 가리키지만, 둘 다 예외가 난 위치를 보여 줄 뿐입니다. " + nextKo)
          : (L"[행동 우선 후보] " + frameSupport + L"이 " + candidateName +
              L"을(를) 가리키지만, 예외가 난 위치를 보여 줄 뿐입니다. " + nextKo)));
    r.recommendations.push_back(en
      ? L"[Actionable candidate] If this clue stays isolated, prefer another capture or a richer recapture profile before escalating to FullMemory (DumpMode=2)."
      : L"[행동 우선 후보] 이 단서가 계속 고립되어 있으면, 바로 FullMemory(DumpMode=2)로 가기 전에 다른 사고를 한 번 더 캡처하거나 더 많은 정보를 담는 재수집 설정을 먼저 쓰세요.");
    return;
  }
  if (topCandidate->status_id == "cross_validated") {
    r.recommendations.push_back(hasFrameFamily
      ? (en
          ? (L"[Actionable candidate] " + frameSupport + L" and another signal agree on " + candidateName +
              L". Use DLL guidance first: update/reinstall or isolate it before broader EXE/system triage.")
          : (L"[행동 우선 후보] " + frameSupport + L"과 다른 신호가 " + candidateName +
              L" 쪽으로 합의합니다. 게임 본체·시스템 DLL을 넓게 점검하기 전에 이 DLL을 먼저 업데이트/재설치하거나 빼고 시험하세요."))
      : (en
          ? (L"[Actionable candidate] Cross-validated signals point to " + candidateName +
              L". Update/reinstall or isolate it before broader DLL triage.")
          : (L"[행동 우선 후보] 교차검증 신호가 " + candidateName +
              L" 쪽으로 모입니다. 광범위한 DLL 점검 전에 이 후보를 먼저 업데이트/격리하세요.")));
    if (CandidateHasFamily(*topCandidate, "first_chance_context")) {
      const auto firstChanceDetail = DescribeFirstChanceContext(r.first_chance_summary, en);
      if (!firstChanceDetail.empty()) {
        r.recommendations.push_back(en
          ? (L"[First-chance] Inspect the repeated first-chance module path first: " + firstChanceDetail)
          : (L"[First-chance] 반복된 선행 예외(first-chance)가 난 모듈 경로를 먼저 확인하세요: " + firstChanceDetail));
      }
    }
    r.recommendations.push_back(en
      ? (L"[Actionable candidate] If the crash repeats, disable " + candidateName +
          L" (or the providing mod/DLL) and retest.")
      : (L"[행동 우선 후보] 동일 문제가 반복되면 " + candidateName +
          L" 또는 해당 모드/DLL을 비활성화하고 다시 테스트하세요."));
  } else if (topCandidate->status_id == "related") {
    r.recommendations.push_back(hasHangThreadGroupFamily
      ? (en
          ? (L"[Synchronization stall] The game main thread and a stable worker group repeatedly retain " + candidateName +
              L" near their active stacks. Update or isolate this DLL first; no OS lock cycle was proven.")
          : (L"[동기화 정지] 게임 메인 스레드와 정지된 워커 그룹의 현재 스택 상단에 " + candidateName +
              L"이(가) 반복됩니다. 이 DLL의 업데이트/격리를 먼저 확인하되 OS 잠금 사이클이 입증된 것은 아닙니다."))
      : hasStandaloneCallstackFamily
      ? (en
          ? (L"[Actionable candidate] Tullius callstack first points to DLL candidate " + candidateName +
              L" (actionable stack). Check it before broad EXE/system triage.")
          : (L"[행동 우선 후보] Tullius 콜스택이 DLL 후보 " + candidateName +
              L"을(를) 가리킵니다. 게임 본체·시스템 DLL을 넓게 점검하기 전에 먼저 확인하세요."))
      : hasFrameFamily
      ? (hasScorableFirstChance
          ? (en
              ? (L"[Actionable candidate] " + frameSupport + L" points to DLL candidate " + candidateName +
                  L" (" + JoinFamilies(*topCandidate, en) + L"). Use DLL guidance first and check the repeated first-chance path before broad EXE/system triage.")
              : (L"[행동 우선 후보] " + frameSupport + L"이 DLL 후보 " + candidateName +
                  L"을(를) 가리킵니다(근거: " + JoinFamilies(*topCandidate, en) + L"). 게임 본체·시스템 DLL을 넓게 점검하기 전에 이 DLL과 반복 선행 예외 경로를 먼저 확인하세요."))
          : hasScorableHistory
            ? (en
                ? (L"[Actionable candidate] " + frameSupport + L" points to DLL candidate " + candidateName +
                    L" (" + JoinFamilies(*topCandidate, en) + L"). Use DLL guidance first and compare repeated same-bucket crashes before broad EXE/system triage.")
                : (L"[행동 우선 후보] " + frameSupport + L"이 DLL 후보 " + candidateName +
                    L"을(를) 가리킵니다(근거: " + JoinFamilies(*topCandidate, en) + L"). 게임 본체·시스템 DLL을 넓게 점검하기 전에 이 DLL과 같은 유형으로 반복된 크래시 이력을 먼저 확인하세요."))
          : hasResourceFamily
            ? (en
                ? (L"[Actionable candidate] " + frameSupport + L" points to DLL candidate " + candidateName +
                    L" (" + JoinFamilies(*topCandidate, en) + L"). Use DLL guidance first and compare nearby resource providers before broad EXE/system triage.")
                : (L"[행동 우선 후보] " + frameSupport + L"이 DLL 후보 " + candidateName +
                    L"을(를) 가리킵니다(근거: " + JoinFamilies(*topCandidate, en) + L"). 게임 본체·시스템 DLL을 넓게 점검하기 전에 이 DLL과 근처 리소스를 제공한 모드를 먼저 확인하세요."))
          : (en
              ? (L"[Actionable candidate] " + frameSupport + L" points to DLL candidate " + candidateName +
                  L" (" + JoinFamilies(*topCandidate, en) + L"). Use DLL guidance first before broad EXE/system triage.")
              : (L"[행동 우선 후보] " + frameSupport + L"이 DLL 후보 " + candidateName +
                  L"을(를) 가리킵니다(근거: " + JoinFamilies(*topCandidate, en) + L"). 게임 본체·시스템 DLL을 넓게 점검하기 전에 이 DLL부터 확인하세요.")))
      // One family is no multi-signal support; a Low one (the pointer scan)
      // is a weak clue, a stronger one (a hook stub owner) just one signal.
      : (topCandidate->family_count <= 1u && topCandidate->confidence_level == i18n::ConfidenceLevel::kLow)
      ? (en
          ? (L"[Actionable candidate] A single weak clue points to " + candidateName +
              L" (" + JoinFamilies(*topCandidate, en) + L"). Check it, but do not treat it as the cause on this alone.")
          : (L"[행동 우선 후보] 약한 단서 하나가 " + candidateName +
              L"을(를) 가리킵니다(근거: " + JoinFamilies(*topCandidate, en) + L"). 확인은 하되 이것만으로 원인이라고 단정하지 마세요."))
      : topCandidate->family_count <= 1u
      ? (en
          ? (L"[Actionable candidate] One signal points to " + candidateName +
              L" (" + JoinFamilies(*topCandidate, en) + L"). Check it before falling back to generic SKSE/plugin triage.")
          : (L"[행동 우선 후보] 신호 하나가 " + candidateName +
              L"을(를) 가리킵니다(근거: " + JoinFamilies(*topCandidate, en) + L"). 일반적인 SKSE/DLL 점검보다 먼저 확인하세요."))
      : (en
          ? (L"[Actionable candidate] Partial multi-signal support points to " + candidateName +
              L" (" + JoinFamilies(*topCandidate, en) + L"). Check it before falling back to generic SKSE/plugin triage.")
          : (L"[행동 우선 후보] 부분적인 다중 신호가 " + candidateName +
              L"을(를) 가리킵니다(근거: " + JoinFamilies(*topCandidate, en) + L"). 일반적인 SKSE/DLL 점검보다 먼저 확인하세요.")));
    if (hasFirstChanceFamily) {
      const auto firstChanceDetail = DescribeFirstChanceContext(r.first_chance_summary, en);
      if (!firstChanceDetail.empty()) {
        r.recommendations.push_back(en
          ? (L"[First-chance] Repeated first-chance exceptions matched this candidate. Check that module path before broad EXE/system crash triage: " + firstChanceDetail)
          : (L"[First-chance] 반복된 선행 예외(first-chance)가 이 후보와 맞습니다. 게임 본체·시스템 DLL을 넓게 점검하기 전에 그 모듈 경로부터 확인하세요: " + firstChanceDetail));
      }
    }
  } else if (topCandidate->status_id == "reference_clue") {
    if (hasFrameFamily) {
      r.recommendations.push_back(en
        ? (L"[Crash Logger frame] " + frameSupport + L" points to DLL candidate " + candidateName +
            L", but no second independent signal agrees yet. Use DLL guidance first and confirm with another capture if needed.")
        : (L"[Crash Logger 프레임] " + frameSupport + L"이 DLL 후보 " + candidateName +
            L"을(를) 가리키지만 이를 뒷받침하는 다른 독립 신호는 아직 없습니다. 이 DLL부터 확인하고, 필요하면 다음 크래시를 추가로 캡처해 비교하세요."));
      r.recommendations.push_back(en
        ? L"[Crash Logger frame] If the frame clue stays isolated, capture another incident or rerun with a richer crash recapture profile before escalating to FullMemory (DumpMode=2)."
        : L"[Crash Logger 프레임] 이 프레임 단서만 계속 남으면 다른 사고를 한 번 더 캡처하거나, 바로 FullMemory(DumpMode=2)로 가지 말고 더 많은 정보를 담는 재수집 설정으로 먼저 다시 캡처하세요.");
    } else {
      r.recommendations.push_back(en
        ? (L"[Object ref] The game was processing " + candidateName +
            L" at crash time, but no second independent signal agrees yet. Treat it as a clue first.")
        : (L"[오브젝트 참조] 사고 당시 게임이 " + candidateName +
            L"을(를) 처리 중이었지만 이를 뒷받침하는 다른 독립 신호는 아직 없습니다. 우선 단서로 보세요."));
      r.recommendations.push_back(en
        ? L"[Object ref] If the clue stays isolated, capture another incident or rerun with a richer crash recapture profile before escalating to FullMemory (DumpMode=2)."
        : L"[오브젝트 참조] 이 단서가 계속 단독으로 남으면 다른 사고를 한 번 더 캡처하거나, 바로 FullMemory(DumpMode=2)로 가지 말고 더 많은 정보를 담는 재수집 설정으로 먼저 다시 캡처하세요.");
    }
  } else if (topCandidate->status_id == "conflicting" && secondCandidate) {
    const auto secondName = DescribeCandidate(*secondCandidate);
    r.recommendations.push_back(en
      ? (L"[Conflict] Signals split between " + candidateName + L" (" + JoinFamilies(*topCandidate, en) + L") and " +
          secondName + L" (" + JoinFamilies(*secondCandidate, en) + L"). Check whether Crash Logger frame first DLL guidance and object ref/stack evidence disagree before retesting.")
      : (L"[충돌] 신호가 " + candidateName + L" (" + JoinFamilies(*topCandidate, en) + L")와 " +
          secondName + L" (" + JoinFamilies(*secondCandidate, en) + L")로 갈립니다. Crash Logger 프레임 쪽 근거와 오브젝트 참조·스택 근거가 어디서 갈리는지 먼저 확인한 뒤 다시 재현해 보세요."));
    r.recommendations.push_back(en
      ? L"[Conflict] If the split persists, rerun with a richer crash recapture profile first; use FullMemory (DumpMode=2) only if the tie remains."
      : L"[충돌] 이렇게 계속 갈리면 더 많은 정보를 담는 재수집 설정으로 먼저 다시 캡처하고, 그래도 갈리면 그때만 FullMemory(DumpMode=2)를 사용하세요.");
  }
}

void AddModalDialogWaitRecommendations(AnalysisResult& r, bool en)
{
  const auto& modal = r.modal_dialog_wait;
  const std::wstring caller = !modal.caller_inferred_mod_name.empty()
    ? (modal.caller_inferred_mod_name + L" (" + modal.caller_module_filename + L")")
    : modal.caller_module_filename;
  const std::wstring named = modal.named_module_filename.empty() ? std::wstring{} : DialogNamedModuleDisplay(modal);

  // The mod the dialog names comes first, as in the quick answer (and so it is
  // the next action).
  if (!named.empty()) {
    r.recommendations.push_back(modal.named_module_version_issue
      ? (en ? (L"[Modal dialog] Update " + named +
                L" to a version made for your game version, or disable it (see what the error window says).")
            : (L"[Modal 대화상자] " + named + L"을(를) 지금 게임 버전에 맞는 버전으로 업데이트하거나 끄세요(오류 창에 적힌 내용 참고)."))
      : (en ? (L"[Modal dialog] The error window names " + named +
                L". Follow what the window says, or update or disable that mod.")
            : (L"[Modal 대화상자] 오류 창에 " + named +
                L"이(가) 적혀 있습니다. 창에 적힌 내용을 따르거나, 그 모드를 업데이트하거나 끄세요.")));
  }
  r.recommendations.push_back(en
    ? L"[Modal dialog] The game is waiting for a dialog to be closed. Alt-Tab or check the taskbar for an error window hidden behind the game, then read its message."
    : L"[Modal 대화상자] 게임이 대화상자가 닫히기를 기다리고 있습니다. Alt-Tab 또는 작업 표시줄에서 게임 창 뒤에 숨은 오류 창을 찾아 메시지를 확인하세요.");
  if (!modal.dialog_text.empty()) {
    r.recommendations.push_back(en
      ? (L"[Modal dialog] Captured dialog text: " + modal.dialog_text)
      : (L"[Modal 대화상자] 캡처된 대화상자 내용: " + modal.dialog_text));
  }
  if (const auto files = DescribeDialogNamedFiles(modal, en); !files.empty()) {
    r.recommendations.push_back(en
      ? (L"[Modal dialog] " + files + L" Read the window to see which mod it is about.")
      : (L"[Modal 대화상자] " + files + L" 창 내용을 읽고 어느 모드에 대한 것인지 확인하세요."));
  }
  // CommonLib puts the plugin's file name in the dialog title, so it names the
  // plugin even when the stack walk could not resolve the caller.
  std::wstring plugin = (modal.caller_kind == "plugin" && !caller.empty()) ? caller
    : (!named.empty() ? named : modal.dialog_title);
  if (plugin.empty()) {
    plugin = en ? L"The plugin" : L"해당 플러그인";
  }
  const std::wstring gameVersion = r.game_version.empty() ? std::wstring{} : (L" (" + ToWideAscii(r.game_version) + L")");
  if (modal.address_library_issue == "plugin_incompatible") {
    r.recommendations.push_back(en
      ? (L"[Modal dialog] " + plugin + L" could not use the Address Library of this game" + gameVersion +
          L": the format or an address it needs is missing. Usually the plugin was built for another game version; "
          L"an outdated or damaged Address Library file gives the same message. Check that Address Library for this "
          L"game version is current, then install the file of that mod made for this game version, or disable it.")
      : (L"[Modal 대화상자] " + plugin + L"이(가) 현재 게임" + gameVersion +
          L"의 Address Library를 쓰지 못했습니다(형식이 맞지 않거나 필요한 주소가 없음). 대개 플러그인이 다른 게임 버전용으로 "
          L"빌드된 경우지만, Address Library 파일이 오래되었거나 손상되어도 같은 메시지가 나옵니다. 이 게임 버전용 Address "
          L"Library가 최신인지 먼저 확인하고, 그다음 이 게임 버전용 모드 파일로 바꾸거나 해당 모드를 비활성화하세요."));
  } else if (modal.address_library_issue == "address_library_missing") {
    r.recommendations.push_back(en
      ? (L"[Modal dialog] " + plugin + L" could not find the Address Library file for this game version" + gameVersion +
          L". Install or update Address Library for SKSE Plugins for this game version. If it is already installed, the plugin was likely built for the other edition (SE 1.5.97 or AE 1.6+); install the matching file.")
      : (L"[Modal 대화상자] " + plugin + L"이(가) 현재 게임 버전" + gameVersion +
          L"용 Address Library 파일을 찾지 못했습니다. 이 게임 버전용 Address Library for SKSE Plugins를 설치하거나 업데이트하세요. 이미 설치되어 있다면 플러그인이 다른 판(SE 1.5.97 / AE 1.6 이상)용으로 빌드되었을 가능성이 높으니 맞는 파일로 바꾸세요."));
  } else if (modal.caller_kind == "plugin" && !caller.empty()) {
    r.recommendations.push_back(en
      ? (L"[Modal dialog] " + caller + L" opened the dialog. Follow its message (missing requirement, version mismatch, config error) or check that mod's documentation.")
      : (L"[Modal 대화상자] " + caller + L"이(가) 대화상자를 열었습니다. 메시지(누락된 선행 모드, 버전 불일치, 설정 오류)를 따르거나 해당 모드 설명을 확인하세요."));
  } else if (modal.caller_kind == "skse_runtime") {
    r.recommendations.push_back(en
      ? L"[Modal dialog] SKSE itself opened the dialog. This usually names a plugin that failed to load or does not match the game version; update or remove that plugin."
      : L"[Modal 대화상자] SKSE 자체가 대화상자를 열었습니다. 보통 로드에 실패했거나 게임 버전과 맞지 않는 플러그인을 알리는 창이므로 해당 플러그인을 업데이트하거나 제거하세요.");
  }
  r.recommendations.push_back(en
    ? L"[Modal dialog] Do not isolate mods based on the stack modules of this dump; they only happened to be on the waiting main thread."
    : L"[Modal 대화상자] 이 덤프의 스택 모듈을 근거로 모드를 격리하지 마세요. 대기 중인 메인 스레드에 우연히 남아 있었을 뿐입니다.");
}

}  // namespace

void BuildRecommendations(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx)
{
  const bool en = ctx.en;
  if (ctx.isFilteredCleanExit) {
    const bool lateRecovery = r.clean_exit_filter_context == "late_recovery";
    r.recommendations.push_back(en
      ? (lateRecovery
           ? L"[Clean exit] The validated helper record says the game kept running after this exception. Do not blame a mod from this preserved diagnostic dump."
           : L"[Clean exit] The validated helper record says the process exited with code 0. Do not blame a mod from this preserved diagnostic dump.")
      : (lateRecovery
           ? L"[정상 종료] 검증된 헬퍼 레코드에 따르면 이 예외 뒤에도 게임이 계속 실행됐습니다. 보존된 진단 덤프만으로 모드를 원인으로 지목하지 마세요."
           : L"[정상 종료] 검증된 헬퍼 레코드에 따르면 프로세스가 종료 코드 0으로 끝났습니다. 보존된 진단 덤프만으로 모드를 원인으로 지목하지 마세요."));
    r.recommendations.push_back(en
      ? L"[Clean exit] Keep the dump, clean-exit evidence sidecar, and helper log together when reporting filter behavior."
      : L"[정상 종료] 필터 동작을 제보할 때 덤프, 정상 종료 증거 사이드카, 헬퍼 로그를 함께 보관하세요.");
    return;
  }
  if (ctx.isForcedClose) {
    // ADR-0022: first, so it is the next action (NextAction.cpp). A jump into
    // freed code names its caller below, which may only have used the address
    // (FaultJumpedIntoFreedCode): not a mod to report to on that alone. Whose hook the
    // freed code was comes from the helper's record, when there is one
    // (ADR-0024).
    const auto& freed = r.freed_hook;
    const std::wstring freedOwner = !freed.detected || freed.owner_module.empty() ? std::wstring{}
      : freed.owner_mod_name.empty()                                         ? freed.owner_module
                                                                              : (freed.owner_mod_name + L" (" + freed.owner_module + L")");
    r.recommendations.push_back(
      (en ? L"[Forced close] " : L"[강제 종료] ") + ForcedCloseLead(r.forced_close, en) +
      (FaultJumpedIntoFreedCode(r) && !freedOwner.empty()
         ? (en ? (L" Nothing to do. While the game was closing, a hook of " + freedOwner +
                  L" was called after it had been freed (recorded while the game ran). If the same error comes every "
                  L"time you close the game, pass the analysis below to the author of " + freedOwner + L".")
               : (L" 따로 할 일은 없습니다. 게임이 닫히는 중에 이미 해제된 " + freedOwner +
                  L"의 후킹 코드가 호출됐습니다(게임 중 기록). 게임을 닫을 때마다 같은 오류가 난다면 아래 분석을 " +
                  freedOwner + L" 제작자에게 전해 주세요."))
         : FaultJumpedIntoFreedCode(r)
         ? (en ? L" Nothing to do. While the game was closing, code that had been freed was called (for example a hook "
                 L"another plugin removed). The mod that called it may only be a victim, and the dump does not show who "
                 L"freed it."
               : L" 따로 할 일은 없습니다. 게임이 닫히는 중에 이미 해제된 코드(예: 다른 플러그인이 제거한 후킹 코드)가 "
                 L"호출됐습니다. 그것을 호출한 모드는 피해자일 수 있고, 누가 해제했는지는 덤프로 알 수 없습니다.")
         : (en ? L" Nothing to do. If the same error comes every time you close the game, pass the analysis below to the "
                 L"author of the mod it names."
               : L" 따로 할 일은 없습니다. 게임을 닫을 때마다 같은 오류가 난다면 아래 분석을 거기 나온 모드의 제작자에게 전해 "
                 L"주세요.")));
  }
  const bool isCrashLike = ctx.isCrashLike;
  const bool isSnapshotLike = ctx.isSnapshotLike;
  const bool isHangLike = ctx.isHangLike;
  const bool isManualCapture = ctx.isManualCapture;
  const bool hasModule = ctx.hasModule;
  const bool isSystem = ctx.isSystem;
  const bool isGameExe = ctx.isGameExe;
  const auto hitch = ctx.hitch;
  const auto& wct = ctx.wct;
  const std::wstring& suspectBasis = ctx.suspectBasis;
  const bool hasStackCandidate = !r.suspects.empty();
  const SuspectItem* firstNonHookStackCandidate = nullptr;
  auto isActionableStackCandidate = [&](const SuspectItem& s) {
    return !minidump::IsKnownHookFramework(s.module_filename) &&
           !minidump::IsSystemishModuleAt(s.module_filename, s.module_path) &&
           !minidump::IsGameExeModule(s.module_filename);
  };
  if (hasStackCandidate) {
    for (const auto& s : r.suspects) {
      if (isActionableStackCandidate(s)) {
        firstNonHookStackCandidate = &s;
        break;
      }
    }
  }
  const bool hasNonHookStackCandidate = (firstNonHookStackCandidate != nullptr);
  const bool topStackCandidateIsHookFramework = hasStackCandidate && minidump::IsKnownHookFramework(r.suspects[0].module_filename);
  const bool topStackCandidateIsSystem =
    hasStackCandidate && minidump::IsSystemishModuleAt(r.suspects[0].module_filename, r.suspects[0].module_path);
  const bool preferStackCandidateOverFault =
    ctx.isHookFramework &&
    hasNonHookStackCandidate;
  const bool allowFaultModuleTopSuspectRecommendations =
    !ctx.isHookFramework || hasNonHookStackCandidate;
  const ActionableCandidate* topCandidate = !r.actionable_candidates.empty() ? &r.actionable_candidates[0] : nullptr;
  const ActionableCandidate* secondCandidate = (r.actionable_candidates.size() > 1u) ? &r.actionable_candidates[1] : nullptr;
  const bool hasActionableCandidates = (topCandidate != nullptr);
  const bool weakFaultLocationOnly =
    IsWeakFaultLocationOnlySuspect(r, ctx) ||
    (topCandidate && IsWeakFaultLocationActionableCandidate(r, *topCandidate, ctx));
  const bool topCandidateMatchesFaultModule =
    topCandidate &&
    CandidateMatchesModule(*topCandidate, r.fault_module_filename);
  const bool topCandidateCrossValidatedFaultModule =
    topCandidateMatchesFaultModule &&
    topCandidate->status_id == "cross_validated";

  if (r.hook_stub_race.detected) {
    // ADR-0016: a timing race in that plugin's hook install; it may not recur
    // on the next run, so a clean retry proves nothing.
    const auto& race = r.hook_stub_race;
    const std::wstring owner = race.target_mod_name.empty()
      ? race.target_module
      : (race.target_mod_name + L" (" + race.target_module + L")");
    r.recommendations.push_back(en
      ? (L"[Known cause] A hook into " + owner + L" was installed while other game threads ran, and one of them "
         L"ran it half-written. Update that mod, or disable it if the crash repeats, and report the timing race to "
         L"its author with this report. The crash depends on timing, so a run without it does not mean it is fixed.")
      : (L"[알려진 원인] " + owner + L"(으)로 가는 후킹이 다른 게임 스레드가 도는 중에 설치되다가, 그중 한 스레드가 반쯤 "
         L"써진 후킹을 실행했습니다. 이 모드를 업데이트하고, 크래시가 반복되면 끄세요. 작성자에게 이 리포트와 함께 설치 시점 "
         L"경쟁 문제를 알려 주세요. 타이밍에 따라 나는 크래시라, 한 번 안 난다고 해결된 것은 아닙니다."));
  }

  if (r.signature_match.has_value()) {
    for (const auto& rec : r.signature_match->recommendations) {
      if (!rec.empty()) {
        r.recommendations.push_back(rec);
      }
    }
  }

  if (r.graphics_diag.has_value()) {
    for (const auto& rec : r.graphics_diag->recommendations) {
      if (!rec.empty()) {
        r.recommendations.push_back(rec);
      }
    }
  }

  // A low-confidence plugin rule (e.g. ESL slots near the limit) is general
  // upkeep, not an explanation of this incident, so it goes to the end of the
  // checklist instead of becoming the first action.
  std::vector<std::wstring> lowConfidencePluginRecommendations;
  if (!r.plugin_diagnostics.empty()) {
    for (const auto& pd : r.plugin_diagnostics) {
      for (const auto& rec : pd.recommendations) {
        if (!rec.empty()) {
          if (!isCrashLike &&
              (rec.find(L"[BEES]") != std::wstring::npos ||
               rec.find(L"bees.dll") != std::wstring::npos ||
               rec.find(L"1.6.1130") != std::wstring::npos)) {
            continue;
          }
          if (pd.confidence_level == i18n::ConfidenceLevel::kLow) {
            lowConfidencePluginRecommendations.push_back(rec);
            continue;
          }
          r.recommendations.push_back(rec);
        }
      }
    }
  }
  // Plugins SKSE refused at startup are upkeep for the next launch, not an
  // explanation of this incident, so they join the end of the checklist too.
  if (r.skse_log.status == "matched" && r.skse_log.issue_count > 0) {
    const auto issues = SummarizeSkseLogIssues(r.skse_log, en, 6, L"; ");
    const bool thisSession = r.skse_log.time_checked;
    lowConfidencePluginRecommendations.push_back(en
      ? (std::wstring(thisSession
            ? L"[SKSE] SKSE did not load these DLLs correctly in this session: "
            : L"[SKSE] Per the skse64.log with this session's image base (not confirmed by time to be this session), "
              L"SKSE did not load these DLLs correctly: ") + issues +
          L". This is not evidence for the cause of this incident. A helper DLL without version data can stay; "
          L"for a plugin you meant to use, do what its note says (a file for this game version, or the DLL it needs).")
      : (std::wstring(thisSession
            ? L"[SKSE] 이번 실행에서 SKSE가 정상적으로 로드하지 못한 DLL: "
            : L"[SKSE] 이미지 베이스가 같은 skse64.log 기준(이번 실행인지 시각으로 확인 못 함), SKSE가 정상적으로 로드하지 못한 DLL: ") +
          issues +
          L". 이번 사고의 원인 근거는 아닙니다. 버전 정보가 없는 보조 DLL은 그대로 둬도 되고, "
          L"쓰려던 플러그인이라면 사유에 맞게 고치세요(현재 게임 버전용 파일로 교체, 또는 필요한 DLL 설치)."));
  }
  auto appendLowConfidencePluginRecommendations = [&]() {
    r.recommendations.insert(
      r.recommendations.end(),
      lowConfidencePluginRecommendations.begin(),
      lowConfidencePluginRecommendations.end());
  };

  // A module that swallows crashes changes how every capture reads, whatever
  // its kind, so it is named before the rest of the checklist, recapture
  // advice included: a recapture with it still loaded tells no more.
  for (const auto& module : r.problematic_modules) {
    const auto* rule = FindProblematicModuleRule(module.filename);
    if (!rule) {
      continue;
    }
    const std::wstring who = DescribeProblematicModule(module, en);
    r.recommendations.push_back(en
      ? (L"[Problem module] " + who + L" is loaded: " + std::wstring(rule->reason_en) +
          L". Disable it while you look for the cause, then reproduce the problem.")
      : (L"[문제 모듈] " + who + L"이(가) 로드되어 있습니다. 이 모듈은 " + std::wstring(rule->reason_ko) +
          L". 원인을 찾는 동안에는 비활성화하고 다시 재현해 보세요."));
  }

  if (r.symbol_runtime_degraded) {
    r.recommendations.push_back(en
      ? L"[Symbols] Fix dbghelp/msdia or symbol cache/path health first before over-trusting weak stack or source-line results."
      : L"[심볼] 약한 스택/소스라인 결과를 과신하기 전에 dbghelp/msdia 또는 심볼 캐시/경로 상태를 먼저 바로잡으세요.");
  }

  if (r.incident_capture_profile_present && r.incident_capture_kind == "crash_recapture") {
    const auto quality = DescribeCaptureProfileStrength(r, en);
    r.recommendations.push_back(en
      ? (L"[Recapture] This dump already came from a richer crash recapture profile" +
          (quality.empty() ? L"" : L" (" + quality + L")") +
          L". Escalate to FullMemory only if the evidence still stays weak.")
      : (L"[재수집] 이 덤프는 이미 richer crash recapture profile로 다시 수집된 결과입니다" +
          (quality.empty() ? L"" : L" (" + quality + L")") +
          L". 근거가 여전히 약할 때만 FullMemory로 올리세요."));
  }
  if (r.incident_recapture_evaluation_present && r.incident_recapture_triggered) {
    const auto reasons = DescribeRecaptureReasons(r, en);
    if (r.incident_recapture_target_profile == "crash_richer") {
      r.recommendations.push_back(en
        ? (L"[Recapture] Helper selected crash_richer because prior analysis stayed weak (" + reasons +
            L"). Use crash_full only if the richer recapture is still ambiguous.")
        : (L"[재수집] 이전 분석이 약해서 helper가 crash_richer를 선택했습니다 (" + reasons +
            L"). richer recapture 이후에도 애매할 때만 crash_full로 올리세요."));
    } else if (r.incident_recapture_target_profile == "crash_full") {
      r.recommendations.push_back(en
        ? (L"[Recapture] Helper escalated to crash_full after repeated weak analysis (" + reasons +
            L"). Focus on why the richer stage was insufficient before asking for another full dump.")
        : (L"[재수집] 약한 분석이 반복되어 helper가 crash_full까지 올렸습니다 (" + reasons +
            L"). 추가 full dump를 요구하기 전에 왜 richer 단계가 부족했는지 먼저 확인하세요."));
    } else if (r.incident_recapture_target_profile == "freeze_snapshot_richer") {
      r.recommendations.push_back(en
        ? (L"[Recapture] Helper selected freeze_snapshot_richer because freeze quality stayed weak (" + reasons +
            L"). Reproduce the stall with snapshot-backed capture before broad deadlock triage.")
        : (L"[재수집] 프리징 품질이 약해서 helper가 freeze_snapshot_richer를 선택했습니다 (" + reasons +
            L"). 광범위한 데드락 점검 전에 snapshot 기반으로 다시 재현해 보세요."));
    }
  }

  if (r.needs_bees && isCrashLike) {
    r.recommendations.push_back(en
      ? L"[BEES] Install BEES (if it is installed, check skse64.log that BackportedESLSupport.dll loaded) or update the game runtime to 1.6.1130+."
      : L"[BEES] BEES를 설치하거나(이미 설치했다면 skse64.log에서 BackportedESLSupport.dll이 로드되었는지 확인) 게임 런타임을 1.6.1130 이상으로 업데이트하세요.");
  }

  // The plugin_rules.json diagnosis already says this (with its own grade).
  if (!r.missing_masters.empty() && !FindMissingMasterDiagnosis(r)) {
    r.recommendations.push_back(en
      ? L"[Masters] Install missing masters or disable dependent plugins."
      : L"[마스터] 누락된 마스터를 설치하거나 의존 플러그인을 비활성화하세요.");
  }

  if (isHangLike && r.modal_dialog_wait.detected) {
    AddModalDialogWaitRecommendations(r, en);
    // The dialog explains the freeze; general load-order upkeep (ESL slots and
    // the like) would only bury it. SKSE's refused DLLs can still bear on it.
    for (const auto& rec : lowConfidencePluginRecommendations) {
      if (rec.rfind(L"[SKSE]", 0) == 0) {
        r.recommendations.push_back(rec);
      }
    }
    return;
  }

  if (r.crash_logger_thread_dump_deadlock.detected && r.main_thread_lock_owner.tid != 0u) {
    // Crash Logger's thread dump caused it: neither the GPU advice, the
    // resource and conflict checklist nor the modules on the main thread's
    // stack apply, so this is the only advice (as the summary leads with it).
    const auto& path = r.main_thread_wait.path_modules;
    const std::wstring mods = path.empty()
      ? std::wstring{}
      : ((en ? L" (" : L"(") + JoinList(path, path.size(), L", ") + L")");
    r.recommendations.push_back(en
      ? (L"[Main thread] This freeze came from Crash Logger's thread dump deadlocking. Do not press Crash Logger's "
          L"thread-dump hotkey (Ctrl+Shift+F12 by default) while playing, or turn it off in CrashLogger.ini: under [Debug], "
          L"set \"Enable Thread Dump Hotkey = false\". The mods on the main thread's stack" + mods +
          L" are not the cause, so there is no need to remove them.")
      : (L"[메인 스레드] 이번 프리징은 Crash Logger의 스레드 덤프가 교착 상태에 빠져서 생겼습니다. 게임 중에는 Crash Logger의 "
          L"스레드 덤프 단축키(기본 Ctrl+Shift+F12)를 누르지 마세요. 아예 끄려면 CrashLogger.ini의 [Debug] 항목에서 "
          L"\"Enable Thread Dump Hotkey = false\"로 바꾸면 됩니다. 메인 스레드 스택에 보이는 모드" + mods +
          L"는 원인이 아니므로 뺄 필요가 없습니다."));
    return;
  }

  // Recommendations (checklist)
  if (isSnapshotLike) {
    r.recommendations.push_back(en
      ? L"[Snapshot] No exception/crash info is present. This dump alone is not enough to blame a mod."
      : L"[정상/스냅샷] 예외(크래시) 정보가 없습니다. 이 덤프만으로 '어떤 모드가 크래시 원인'인지 판단하기 어렵습니다.");
    r.recommendations.push_back(en
      ? L"[Snapshot] Capture during a real issue for diagnosis: (1) real CTD dump, (2) manual capture during freeze/infinite loading (Ctrl+Shift+Backspace by default) or an auto hang dump."
      : L"[정상/스냅샷] 문제 상황에서 캡처해야 진단이 가능합니다: (1) 실제 크래시 덤프, (2) 프리징/무한로딩 중 수동 캡처(기본 Ctrl+Shift+Backspace) 또는 자동 감지 덤프");
  }

  if (r.exc_code != 0) {
    if (r.exc_code == 0xC0000005u) {
      r.recommendations.push_back(en
        ? L"[Basics] ExceptionCode=0xC0000005 (Access Violation). Often caused by DLL hooks / invalid memory access."
        : L"[기본] ExceptionCode=0xC0000005(접근 위반)입니다. 보통 DLL 후킹/메모리 접근 문제로 발생합니다.");
    } else if (r.exc_code == 0x80000003u) {
      r.recommendations.push_back(en
        ? L"[Basics] ExceptionCode=0x80000003 (breakpoint). A mod usually raises this when it stops itself on a failed internal check (assert/abort). Check the first non-system module in the call stack, the code that called the stopping function."
        : L"[기본] ExceptionCode=0x80000003(브레이크포인트)입니다. 모드가 내부 검사(assert/abort)에 실패해 스스로 실행을 멈출 때 주로 생깁니다. 콜스택에서 시스템 DLL이 아닌 첫 모듈, 즉 멈춤을 호출한 코드를 먼저 확인하세요.");
    } else {
      wchar_t buf[128]{};
      swprintf_s(buf, en ? L"[Basics] ExceptionCode=0x%08X." : L"[기본] ExceptionCode=0x%08X 입니다.", r.exc_code);
      r.recommendations.push_back(buf);
    }

    // A forced close gets neither "a real CTD" nor "if the game kept
    // running": the player closed it (ADR-0022).
    if (!ctx.isForcedClose && IsCrashLoggerConfirmedCppException(r)) {
      r.recommendations.push_back(en
        ? L"[Interpretation] Crash Logger logged this C++ exception as the unhandled exception that crashed the game, so this is a real CTD, not a caught throw. Start with the module that threw it (first non-system module in the call stack)."
        : L"[해석] Crash Logger도 이 C++ 예외를 게임을 끝낸 처리되지 않은 예외로 기록했습니다. 잡힌 예외(오탐)가 아니라 실제 CTD입니다. 예외를 던진 모듈(콜스택에서 시스템 DLL이 아닌 첫 모듈)부터 확인하세요.");
    } else if (!ctx.isForcedClose && r.exc_code == 0xE06D7363u) {
      // With the default CrashHookMode=1 the plugin records a C++ exception
      // only when nothing handled it: Crash Logger's report, the unhandled-
      // exception filter or abort() (CrashHandler.cpp), so lead with that; a
      // caught throw is the CrashHookMode=2 case.
      r.recommendations.push_back(en
        ? L"[Interpretation] 0xE06D7363 is a C++ exception (throw). With the default CrashHookMode=1 it is recorded only when nothing handled it (Crash Logger's report, the unhandled-exception filter, or abort()), so if the game closed, start with the module that threw it (the first non-system module in the call stack)."
        : L"[해석] 0xE06D7363은 C++ 예외(throw) 코드입니다. 기본값 CrashHookMode=1에서는 어디에서도 처리되지 않은 예외만(Crash Logger 보고, 처리되지 않은 예외 필터, abort()) 기록되므로, 게임이 꺼졌다면 예외를 던진 모듈(콜스택에서 시스템 DLL이 아닌 첫 모듈)부터 확인하세요.");
      r.recommendations.push_back(en
        ? L"[Interpretation] If the game kept running, this dump is not a real CTD: the exception was caught and handled."
        : L"[해석] 게임이 계속 실행됐다면 이 덤프는 실제 CTD가 아니라, 게임이 잡아서 처리한 예외입니다.");
      r.recommendations.push_back(en
        ? L"[Config] If you changed CrashHookMode to 2 in SkyrimDiag.ini, set it back to 1 (the default); mode 2 also records C++ exceptions the game caught and handled."
        : L"[설정] SkyrimDiag.ini에서 CrashHookMode=2로 바꿨다면 기본값 1로 되돌리세요. 2에서는 게임이 잡아서 처리한 C++ 예외까지 기록됩니다.");
    }
  }

  const std::wstring faultModuleLower = minidump::WideLower(r.fault_module_filename);
  const bool faultIsCrashLogger =
    faultModuleLower == L"crashlogger.dll" || faultModuleLower == L"crashloggersse.dll";
  if (r.exc_code != 0 && faultIsCrashLogger) {
    // CrashLogger catches access violations from its own memory probes while
    // writing a report, so a fault inside it usually belongs to a crash it was
    // already reporting rather than to CrashLogger.
    r.recommendations.push_back(en
      ? L"[Interpretation] The exception is inside CrashLogger. It is most likely a secondary fault from CrashLogger probing memory while it reported another crash. Judge the crash by the original exception and call stack in the paired CrashLogger log, not by CrashLogger."
      : L"[해석] 예외 위치가 CrashLogger 내부입니다. CrashLogger가 다른 크래시를 기록하면서 메모리를 조사하다 낸 2차 예외일 가능성이 큽니다. CrashLogger가 아니라 짝지어진 CrashLogger 로그의 원래 예외와 콜스택을 기준으로 판단하세요.");
    if (!r.crash_logger_exception_type.empty()) {
      const std::wstring where = r.crash_logger_exception_location.empty()
        ? std::wstring{}
        : (en ? (L" at " + r.crash_logger_exception_location) : (L"(" + r.crash_logger_exception_location + L")"));
      const bool breakpoint = r.crash_logger_exception_type == L"EXCEPTION_BREAKPOINT";
      r.recommendations.push_back(en
        ? (L"[Interpretation] The paired CrashLogger log records the original exception: " + r.crash_logger_exception_type + where +
            (breakpoint ? L". A breakpoint like this is usually a mod stopping itself on a failed internal check (assert/abort); look at the mods on that log's call stack." : L"."))
        : (L"[해석] 짝지어진 CrashLogger 로그의 원래 예외는 " + r.crash_logger_exception_type + where + L"입니다." +
            (breakpoint ? L" 이런 브레이크포인트는 보통 모드가 내부 검사(assert/abort)에 실패해 스스로 멈춘 것이니, 그 로그의 콜스택에 있는 모드를 보세요." : L"")));
    }
  } else if (ctx.isHookFramework) {
    r.recommendations.push_back(en
      ? L"[Hook framework] This mod extensively hooks the game engine. It may be a victim of memory corruption caused by another mod, not the root cause itself. Check other suspect candidates first."
      : L"[훅 프레임워크] 이 모드는 게임 엔진을 광범위하게 훅합니다. 다른 모드의 메모리 오염으로 인한 피해자일 수 있으며, 이 모드 자체가 원인이 아닐 수 있습니다. 다른 후보 모드를 먼저 점검하세요.");
  }

  // A lock-owner wait (both WCT passes) puts the stall on the owner thread,
  // and Crash Logger's thread-dump deadlock on Crash Logger; candidates from
  // the main thread's stack only called into the wait.
  // An engine loop on a NaN value (engine_spin_nan) names no mod either: the
  // stack plugins only called into the engine.
  const bool lockOwnerWait = IsLockOwnerFreezeState(r.freeze_analysis.state_id);
  // Nor does a deadlock on the engine's own locks (engine_lock_deadlock).
  const bool stackIsCallPathOnly = lockOwnerWait || r.engine_spin.nan_detected ||
    r.freeze_analysis.state_id == "engine_lock_deadlock";
  if (!stackIsCallPathOnly) {
    AddActionableCandidateRecommendations(
      r,
      en,
      topCandidate,
      secondCandidate,
      topCandidate && IsWeakFaultLocationActionableCandidate(r, *topCandidate, ctx));
  }

  // A main thread waiting in the engine, the GPU or a driver only has the
  // stack plugins on its call path (ADR-0009): the summary says not to remove
  // them, so they get no update/disable advice either. A lock owner named by
  // WCT comes first in the summary; one WCT pass alone keeps the stack advice.
  const bool stackIsBystander = IsBystanderWait(r.main_thread_wait) && r.suspects_from_main_thread &&
    r.main_thread_lock_owner.tid == 0u;
  // Nor after a jump into freed code: the stack starts at its caller, which
  // may only have used the address (FaultJumpedIntoFreedCode).
  const bool allowTopSuspectActionRecommendations =
    !isSnapshotLike && !stackIsCallPathOnly && !stackIsBystander && !FaultJumpedIntoFreedCode(r);
  if (!hasActionableCandidates && allowTopSuspectActionRecommendations && !weakFaultLocationOnly &&
      !r.inferred_mod_name.empty() && !preferStackCandidateOverFault && allowFaultModuleTopSuspectRecommendations) {
    r.recommendations.push_back(en
      ? (L"[Top suspect] Reproduce after updating/reinstalling '" + r.inferred_mod_name + L"'.")
      : (L"[유력 후보] '" + r.inferred_mod_name + L"' 모드를 업데이트/재설치 후 재현 여부 확인"));
    r.recommendations.push_back(en
      ? (L"[Top suspect] If it repeats, disable the mod (or its SKSE plugin DLL) and retest: '" + r.inferred_mod_name + L"'.")
      : (L"[유력 후보] 동일 크래시가 반복되면 '" + r.inferred_mod_name + L"' 모드(또는 해당 모드의 SKSE 플러그인 DLL)를 비활성화 후 재현 여부 확인"));
  }

  if (!hasActionableCandidates && allowTopSuspectActionRecommendations && !weakFaultLocationOnly &&
      (r.inferred_mod_name.empty() || preferStackCandidateOverFault) && hasNonHookStackCandidate) {
    const auto& s0 = *firstNonHookStackCandidate;
    if (!s0.inferred_mod_name.empty()) {
      r.recommendations.push_back(en
        ? (L"[Top suspect] actionable " + suspectBasis + L" candidate: reproduce after updating/reinstalling '" + s0.inferred_mod_name + L"'.")
        : (L"[유력 후보] 실행 우선 " + suspectBasis + L" 기반 후보: '" + s0.inferred_mod_name + L"' 모드 업데이트/재설치 후 재현 여부 확인"));
      r.recommendations.push_back(en
        ? (L"[Top suspect] If it repeats, disable the mod (or its SKSE plugin DLL) and retest: '" + s0.inferred_mod_name + L"'.")
        : (L"[유력 후보] 동일 문제가 반복되면 '" + s0.inferred_mod_name + L"' 모드(또는 해당 모드의 SKSE 플러그인 DLL)를 비활성화 후 재현 여부 확인"));
    } else if (!s0.module_filename.empty()) {
      r.recommendations.push_back(en
        ? (L"[Top suspect] actionable " + suspectBasis + L" candidate DLL: " + s0.module_filename + L" — check the providing mod first.")
        : (L"[유력 후보] 실행 우선 " + suspectBasis + L" 기반 후보 DLL: " + s0.module_filename + L" — 포함된 모드를 우선 점검"));
    }
  } else if (!hasActionableCandidates && allowTopSuspectActionRecommendations && (r.inferred_mod_name.empty() || preferStackCandidateOverFault) && topStackCandidateIsHookFramework) {
    r.recommendations.push_back(en
      ? L"[Top suspect] The current top stack candidate is a known hook framework DLL. Treat it as a victim location first and prioritize non-hook candidates/resources/conflicts."
      : L"[유력 후보] 현재 스택 1순위 후보가 알려진 훅 프레임워크 DLL입니다. 우선은 피해 위치로 보고, 비-훅 후보/리소스/충돌 단서를 먼저 점검하세요.");
  } else if (!hasActionableCandidates && allowTopSuspectActionRecommendations && (r.inferred_mod_name.empty() || preferStackCandidateOverFault) && topStackCandidateIsSystem) {
    r.recommendations.push_back(en
      ? L"[Top suspect] The current top stack candidate is a Windows system DLL. For hang captures this is often a waiting/victim location, so prioritize non-system candidates/resources/conflicts."
      : L"[유력 후보] 현재 스택 1순위 후보가 Windows 시스템 DLL입니다. 행 캡처에서는 대기/피해 위치인 경우가 많으므로 비-시스템 후보/리소스/충돌 단서를 우선 점검하세요.");
  }

  if (weakFaultLocationOnly) {
    std::vector<std::wstring> nearbyProbableDlls;
    const std::wstring faultLower = minidump::WideLower(r.fault_module_filename);
    for (const auto& module : r.crash_logger_top_modules) {
      // The SKSE runtime is the path plugin tasks take, not a suspect.
      if (minidump::WideLower(module) == faultLower || minidump::IsSkseModule(module)) {
        continue;
      }
      nearbyProbableDlls.push_back(module);
      if (nearbyProbableDlls.size() >= 4u) {
        break;
      }
    }
    if (!nearbyProbableDlls.empty()) {
      r.recommendations.push_back(en
        ? (L"[DLL guidance] This DLL is only the current fault-location clue. Compare nearby Crash Logger probable DLLs too: " +
            JoinList(nearbyProbableDlls, 4, L", ") + L".")
        : (L"[DLL 점검] 이 DLL은 예외가 난 위치라는 단서만 있습니다. Crash Logger가 함께 지목한 근처 DLL도 비교하세요: " +
            JoinList(nearbyProbableDlls, 4, L", ") + L"."));
    }
  }

  // A cell or worldspace only says where the player was; never advise
  // disabling the location mod on that alone (see FirstCulpritObjectRef).
  const auto* culpritRef = FirstCulpritObjectRef(r);
  if (!hasActionableCandidates && culpritRef) {
    const auto& topRef = *culpritRef;
    std::wstring espDesc = topRef.esp_name;
    if (!topRef.form_id.empty()) {
      espDesc += L" [" + topRef.form_id + L"]";
    }
    r.recommendations.push_back(en
      ? (L"[ESP/ESM] At crash time, an object from " + espDesc + L" was being processed. Try disabling this mod to check if the crash reproduces.")
      : (L"[ESP/ESM] 크래시 시점에 " + espDesc + L" 모드의 오브젝트가 처리 중이었습니다. 해당 모드를 비활성화하여 재현 여부를 확인하세요."));
  }

  if (!r.resources.empty()) {
    bool hasConflict = false;
    for (const auto& rr : r.resources) {
      if (rr.is_conflict) {
        hasConflict = true;
        break;
      }
    }

    r.recommendations.push_back(en
      ? L"[Mesh/Anim] This dump includes recent resource load history (.nif/.hkx/.tri). Check the 'Recent resources' section."
      : L"[메쉬/애니] 이 덤프에는 최근 로드된 리소스(.nif/.hkx/.tri) 기록이 포함되어 있습니다. '최근 로드된 리소스' 항목을 확인하세요.");
    if (hasConflict) {
      r.recommendations.push_back(en
        ? L"[Conflict] If multiple mods provide the same file, conflicts are common. Adjust MO2 priority / disable mods to retest."
        : L"[충돌] 같은 파일을 제공하는 모드가 2개 이상이면 충돌 가능성이 큽니다. MO2에서 우선순위(모드 순서) 조정/비활성화로 재현 여부 확인");
    }
  }

  // Stutter advice is for freezes and manual captures; a crash report gains
  // nothing from it.
  if (hitch.count > 0 && !isCrashLike) {
    r.recommendations.push_back(en
      ? L"[Performance] PerfHitch events were recorded. Check Events tab (t_ms and hitch(ms)) to see when the stutter happens."
      : L"[성능] PerfHitch 이벤트가 기록되었습니다. 이벤트 탭에서 t_ms와 hitch(ms)를 확인해 '언제 끊기는지' 먼저 파악하세요.");
    if (!r.resources.empty()) {
      r.recommendations.push_back(en
        ? L"[Performance] Check Resources tab for .nif/.hkx/.tri loaded right before/after the hitch, and their providing mods. (Correlation, not proof)"
        : L"[성능] 리소스 탭에서 히치 직전/직후 로드된 .nif/.hkx/.tri 및 제공 모드를 확인하세요. (상관관계 기반, 확정 아님)");
    }
  }

  if (allowTopSuspectActionRecommendations && hasModule && !isSystem && !isGameExe && !preferStackCandidateOverFault && allowFaultModuleTopSuspectRecommendations) {
    r.recommendations.push_back(en
      ? (topCandidateCrossValidatedFaultModule
          ? L"[Top suspect] Verify prerequisites/versions for the mod containing this DLL (SKSE / Address Library / game runtime)."
          : L"[DLL guidance] Verify prerequisites/versions for the mod containing this DLL before treating it as the root cause.")
      : (topCandidateCrossValidatedFaultModule
          ? L"[유력 후보] 해당 DLL이 포함된 모드의 선행 모드/요구 버전(SKSE/Address Library/엔진 버전) 충족 여부 확인"
          : L"[DLL 점검] 이 DLL을 바로 근본 원인으로 단정하기 전에, 포함된 모드의 선행 모드/요구 버전(SKSE/Address Library/엔진 버전)부터 확인하세요."));
    if (topCandidateCrossValidatedFaultModule) {
      r.recommendations.push_back(en
        ? L"[Top suspect] Attach this report (*_SkyrimDiagReport.txt) and dump (*.dmp) when reporting to the mod author."
        : L"[유력 후보] 이 리포트 파일(*_SkyrimDiagReport.txt)과 덤프(*.dmp)를 모드 제작자에게 첨부");
    }
  } else if (allowTopSuspectActionRecommendations && hasModule && isGameExe) {
    if (FirstCulpritObjectRef(r)) {
      // ESP/ESM reference is the primary clue for EXE crashes — already emitted above as [ESP/ESM]
      if (hasStackCandidate && !r.suspects_from_stackwalk) {
        // Stack scan suspect exists but is weak — note it as supplementary
        r.recommendations.push_back(en
          ? (L"[Stack scan] " + r.suspects[0].module_filename + L" was also detected nearby via stack scan, but this is a weak signal. Prioritize the ESP/ESM clue above.")
          : (L"[스택 스캔] 스택 스캔에서 " + r.suspects[0].module_filename + L"도 감지되었으나 신뢰도가 낮습니다. 위의 ESP/ESM 단서를 우선 점검하세요."));
      }
    }
    // A fault in the game executable says where it failed, not why. Engine
    // code that trips over bad plugin data (the v0.2.59 field case: a trap
    // hitting a modded creature, every frame vanilla) is at least as common
    // as a version mismatch or a hook, so do not single those out.
    r.recommendations.push_back(en
      ? L"[Check] The crash is in the game executable. That alone does not tell the cause: the engine may have tripped over plugin data (ESP/ESM, meshes) it was processing, or an Address Library/SKSE version mismatch or a DLL hook may be involved."
      : L"[점검] 크래시 위치가 게임 본체(EXE)입니다. 이것만으로는 원인을 가릴 수 없습니다. 엔진이 처리하던 플러그인 데이터(ESP/ESM·메시) 문제일 수도, Address Library/SKSE 버전 불일치나 DLL 훅 문제일 수도 있습니다.");
    r.recommendations.push_back(en
      ? L"[Check] Disable recently added/updated SKSE plugin DLLs one by one and retest."
      : L"[점검] 최근 추가/업데이트한 SKSE 플러그인(DLL)부터 하나씩 제외하며 재현 여부 확인");
  } else if (allowTopSuspectActionRecommendations && hasModule && isSystem) {
    if (!ctx.externalSoftware.empty()) {
      // Not a mod: what the player can do is update it or turn it off.
      r.recommendations.push_back(en
        ? (L"[External software] The crash is inside " + r.fault_module_filename + L" from " + ctx.externalSoftware +
            L", a program outside the game and its mods. Update or clean-reinstall it (for a GPU vendor, the driver and its app), "
            L"or turn off its overlay and in-game features, then retest.")
        : (L"[외부 프로그램] 크래시가 " + ctx.externalSoftware + L"의 " + r.fault_module_filename +
            L" 안에서 났습니다. 게임과 모드 밖의 프로그램입니다. 업데이트하거나 깨끗이 다시 설치하고(그래픽 카드 제조사라면 드라이버와 그 앱), "
            L"오버레이와 게임 내 기능을 끈 뒤 다시 시험해 보세요."));
      r.recommendations.push_back(en
        ? L"[Check] If it keeps crashing there, another mod/DLL may be corrupting memory that this program touches."
        : L"[점검] 계속 같은 곳에서 난다면 다른 모드/DLL이 이 프로그램이 쓰는 메모리를 망가뜨렸을 수도 있습니다.");
    } else {
      r.recommendations.push_back(en
        ? L"[Check] When a Windows system DLL is shown, the real culprit is often another mod/DLL."
        : L"[점검] Windows 시스템 DLL로 표시될 때는 실제 원인이 다른 모드/DLL인 경우가 많습니다.");
    }
    r.recommendations.push_back(en
      ? L"[Check] Disable recently added/updated SKSE plugin DLLs one by one and retest."
      : L"[점검] 최근 추가/업데이트한 SKSE 플러그인(DLL)부터 하나씩 제외하며 재현 여부 확인");
    r.recommendations.push_back(en
      ? L"[Check] Verify SKSE version, game runtime (AE/SE/VR), and Address Library all match."
      : L"[점검] SKSE 버전/게임 버전(AE/SE/VR)/Address Library 버전이 서로 맞는지 확인");
  } else {
    // Only a crash has a faulting module to look for; a freeze has none. Nor
    // is a richer dump the next step when the game was being closed anyway
    // (ADR-0022) or the decoded jump to no code names its call (ADR-0019).
    const bool jumpFromKnownCall = FaultIsJumpToNoCode(r) && !r.fault_instruction.call_site.empty();
    if (isCrashLike && !ctx.isForcedClose && !jumpFromKnownCall) {
    r.recommendations.push_back(en
      ? L"[Check] Fault module could not be determined. Capture again with a richer crash recapture profile (full memory info / module headers / indirect memory) before escalating to FullMemory (DumpMode=2)."
      : L"[점검] 덤프에서 예외가 난 모듈을 특정하지 못했습니다. 바로 FullMemory(DumpMode=2)로 가지 말고 더 많은 정보를 담는 재수집 설정(full memory info / module headers / indirect memory)으로 먼저 다시 캡처하세요.");
    }
  }

  // Crash advice; a freeze or snapshot taken on a loading screen is not one.
  if (isCrashLike && (r.state_flags & skydiag::kState_Loading) != 0u) {
    r.recommendations.push_back(en
      ? L"[Loading] Crashes right after load screens often involve animation/mesh/texture/skeleton/script initialization."
      : L"[로딩 중] 로딩 화면/세이브 로드 직후 크래시는 애니메이션/메쉬/텍스처/스켈레톤/스크립트 초기화 쪽이 흔합니다.");
    r.recommendations.push_back(en
      ? L"[Loading] Check mods affecting that stage first (animations/skeleton/body/physics/precaching)."
      : L"[로딩 중] 해당 시점에 개입하는 모드(애니메이션/스켈레톤/바디/물리/프리캐시)를 우선 점검");
  }

  if (r.freeze_analysis.has_analysis) {
    // ADR-0009: say what the main thread was doing before any module triage,
    // unless a deadlock, a module-level stall or a loader stall already
    // explains the freeze (those come first, and the wait is still evidence).
    const auto& wait = r.main_thread_wait;
    const auto& stateId = r.freeze_analysis.state_id;
    const bool stallExplained = stateId == "deadlock_likely" || stateId == "synchronization_stall_likely" ||
      stateId == "loader_stall_likely" || r.hang_thread_module_consensus.has_consensus;
    if (isHangLike && stateId == "engine_lock_deadlock") {
      // ADR-0021: the deadlocked threads themselves are read; a known pattern
      // adds its own [Known cause] fix.
      const std::wstring tids = JoinThreadIds(EngineLockDeadlockThreads(r.engine_lock_deadlock));
      r.recommendations.push_back(en
        ? (L"[Main thread] Game threads " + tids +
            L" deadlocked on the game engine's own locks. No mod is shown to have caused it: do not remove mods only "
            L"because they are on these threads' stacks. If it keeps happening, look for an engine fix mod for this "
            L"freeze for your game version and share this report when asking for help.")
        : (L"[메인 스레드] 게임 스레드 " + tids +
            L"이(가) 게임 엔진 자체 잠금에서 교착 상태에 빠졌습니다. 어느 모드가 일으켰다는 근거는 없으니 이 스레드들 "
            L"스택에 있다는 이유만으로 모드를 빼지 마세요. 계속 반복되면 지금 게임 버전에서 이 프리징을 고치는 엔진 수정 "
            L"모드가 있는지 찾아보고, 도움을 요청할 때 이 리포트를 공유하세요."));
    } else if (isHangLike && r.engine_spin.nan_detected) {
      // engine_spin_nan comes first: the stuck thread itself is read.
      const auto& mods = r.engine_spin.call_path_modules;
      const std::wstring path = mods.empty() ? std::wstring{} : JoinList(mods, mods.size(), L", ");
      r.recommendations.push_back(en
        ? (L"[Main thread] A value the game was calculating became NaN (not a number), and the engine looped on it "
            L"forever (" + EngineSpinLocation(r.engine_spin) +
            L"). Check the mods you recently added or updated that change combat, animation, AI/pathing or how actors "
            L"move, disabling them one at a time." +
            (path.empty() ? std::wstring{}
                          : (L" The plugins on the stuck thread's stack (" + path +
                              L") were only on the call path; do not remove them on this evidence alone.")))
        : (L"[메인 스레드] 게임 계산 값이 숫자가 아닌 값(NaN)이 되어 엔진이 같은 계산을 끝없이 반복했습니다(" +
            EngineSpinLocation(r.engine_spin) +
            L"). 최근에 추가하거나 업데이트한 모드 중 전투, 애니메이션, AI/길찾기, 캐릭터 움직임을 바꾸는 모드를 하나씩 꺼 보며 "
            L"확인하세요." +
            (path.empty() ? std::wstring{}
                          : (L" 멈춘 스레드 스택의 플러그인(" + path + L")은 호출 경로에 있었을 뿐이므로 이 근거만으로 빼지 마세요."))));
    } else if (stallExplained) {
      // No [Main thread] advice.
    } else if (isHangLike && r.main_thread_lock_owner.confirmed &&
               IsGraphicsStackModule(r.main_thread_lock_owner.top_module)) {
      // The thread holding the lock was itself in the graphics driver.
      const auto& owner = r.main_thread_lock_owner;
      const auto& injectors = r.graphics_env.injection_modules;
      const std::wstring detected = injectors.empty()
        ? std::wstring{}
        : (en ? (L" (detected: " + JoinList(injectors, injectors.size(), L", ") + L")")
              : (L" (감지됨: " + JoinList(injectors, injectors.size(), L", ") + L")"));
      const auto& path = wait.path_modules;
      const std::wstring mods = path.empty() ? std::wstring{} : JoinList(path, path.size(), L", ");
      r.recommendations.push_back(en
        ? (L"[Main thread] The main thread was waiting for a " + DescribeLockType(owner, en) + L" held by " +
            DescribeLockOwnerThread(owner, en) + L", and that thread was inside the graphics driver (" + owner.top_module +
            L"). Update or clean-install the GPU driver, then test without graphics injectors, upscalers and frame generation" +
            detected + L"." +
            (mods.empty() ? std::wstring{}
                          : (L" The plugins on the main thread's stack (" + mods + L") only called into the wait.")))
        : (L"[메인 스레드] 메인 스레드는 " + DescribeLockOwnerThread(owner, en) + L"이(가) 쥔 " + DescribeLockType(owner, en) +
            L" 잠금을 기다리고 있었고, 그 스레드는 그래픽 드라이버(" + owner.top_module +
            L") 안에 있었습니다. GPU 드라이버를 업데이트하거나 클린 설치한 뒤, 그래픽 인젝터·업스케일러·프레임 생성" + detected +
            L" 없이 시험해 보세요." +
            (mods.empty() ? std::wstring{}
                          : (L" 메인 스레드 스택의 플러그인(" + mods + L")은 그 대기를 호출했을 뿐입니다."))));
    } else if (isHangLike && r.main_thread_lock_owner.confirmed) {
      // One WCT pass is not enough to send the player away from the stack.
      const auto& owner = r.main_thread_lock_owner;
      r.recommendations.push_back(en
        ? (L"[Main thread] The main thread was waiting for a " + DescribeLockType(owner, en) + L" held by " +
            DescribeLockOwnerThread(owner, en) +
            L". Look at what that thread runs rather than at the plugins on the main thread's stack; if the freeze repeats, "
            L"take a new capture while frozen and check whether the same thread holds the lock.")
        : (L"[메인 스레드] 메인 스레드는 " + DescribeLockOwnerThread(owner, en) + L"이(가) 쥔 " + DescribeLockType(owner, en) +
            L" 잠금을 기다리고 있었습니다. 메인 스레드 스택의 플러그인보다 그 스레드가 실행하던 모듈을 먼저 확인하세요. "
            L"프리징이 반복되면 멈춘 상태에서 새로 캡처해 같은 스레드가 잠금을 쥐고 있는지 비교하세요."));
    } else if (isHangLike && IsGpuWait(wait)) {
      // The injector DLLs found in the game folder, by file name: the same
      // d3d11.dll/dxgi.dll pair can be ENB, ReShade or DXVK.
      const auto& injectors = r.graphics_env.injection_modules;
      const std::wstring detected = injectors.empty()
        ? std::wstring{}
        : (en ? (L" (detected: " + JoinList(injectors, injectors.size(), L", ") + L")")
              : (L" (감지됨: " + JoinList(injectors, injectors.size(), L", ") + L")"));
      const bool inDriver = wait.kind == "graphics_driver_wait";
      const std::wstring where = en
        ? (inDriver ? (L"was waiting in the graphics driver (" + wait.waiting_module + L")")
                    : std::wstring(L"was waiting for the GPU (the engine was polling a Direct3D query)"))
        : (inDriver ? (L"그래픽 드라이버(" + wait.waiting_module + L") 안에서 대기 중이었습니다")
                    : std::wstring(L"GPU를 기다리고 있었습니다(엔진이 Direct3D 쿼리 결과를 기다림)"));
      r.recommendations.push_back(en
        ? (L"[Main thread] The main thread " + where +
            L". Update or clean-install the GPU driver, then test without graphics injectors and upscalers" + detected +
            L" and with lighter texture/video-memory load.")
        : (L"[메인 스레드] 메인 스레드가 " + where +
            L". GPU 드라이버를 업데이트하거나 클린 설치한 뒤, 그래픽 인젝터·업스케일러" + detected +
            L" 없이, 그리고 텍스처/비디오 메모리 부담을 줄여서 시험해 보세요."));
    } else if (isHangLike && HasBlockedGameThread(wait)) {
      const std::wstring who = wait.blocked_mod_name.empty()
        ? wait.blocked_module
        : (wait.blocked_mod_name + L" (" + wait.blocked_module + L")");
      r.recommendations.push_back(en
        ? (L"[Main thread] The main thread was waiting inside the game engine for another game thread, and that thread was inside " +
            who + L" (top: " + wait.blocked_top + L"). Disable or update " + who + L" and reproduce the same situation.")
        : (L"[메인 스레드] 메인 스레드는 게임 엔진 안에서 다른 게임 스레드를 기다리고 있었고, 그 스레드는 " + who +
            L" 코드 안에 있었습니다(맨 위: " + wait.blocked_top + L"). " + who + L"을(를) 끄거나 업데이트한 뒤 같은 상황을 재현해 보세요."));
    } else if (isHangLike && HasCallPathGameThread(wait)) {
      const auto& below = wait.blocked_path_modules;
      const std::wstring path = below.empty()
        ? std::wstring{}
        : ((en ? L" (" : L"(") + JoinList(below, below.size(), L", ") + L")");
      r.recommendations.push_back(en
        ? (L"[Main thread] The main thread was waiting inside the game engine for another game thread, and that thread "
            L"was running game engine code (top: " + wait.blocked_top + L"); the plugins further down its stack" + path +
            L" were only on the call path. Do not remove them on this evidence alone; if the freeze repeats, capture it "
            L"again while frozen (Ctrl+Shift+Backspace by default) and compare what stays the same.")
        : (L"[메인 스레드] 메인 스레드는 게임 엔진 안에서 다른 게임 스레드를 기다리고 있었고, 그 스레드는 게임 엔진 코드를 "
            L"실행하고 있었습니다(맨 위: " + wait.blocked_top + L"). 그 스레드 스택 아래쪽 플러그인" + path +
            L"은 호출 경로에 있었을 뿐이니 이 근거만으로 빼지 마세요. 프리징이 반복되면 멈춘 상태에서 다시 캡처(기본 "
            L"Ctrl+Shift+Backspace)해 공통점을 비교하세요."));
    } else if (isHangLike && wait.kind == "engine_wait") {
      const std::wstring path = wait.path_modules.empty()
        ? std::wstring{}
        : ((en ? L" (" : L"(") + JoinList(wait.path_modules, wait.path_modules.size(), L", ") + L")");
      r.recommendations.push_back(en
        ? (L"[Main thread] The main thread was waiting inside the game engine, not running mod code. Do not remove the plugins on its stack" +
            path + L" on this evidence alone; if the freeze repeats, capture it again while frozen (Ctrl+Shift+Backspace by default) and compare what stays the same.")
        : (L"[메인 스레드] 메인 스레드는 모드 코드를 실행하던 것이 아니라 게임 엔진 안에서 대기 중이었습니다. 이 근거만으로 스택의 플러그인" +
            path + L"을 빼지 마세요. 프리징이 반복되면 멈춘 상태에서 다시 캡처(기본 Ctrl+Shift+Backspace)해 공통점을 비교하세요."));
    }
    if (isHangLike) {
      const auto& state = r.blackbox_freeze_summary;
      if (std::find(state.open_menus.begin(), state.open_menus.end(), L"Console") != state.open_menus.end()) {
        r.recommendations.push_back(en
          ? L"[Context] The Console was open when the game froze. If a console command was run just before, check whether that command alone stalls the game."
          : L"[상황] 프리징 당시 콘솔이 열려 있었습니다. 직전에 콘솔 명령을 실행했다면 그 명령만으로 게임이 멈추는지 확인해 보세요.");
      }
      if (state.pause_gap_seconds > 0.0) {
        r.recommendations.push_back(en
          ? (L"[Context] A pause of " + DescribeDuration(state.pause_gap_seconds, en) + L" (PC asleep or the game minimized) ended " +
              DescribeDuration(state.pause_gap_ended_seconds_before, en) +
              L" before this freeze. If freezes mostly come after a resume, close the game before the PC sleeps and see whether they stop.")
          : (L"[상황] " + DescribeDuration(state.pause_gap_seconds, en) + L" 동안의 정지(PC 절전이나 게임 최소화)가 이 프리징 " +
              DescribeDuration(state.pause_gap_ended_seconds_before, en) +
              L" 전에 끝났습니다. 프리징이 주로 절전 해제 뒤에 난다면, PC를 절전시키기 전에 게임을 끄고 계속 나는지 보세요."));
      }
    }
    // A manual capture of a running game is not a freeze ("[Manual]" says so);
    // freeze-state advice would contradict it.
    if (!isHangLike) {
      // No freeze-state advice.
    } else if (r.freeze_analysis.state_id == "deadlock_likely") {
      r.recommendations.push_back(en
        ? L"[Freeze] WCT cycle evidence makes deadlock the primary interpretation. Check synchronization-heavy mods and thread ownership first."
        : L"[프리징] WCT cycle 근거가 있어 데드락 해석이 우선입니다. 동기화/후킹 성격이 강한 모드와 스레드 소유 관계를 먼저 확인하세요.");
      if (r.freeze_analysis.support_quality == "snapshot_consensus_backed") {
        r.recommendations.push_back(en
          ? L"[Freeze] Repeated WCT captures preserved the same cycle picture. Prioritize the repeated ownership chain before broad busy-wait triage."
          : L"[프리징] 반복 WCT 캡처에서 같은 cycle 구도가 유지됐습니다. 광범위한 busy-wait 점검보다 반복되는 소유 체인을 먼저 확인하세요.");
      }
    } else if (r.freeze_analysis.state_id == "loader_stall_likely") {
      r.recommendations.push_back(en
        ? L"[Freeze] Loading-context evidence points to a loader stall. Prioritize mesh/animation/physics/precache changes before generic DLL triage."
        : L"[프리징] 로딩 문맥 근거가 있어 loader stall 가능성이 큽니다. 일반 DLL 점검보다 메쉬/애니메이션/물리/프리캐시 변경을 먼저 보세요.");
      if (r.freeze_analysis.support_quality == "snapshot_consensus_backed") {
        r.recommendations.push_back(en
          ? L"[Freeze] Repeated WCT captures stayed aligned with the loading window. Recheck the same initialization path before widening freeze triage."
          : L"[프리징] 반복 WCT 캡처가 같은 로딩 윈도우와 계속 일치했습니다. 프리징 점검 범위를 넓히기 전에 같은 초기화 경로를 먼저 다시 확인하세요.");
      }
      if (!r.freeze_analysis.blackbox_context.recent_non_system_modules.empty()) {
        r.recommendations.push_back(en
          ? (L"[Freeze] blackbox module churn highlighted recent non-system modules: " +
              JoinList(r.freeze_analysis.blackbox_context.recent_non_system_modules, 4, L", ") +
              L". Check them before widening DLL/plugin triage.")
          : (L"[프리징] blackbox module churn에서 최근 비시스템 모듈이 강조되었습니다: " +
              JoinList(r.freeze_analysis.blackbox_context.recent_non_system_modules, 4, L", ") +
              L". 광범위한 DLL/플러그인 점검 전에 이 후보들을 먼저 확인하세요."));
      }
      if (r.freeze_analysis.first_chance_context.repeated_signature_count > 0u) {
        r.recommendations.push_back(en
          ? L"[Freeze] repeated suspicious first-chance exceptions strengthen the loader-stall interpretation. Recheck the highlighted initialization path before broad deadlock triage."
          : L"[프리징] 반복 suspicious first-chance 예외가 loader stall 해석을 강화합니다. 광범위한 데드락 점검 전에 강조된 초기화 경로를 다시 확인하세요.");
      }
      if (!r.freeze_analysis.first_chance_context.recent_non_system_modules.empty()) {
        r.recommendations.push_back(en
          ? (L"[Freeze] first-chance context highlighted recent non-system modules: " +
              JoinList(r.freeze_analysis.first_chance_context.recent_non_system_modules, 4, L", ") +
              L". Compare them against the current loading pipeline first.")
          : (L"[프리징] first-chance 문맥에서 최근 비시스템 모듈이 강조되었습니다: " +
              JoinList(r.freeze_analysis.first_chance_context.recent_non_system_modules, 4, L", ") +
              L". 현재 로딩 파이프라인과 먼저 대조하세요."));
      }
    } else if (r.freeze_analysis.state_id == "freeze_candidate") {
      r.recommendations.push_back(en
        ? L"[Freeze] Signals indicate a real freeze, but not a clean deadlock/stall classification yet. Compare related candidates and repeat capture during the issue."
        : L"[프리징] 실제 프리징 신호는 있으나 데드락/로더 stall로 깔끔하게 분류되지는 않았습니다. 관련 후보를 비교하고 문제 상황에서 다시 캡처하세요.");
    } else if (r.freeze_analysis.state_id == "freeze_ambiguous") {
      r.recommendations.push_back(en
        ? L"[Freeze] Current freeze evidence is ambiguous. Prefer another capture during the issue and compare WCT/events before escalating."
        : L"[프리징] 현재 프리징 근거가 애매합니다. 확대 해석 전에 문제 상황에서 다시 캡처해 WCT/이벤트와 비교하세요.");
    }

    if (r.freeze_analysis.support_quality == "snapshot_fallback") {
      r.recommendations.push_back(en
        ? L"[Freeze] PSS snapshot was requested but capture fell back to live-process transport. Treat weak freeze conclusions more conservatively."
        : L"[프리징] PSS snapshot을 요청했지만 live-process로 fallback되었습니다. 약한 프리징 결론은 더 보수적으로 해석하세요.");
    }
  }

  if (r.has_wct) {
    if (isHangLike) {
      if (wct) {
        if (wct->cycles > 0) {
          r.recommendations.push_back(en
            ? L"[Hang] WCT detected isCycle=true thread(s). Deadlock is likely."
            : L"[프리징] WCT에서 isCycle=true 스레드가 감지되었습니다. 데드락 가능성이 높습니다.");
        } else if (r.main_thread_lock_owner.tid == 0u && !HasBlockedGameThread(r.main_thread_wait)) {
          // A main thread blocked on a lock WCT can name, or waiting for a
          // game thread stuck in a plugin, is no busy wait.
          r.recommendations.push_back(en
            ? L"[Hang] No WCT cycle: possible infinite loop / busy wait."
            : L"[프리징] WCT cycle이 없으면 무한루프/바쁜 대기(busy wait) 가능성도 있습니다.");
        }
      }
      if (!r.crash_logger_thread_dump_deadlock.detected) {
        r.recommendations.push_back(en
          ? L"[Hang] If it repeats, use Events tab (just before the freeze) to narrow related mods."
          : L"[프리징] 프리징이 반복되면 문제 상황 직전에 실행된 이벤트(이벤트 탭)를 기준으로 관련 모드를 점검");
      }
    } else if (isManualCapture && isSnapshotLike) {
      // The freeze verdict's rule (FreezeCandidateConsensus.cpp): under the
      // helper's threshold alone a load can still be stuck.
      if (wct && wct->has_capture && wct->thresholdSec > 0u &&
          wct->secondsSinceHeartbeat < internal::kNotHangHeartbeatAgeSec) {
        // No fixed buffer: the heartbeat age comes from the dump's WCT stream
        // (a crafted -1e308 is over 300 digits).
        const std::wstring age = FormatDecimal(wct->secondsSinceHeartbeat, 1);
        const std::wstring limit = FormatDecimal(internal::kNotHangHeartbeatAgeSec, 0);
        r.recommendations.push_back(en
          ? (L"[Manual] At capture time, heartbeatAge=" + age + L"s (under " + limit +
              L"s): the main thread was still responding, so it is not considered a hang.")
          : (L"[수동] 수동 캡처 당시 heartbeatAge=" + age + L"s(" + limit +
              L"초 미만)로 메인 스레드가 아직 응답하고 있었으므로 '프리징/무한로딩'으로 판단되지 않습니다."));
      }
      r.recommendations.push_back(en
        ? L"[Manual] Manual captures include WCT. For real freezes/infinite loading, check the WCT tab from a capture taken during the issue."
        : L"[수동] 수동 캡처에는 WCT가 포함됩니다. 실제 프리징/무한로딩 중 캡처한 덤프에서 WCT 탭을 참고하세요.");
    }
  }

  // Memory near a limit makes allocations fail (a crash) or makes Windows
  // move the game's textures out of VRAM (stutter, freezes). It names no mod,
  // so it never outranks a candidate.
  if (const MemoryPressure pressure = AssessMemoryPressure(r.memory); pressure.Any()) {
    if (pressure.vram_high) {
      const std::wstring amount = FormatGb(r.memory.vram_usage_bytes, 2) + L"/" + FormatGb(r.memory.vram_budget_bytes, 2) + L" GB";
      r.recommendations.push_back(en
        ? (L"[Memory] At capture time the game was using " + std::to_wstring(pressure.vram_percent) +
            L"% of its VRAM budget (" + amount + L"). Running out of VRAM can crash or freeze the game. "
            L"Lower texture resolution (texture packs, ENB/Community Shaders settings, upscaler) and retest.")
        : (L"[메모리] 캡처 당시 게임이 VRAM 예산의 " + std::to_wstring(pressure.vram_percent) +
            L"%(" + amount + L")를 쓰고 있었습니다. VRAM이 모자라면 크래시나 프리징이 날 수 있습니다. "
            L"텍스처 해상도(텍스처 모드, ENB/Community Shaders 설정, 업스케일러)를 낮춰 재현 여부를 확인하세요."));
    }
    if (pressure.commit_high) {
      const std::wstring amount = FormatGb(r.memory.commit_used_bytes, 1) + L"/" + FormatGb(r.memory.commit_limit_bytes, 1) + L" GB";
      r.recommendations.push_back(en
        ? (L"[Memory] At capture time system commit was at " + std::to_wstring(pressure.commit_percent) +
            L"% of its limit (" + amount + L"). When it runs out, allocations fail and the game crashes. "
            L"Close other programs or enlarge the page file (or let Windows manage its size), then retest.")
        : (L"[메모리] 캡처 당시 시스템 커밋이 한도의 " + std::to_wstring(pressure.commit_percent) +
            L"%(" + amount + L")였습니다. 한도에 닿으면 메모리 할당이 실패해 게임이 튕깁니다. "
            L"다른 프로그램을 닫거나 페이지 파일을 늘린(또는 시스템이 관리하도록 둔) 뒤 재현 여부를 확인하세요."));
    }
  }

  appendLowConfidencePluginRecommendations();

  if (!r.troubleshooting_steps.empty()) {
    const std::wstring steps = std::to_wstring(r.troubleshooting_steps.size());
    const wchar_t* kindEn = isCrashLike ? L"crash type" : (isHangLike ? L"freeze type" : L"capture");
    const wchar_t* kindKo = isCrashLike ? L"이 크래시 유형" : (isHangLike ? L"이 프리징 유형" : L"이 상황");
    r.recommendations.push_back(en
      ? (L"[Troubleshooting] See the troubleshooting checklist (" + steps + L" steps) for this " + kindEn + L".")
      : (L"[트러블슈팅] " + std::wstring(kindKo) + L"에 대한 단계별 체크리스트(" + steps + L"단계)를 확인하세요."));
  }
  }

}  // namespace skydiag::dump_tool::internal
