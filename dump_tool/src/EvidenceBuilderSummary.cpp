#include "EvidenceBuilderPrivate.h"
#include "EvidenceBuilderEvidencePipeline.h"

#include <algorithm>
#include <cwchar>

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

bool CandidateHasFrameSupport(const ActionableCandidate& candidate)
{
  return CandidateHasFamily(candidate, "crash_logger_frame");
}

bool CandidateHasScorableFirstChanceSupport(const AnalysisResult& r, const ActionableCandidate& candidate)
{
  return CandidateHasFamily(candidate, "first_chance_context") &&
         HasScorableFirstChanceContext(r.first_chance_summary);
}

bool CandidateHasScorableHistorySupport(const AnalysisResult& r, const ActionableCandidate& candidate)
{
  return CandidateHasFamily(candidate, "history_repeat") &&
         (r.history_correlation.count > 1 || !r.bucket_candidate_repeats.empty());
}

std::wstring DescribeFrameSupport(const AnalysisResult& r, const ActionableCandidate& candidate, bool en)
{
  if (CandidateMatchesModule(candidate, r.crash_logger_direct_fault_module)) {
    return en ? L"Crash Logger frame first (direct DLL fault)"
              : L"Crash Logger frame first (direct DLL fault)";
  }
  if (CandidateMatchesModule(candidate, r.crash_logger_first_actionable_probable_module)) {
    return en ? L"Crash Logger frame first (first actionable probable DLL frame)"
              : L"Crash Logger frame first (첫 actionable probable DLL frame)";
  }
  if (CandidateMatchesModule(candidate, r.crash_logger_probable_streak_module)) {
    return en ? L"Crash Logger frame first (probable frame streak)"
              : L"Crash Logger frame first (probable frame streak)";
  }
  return en ? L"Crash Logger frame first" : L"Crash Logger frame first";
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
      labels.push_back(en ? L"near resource provider" : L"인접 리소스 provider");
    } else if (family == "history_repeat") {
      labels.push_back(en ? L"history repeat" : L"반복 기록");
    } else if (family == "first_chance_context") {
      labels.push_back(en ? L"repeated first-chance context" : L"반복 first-chance 문맥");
    }
  }
  return labels.empty() ? (en ? L"limited evidence" : L"제한된 근거") : JoinList(labels, labels.size(), L" + ");
}

}  // namespace

std::wstring BuildSummarySentence(const AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx)
{
  const bool en = ctx.en;
  if (r.signature_match.has_value()) {
    const auto& sig = *r.signature_match;
    return en
      ? (L"Known pattern [" + ToWideAscii(sig.id) + L"]: " + sig.cause + L" (Confidence: " + sig.confidence + L")")
      : (L"알려진 패턴 [" + ToWideAscii(sig.id) + L"]: " + sig.cause + L" (신뢰도: " + sig.confidence + L")");
  }

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
    (minidump::IsSystemishModule(topSuspect->module_filename) || minidump::IsLikelyWindowsSystemModulePath(topSuspect->module_path));
  const ActionableCandidate* topCandidate = !r.actionable_candidates.empty() ? &r.actionable_candidates[0] : nullptr;
  const ActionableCandidate* secondCandidate = (r.actionable_candidates.size() > 1u) ? &r.actionable_candidates[1] : nullptr;
  const std::wstring topCandidateConf = topCandidate && !topCandidate->confidence.empty()
    ? topCandidate->confidence
    : ConfidenceText(lang, topCandidate ? topCandidate->confidence_level : i18n::ConfidenceLevel::kUnknown);
  const bool topCandidateHasFrame = topCandidate && CandidateHasFrameSupport(*topCandidate);
  const bool topCandidateHasObjectRef = topCandidate && CandidateHasFamily(*topCandidate, "crash_logger_object_ref");
  const bool topCandidateBackedByFrame =
    topCandidate &&
    (CandidateMatchesModule(*topCandidate, r.crash_logger_direct_fault_module) ||
     CandidateMatchesModule(*topCandidate, r.crash_logger_first_actionable_probable_module) ||
     CandidateMatchesModule(*topCandidate, r.crash_logger_probable_streak_module));

  std::wstring who;
  if (!r.inferred_mod_name.empty()) {
    who = r.inferred_mod_name + L" (" + r.fault_module_filename + L")";
  } else if (!r.fault_module_filename.empty()) {
    who = r.fault_module_filename;
  } else {
    who = en ? L"(unknown)" : L"(알 수 없음)";
  }

  std::wstring summary;

  if (isSnapshotLike) {
    summary = isManualCapture
      ? (en
          ? L"Looks like a manual snapshot. This alone does not prove a problem. (Confidence: High)"
          : L"수동 캡처 스냅샷으로 보입니다. 이 결과만으로 '문제가 있다'고 단정할 수 없습니다. (신뢰도: 높음)")
      : (en
          ? L"Looks like a snapshot dump (not a crash/hang). Useful for state inspection, not root cause. (Confidence: High)"
          : L"스냅샷 덤프(크래시/행 아님)로 보입니다. 원인 판정용이 아니라 '상태 확인'에 유용합니다. (신뢰도: 높음)");
  } else if (hasModule && !isSystem && !isGameExe && ctx.isHookFramework) {
    if (hasNonHookSuspect && !nonHookSuspectWho.empty()) {
      summary = en
        ? (L"Crash is reported in " + who + L" (known hook framework), but " + suspectBasis + L" points to " + nonHookSuspectWho +
            L". (Confidence: " + nonHookSuspectConf + L")")
        : (L"크래시 위치가 " + who + L"(알려진 훅 프레임워크)로 보고되었지만, " + suspectBasis + L"에서는 " + nonHookSuspectWho +
            L" 가 유력합니다. (신뢰도: " + nonHookSuspectConf + L")");
    } else {
      summary = en
        ? (L"Crash is reported in " + who + L" (known hook framework). This is often a victim location, so avoid treating it as a standalone root cause. (Confidence: Low)")
        : (L"크래시 위치가 " + who + L"(알려진 훅 프레임워크)로 보고되었습니다. 이 경우 피해 위치로 잡히는 일이 많아 단독 원인으로 단정하기 어렵습니다. (신뢰도: 낮음)");
    }
  } else if (hasModule && !isSystem && !isGameExe) {
    if (!r.crash_logger_direct_fault_module.empty() &&
        minidump::WideLower(r.crash_logger_direct_fault_module) == minidump::WideLower(r.fault_module_filename)) {
      summary = en
        ? (L"Top suspect: " + who + L" — Crash Logger frame first and direct DLL fault both point inside this DLL. (Confidence: High)")
        : (L"유력 후보: " + who + L" — Crash Logger frame first 와 direct DLL fault 가 모두 이 DLL 내부를 가리킵니다. (신뢰도: 높음)");
    } else {
      summary = en
        ? (L"Top suspect: " + who + L" — the crash appears to occur inside this DLL. (Confidence: High)")
        : (L"유력 후보: " + who + L" — 해당 DLL 내부에서 크래시가 발생한 것으로 보입니다. (신뢰도: 높음)");
    }
  } else if (hasModule && isSystem) {
    if (topCandidate && topCandidate->status_id == "cross_validated") {
      const auto candidateName = DescribeCandidate(*topCandidate);
      const auto families = JoinCandidateFamilies(*topCandidate, en);
      summary = en
        ? (L"Crash is reported in a Windows system DLL, but the strongest actionable candidate is " + candidateName +
            L" (" + families + L"). (Confidence: " + topCandidateConf + L")")
        : (L"크래시가 Windows 시스템 DLL에서 보고되었지만, 가장 강한 실행 우선 후보는 " + candidateName +
            L" 입니다. (" + families + L", 신뢰도: " + topCandidateConf + L")");
    } else if (topCandidate && topCandidateHasFrame && topCandidateBackedByFrame) {
      const auto candidateName = DescribeCandidate(*topCandidate);
      const auto frameSupport = DescribeFrameSupport(r, *topCandidate, en);
      const bool hasFirstChanceSupport = CandidateHasScorableFirstChanceSupport(r, *topCandidate);
      const bool hasHistorySupport = CandidateHasScorableHistorySupport(r, *topCandidate);
      summary = en
        ? (L"Crash is reported in a Windows system DLL, but " + frameSupport + L" points to DLL candidate " + candidateName +
            L". This is stronger than an isolated object ref." +
            (hasFirstChanceSupport ? L" Repeated suspicious first-chance context also matched this candidate." : L"") +
            (hasHistorySupport ? L" Repeated crash bucket history also matched this candidate." : L"") +
            L" (Confidence: " + topCandidateConf + L")")
        : (L"크래시가 Windows 시스템 DLL에서 보고되었지만, " + frameSupport + L" 가 DLL 후보 " + candidateName +
            L" 를 가리킵니다. 이는 단독 object ref 보다 강한 신호입니다." +
            (hasFirstChanceSupport ? L" 반복 suspicious first-chance 문맥도 이 후보와 맞습니다." : L"") +
            (hasHistorySupport ? L" 반복 크래시 버킷 이력도 이 후보와 맞습니다." : L"") +
            L" (신뢰도: " + topCandidateConf + L")");
    } else if (hasNonHookSuspect && !nonHookSuspectWho.empty()) {
      summary = en
        ? (L"Crash is reported in a Windows system DLL, but " + suspectBasis + L" points to " + nonHookSuspectWho +
            L". (Confidence: " + nonHookSuspectConf + L")")
        : (L"크래시가 Windows 시스템 DLL에서 보고되었지만, " + suspectBasis + L"에서는 " + nonHookSuspectWho +
            L" 가 유력합니다. (신뢰도: " + nonHookSuspectConf + L")");
    } else if (hasSuspect && !suspectWho.empty()) {
      summary = en
        ? (L"Crash is reported in a Windows system DLL, and the top stack candidate is " + suspectWho +
            L". This can still be a victim location rather than the root cause. (Confidence: Low)")
        : (L"크래시가 Windows 시스템 DLL에서 보고되었고, 스택 후보 1순위는 " + suspectWho +
            L" 입니다. 이 경우에도 실제 원인은 다른 DLL/모드일 수 있습니다. (신뢰도: 낮음)");
    } else if (r.exc_code == 0xE06D7363u) {
      summary = en
        ? L"Reported in a Windows system DLL with 0xE06D7363 (C++ exception). Could be normal throw/catch; confirm this was an actual CTD. (Confidence: Low)"
        : L"0xE06D7363(C++ 예외)로 Windows 시스템 DLL에서 보고되었습니다. 정상 동작 중 throw/catch일 수도 있어 실제 CTD 여부 확인이 필요합니다. (신뢰도: 낮음)";
    } else {
      summary = en
        ? L"Crash is reported in a Windows system DLL. The real culprit may be another mod/DLL. (Confidence: Low)"
        : L"크래시가 Windows 시스템 DLL에서 보고되었습니다. 실제 원인은 다른 모드/DLL일 수 있습니다. (신뢰도: 낮음)";
    }
  } else if (hasModule && isGameExe) {
    const bool hasObjectRefs = !r.crash_logger_object_refs.empty();
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
      summary = en
        ? (L"Crash is reported in the game executable. " + frameSupport + L" points to DLL candidate " + candidateName +
            L" (" + families + L"). This is stronger than an isolated object ref." +
            (hasFirstChanceSupport ? L" Repeated suspicious first-chance context also matched this candidate." : L"") +
            (hasHistorySupport ? L" Repeated crash bucket history also matched this candidate." : L"") +
            L" (Confidence: " + topCandidateConf + L")")
        : (L"크래시 위치가 게임 본체(EXE)이며, " + frameSupport + L" 가 DLL 후보 " + candidateName +
            L" 를 가리킵니다. (" + families + L", 단독 object ref 보다 강한 신호" +
            (hasFirstChanceSupport ? L", 반복 suspicious first-chance 문맥도 이 후보와 맞음" : L"") +
            (hasHistorySupport ? L", 반복 크래시 버킷 이력도 이 후보와 맞음" : L"") +
            L", 신뢰도: " + topCandidateConf + L")");
    } else if (topCandidate && topCandidate->status_id == "related") {
      const auto candidateName = DescribeCandidate(*topCandidate);
      const auto families = JoinCandidateFamilies(*topCandidate, en);
      summary = en
        ? (L"Crash is reported in the game executable. Actionable candidate: " + candidateName +
            L" (" + families + L"). (Confidence: " + topCandidateConf + L")")
        : (L"크래시 위치가 게임 본체(EXE)이며, 실행 우선 후보는 " + candidateName +
            L" 입니다. (" + families + L", 신뢰도: " + topCandidateConf + L")");
    } else if (topCandidate && topCandidate->status_id == "reference_clue" && topCandidateHasFrame) {
      const auto candidateName = DescribeCandidate(*topCandidate);
      const auto frameSupport = DescribeFrameSupport(r, *topCandidate, en);
      summary = en
        ? (L"Crash is reported in the game executable, and " + frameSupport + L" points to DLL candidate " + candidateName +
            L". No second independent signal agrees yet. (Confidence: " + topCandidateConf + L")")
        : (L"크래시 위치가 게임 본체(EXE)이며, " + frameSupport + L" 가 DLL 후보 " + candidateName +
            L" 를 가리킵니다. 아직 두 번째 독립 신호 합의는 없습니다. (신뢰도: " + topCandidateConf + L")");
    } else if (topCandidate && topCandidate->status_id == "reference_clue" && topCandidateHasObjectRef) {
      const auto candidateName = DescribeCandidate(*topCandidate);
      summary = en
        ? (L"Crash is reported in the game executable, and CrashLogger shows the game was processing " + candidateName +
            L". No second independent signal agrees yet. (Confidence: " + topCandidateConf + L")")
        : (L"크래시 위치가 게임 본체(EXE)이며, CrashLogger 기준으로 " + candidateName +
            L" 처리 중이었습니다. 아직 두 번째 독립 신호 합의는 없습니다. (신뢰도: " + topCandidateConf + L")");
    // Priority 1: stackwalk-based non-hook suspect (high reliability)
    } else if (hasNonHookSuspect && suspectsAreStackwalk && !nonHookSuspectWho.empty()) {
      if (hasObjectRefs) {
        const auto& topRef = r.crash_logger_object_refs[0];
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
      const auto& topRef = r.crash_logger_object_refs[0];
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
        ? L"Crash is reported in the game executable. Version mismatch/hook conflict is possible. (Confidence: Medium)"
        : L"크래시 위치가 게임 본체(EXE)로 보고되었습니다. 버전 불일치/후킹 충돌 가능성이 있습니다. (신뢰도: 중간)";
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

      if (hasSuspect && !suspectWho.empty()) {
        if ((topSuspectIsHookFramework || topSuspectIsSystem) && hasNonHookSuspect && !nonHookSuspectWho.empty()) {
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

  return summary;
}

}  // namespace skydiag::dump_tool::internal
