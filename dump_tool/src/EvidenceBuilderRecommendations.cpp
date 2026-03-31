#include "EvidenceBuilderPrivate.h"

#include <algorithm>
#include <cwchar>

#include "MinidumpUtil.h"
#include "SkyrimDiagShared.h"

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
    return en ? L"near resource provider" : L"인접 리소스 provider";
  }
  if (familyId == "history_repeat") {
    return en ? L"history repeat" : L"버킷 반복";
  }
  if (familyId == "first_chance_context") {
    return en ? L"repeated first-chance context" : L"반복 first-chance 문맥";
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

bool HasDenseFirstChanceLoadingWindow(const FirstChanceSummary& summary)
{
  return summary.recent_count >= 3u &&
         summary.loading_window_count >= 2u &&
         summary.loading_window_count * 2u >= summary.recent_count;
}

std::wstring DescribeCaptureProfileStrength(const AnalysisResult& r, bool en)
{
  std::vector<std::wstring> parts;
  if (r.incident_capture_profile_process_thread_data) {
    parts.push_back(en ? L"process/thread data" : L"process/thread data");
  }
  if (r.incident_capture_profile_full_memory_info) {
    parts.push_back(en ? L"full memory info" : L"full memory info");
  }
  if (r.incident_capture_profile_module_headers) {
    parts.push_back(en ? L"module headers" : L"module headers");
  }
  if (r.incident_capture_profile_indirect_memory) {
    parts.push_back(en ? L"indirect memory" : L"indirect memory");
  }
  if (r.incident_capture_profile_ignore_inaccessible_memory) {
    parts.push_back(en ? L"inaccessible-memory tolerance" : L"inaccessible-memory tolerance");
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
    const ActionableCandidate* secondCandidate)
{
  if (!topCandidate) {
    return;
  }

  const auto candidateName = DescribeCandidate(*topCandidate);
  const bool hasFrameFamily = CandidateHasFamily(*topCandidate, "crash_logger_frame");
  const bool hasFirstChanceFamily = CandidateHasFamily(*topCandidate, "first_chance_context");
  const bool hasHistoryFamily = CandidateHasFamily(*topCandidate, "history_repeat");
  const bool hasResourceFamily = CandidateHasFamily(*topCandidate, "resource_provider");
  const bool hasStandaloneCallstackFamily =
    CandidateHasFamily(*topCandidate, "actionable_stack") &&
    !hasFrameFamily &&
    !CandidateHasFamily(*topCandidate, "crash_logger_object_ref") &&
    !hasResourceFamily &&
    topCandidate->score >= 5u;
  const bool hasScorableFirstChance = hasFirstChanceFamily && HasScorableFirstChanceContext(r.first_chance_summary);
  const bool hasScorableHistory = hasHistoryFamily && (r.history_correlation.count > 1 || !r.bucket_candidate_repeats.empty());
  const auto frameSupport = hasFrameFamily ? DescribeCrashLoggerFrameSupport(r, *topCandidate, en) : std::wstring{};
  if (topCandidate->status_id == "cross_validated") {
    r.recommendations.push_back(hasFrameFamily
      ? (en
          ? (L"[Actionable candidate] " + frameSupport + L" and another signal agree on " + candidateName +
              L". Use DLL guidance first: update/reinstall or isolate it before broader EXE/system triage.")
          : (L"[행동 우선 후보] " + frameSupport + L" 와 다른 신호가 " + candidateName +
              L" 쪽으로 합의합니다. 광범위한 EXE/system 점검보다 먼저 DLL guidance로 업데이트/재설치/격리를 진행하세요."))
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
          : (L"[First-chance] 반복 first-chance 모듈 경로를 먼저 확인하세요: " + firstChanceDetail));
      }
    }
    r.recommendations.push_back(en
      ? (L"[Actionable candidate] If the crash repeats, disable " + candidateName +
          L" (or the providing mod/DLL) and retest.")
      : (L"[행동 우선 후보] 동일 문제가 반복되면 " + candidateName +
          L" 또는 해당 모드/DLL을 비활성화하고 다시 테스트하세요."));
  } else if (topCandidate->status_id == "related") {
    r.recommendations.push_back(hasStandaloneCallstackFamily
      ? (en
          ? (L"[Actionable candidate] Tullius callstack first points to DLL candidate " + candidateName +
              L" (actionable stack). Check it before broad EXE/system triage.")
          : (L"[행동 우선 후보] Tullius callstack first 가 DLL 후보 " + candidateName +
              L" (actionable stack)를 가리킵니다. 광범위한 EXE/system 점검 전에 먼저 확인하세요."))
      : hasFrameFamily
      ? (hasScorableFirstChance
          ? (en
              ? (L"[Actionable candidate] " + frameSupport + L" points to DLL candidate " + candidateName +
                  L" (" + JoinFamilies(*topCandidate, en) + L"). Use DLL guidance first and check the repeated first-chance path before broad EXE/system triage.")
              : (L"[행동 우선 후보] " + frameSupport + L" 가 DLL 후보 " + candidateName +
                  L" (" + JoinFamilies(*topCandidate, en) + L")를 가리킵니다. 광범위한 EXE/system 점검보다 먼저 DLL guidance 와 반복 first-chance 경로를 함께 확인하세요."))
          : hasScorableHistory
            ? (en
                ? (L"[Actionable candidate] " + frameSupport + L" points to DLL candidate " + candidateName +
                    L" (" + JoinFamilies(*topCandidate, en) + L"). Use DLL guidance first and compare repeated same-bucket crashes before broad EXE/system triage.")
                : (L"[행동 우선 후보] " + frameSupport + L" 가 DLL 후보 " + candidateName +
                    L" (" + JoinFamilies(*topCandidate, en) + L")를 가리킵니다. 광범위한 EXE/system 점검보다 먼저 DLL guidance 와 반복 버킷 이력을 함께 확인하세요."))
          : hasResourceFamily
            ? (en
                ? (L"[Actionable candidate] " + frameSupport + L" points to DLL candidate " + candidateName +
                    L" (" + JoinFamilies(*topCandidate, en) + L"). Use DLL guidance first and compare nearby resource providers before broad EXE/system triage.")
                : (L"[행동 우선 후보] " + frameSupport + L" 가 DLL 후보 " + candidateName +
                    L" (" + JoinFamilies(*topCandidate, en) + L")를 가리킵니다. 광범위한 EXE/system 점검보다 먼저 DLL guidance 와 인접 리소스 provider를 함께 확인하세요."))
          : (en
              ? (L"[Actionable candidate] " + frameSupport + L" points to DLL candidate " + candidateName +
                  L" (" + JoinFamilies(*topCandidate, en) + L"). Use DLL guidance first before broad EXE/system triage.")
              : (L"[행동 우선 후보] " + frameSupport + L" 가 DLL 후보 " + candidateName +
                  L" (" + JoinFamilies(*topCandidate, en) + L")를 가리킵니다. 광범위한 EXE/system 점검보다 먼저 DLL guidance를 따르세요.")))
      : (en
          ? (L"[Actionable candidate] Partial multi-signal support points to " + candidateName +
              L" (" + JoinFamilies(*topCandidate, en) + L"). Check it before falling back to generic SKSE/plugin triage.")
          : (L"[행동 우선 후보] 부분적인 다중 신호가 " + candidateName +
              L" (" + JoinFamilies(*topCandidate, en) + L")를 가리킵니다. 일반적인 SKSE/DLL 점검보다 먼저 확인하세요.")));
    if (hasFirstChanceFamily) {
      const auto firstChanceDetail = DescribeFirstChanceContext(r.first_chance_summary, en);
      if (!firstChanceDetail.empty()) {
        r.recommendations.push_back(en
          ? (L"[First-chance] Repeated first-chance exceptions matched this candidate. Check that module path before broad EXE/system crash triage: " + firstChanceDetail)
          : (L"[First-chance] 반복 first-chance 예외가 이 후보와 맞습니다. 광범위한 EXE/system 크래시 점검 전에 해당 모듈 경로부터 확인하세요: " + firstChanceDetail));
      }
    }
  } else if (topCandidate->status_id == "reference_clue") {
    if (hasFrameFamily) {
      r.recommendations.push_back(en
        ? (L"[Crash Logger frame] " + frameSupport + L" points to DLL candidate " + candidateName +
            L", but no second independent signal agrees yet. Use DLL guidance first and confirm with another capture if needed.")
        : (L"[Crash Logger 프레임] " + frameSupport + L" 가 DLL 후보 " + candidateName +
            L" 를 가리키지만 아직 두 번째 독립 신호 합의는 없습니다. 우선 DLL guidance를 따르고 필요하면 추가 캡처로 확인하세요."));
      r.recommendations.push_back(en
        ? L"[Crash Logger frame] If the frame clue stays isolated, capture another incident or rerun with a richer crash recapture profile before escalating to FullMemory (DumpMode=2)."
        : L"[Crash Logger 프레임] 이 frame 단서가 계속 단독으로 남으면 다른 사고를 한 번 더 캡처하거나, 바로 FullMemory(DumpMode=2)로 가지 말고 richer crash recapture profile로 먼저 재수집하세요.");
    } else {
      r.recommendations.push_back(en
        ? (L"[Object ref] The game was processing " + candidateName +
            L" at crash time, but no second independent signal agrees yet. Treat it as a clue first.")
        : (L"[오브젝트 참조] 사고 당시 게임이 " + candidateName +
            L" 을(를) 처리 중이었지만 아직 두 번째 독립 신호 합의는 없습니다. 우선 단서로 보세요."));
      r.recommendations.push_back(en
        ? L"[Object ref] If the clue stays isolated, capture another incident or rerun with a richer crash recapture profile before escalating to FullMemory (DumpMode=2)."
        : L"[오브젝트 참조] 이 단서가 계속 단독으로 남으면 다른 사고를 한 번 더 캡처하거나, 바로 FullMemory(DumpMode=2)로 가지 말고 richer crash recapture profile로 먼저 재수집하세요.");
    }
  } else if (topCandidate->status_id == "conflicting" && secondCandidate) {
    const auto secondName = DescribeCandidate(*secondCandidate);
    r.recommendations.push_back(en
      ? (L"[Conflict] Signals split between " + candidateName + L" (" + JoinFamilies(*topCandidate, en) + L") and " +
          secondName + L" (" + JoinFamilies(*secondCandidate, en) + L"). Check whether Crash Logger frame first DLL guidance and object ref/stack evidence disagree before retesting.")
      : (L"[충돌] 신호가 " + candidateName + L" (" + JoinFamilies(*topCandidate, en) + L")와 " +
          secondName + L" (" + JoinFamilies(*secondCandidate, en) + L")로 갈립니다. Crash Logger frame first DLL guidance 와 object ref/stack 근거가 어디서 갈리는지 먼저 확인한 뒤 재현을 확인하세요."));
    r.recommendations.push_back(en
      ? L"[Conflict] If the split persists, rerun with a richer crash recapture profile first; use FullMemory (DumpMode=2) only if the tie remains."
      : L"[충돌] 이 분리가 계속되면 richer crash recapture profile로 먼저 다시 캡처하고, 그래도 갈리면 그때만 FullMemory(DumpMode=2)를 사용하세요.");
  }
}

}  // namespace

void BuildRecommendations(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx)
{
  const bool en = ctx.en;
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
           !minidump::IsSystemishModule(s.module_filename) &&
           !minidump::IsLikelyWindowsSystemModulePath(s.module_path) &&
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
    hasStackCandidate &&
    (minidump::IsSystemishModule(r.suspects[0].module_filename) || minidump::IsLikelyWindowsSystemModulePath(r.suspects[0].module_path));
  const bool preferStackCandidateOverFault =
    ctx.isHookFramework &&
    hasNonHookStackCandidate;
  const bool allowFaultModuleTopSuspectRecommendations =
    !ctx.isHookFramework || hasNonHookStackCandidate;
  const ActionableCandidate* topCandidate = !r.actionable_candidates.empty() ? &r.actionable_candidates[0] : nullptr;
  const ActionableCandidate* secondCandidate = (r.actionable_candidates.size() > 1u) ? &r.actionable_candidates[1] : nullptr;
  const bool hasActionableCandidates = (topCandidate != nullptr);
  const bool topCandidateMatchesFaultModule =
    topCandidate &&
    CandidateMatchesModule(*topCandidate, r.fault_module_filename);
  const bool topCandidateCrossValidatedFaultModule =
    topCandidateMatchesFaultModule &&
    topCandidate->status_id == "cross_validated";

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
          r.recommendations.push_back(rec);
        }
      }
    }
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
      ? L"[BEES] Install BEES or update the game runtime to 1.6.1130+."
      : L"[BEES] BEES를 설치하거나 게임 런타임을 1.6.1130 이상으로 업데이트하세요.");
  }

  if (!r.missing_masters.empty()) {
    r.recommendations.push_back(en
      ? L"[Masters] Install missing masters or disable dependent plugins."
      : L"[마스터] 누락된 마스터를 설치하거나 의존 플러그인을 비활성화하세요.");
  }

  // Recommendations (checklist)
  if (isSnapshotLike) {
    r.recommendations.push_back(en
      ? L"[Snapshot] No exception/crash info is present. This dump alone is not enough to blame a mod."
      : L"[정상/스냅샷] 예외(크래시) 정보가 없습니다. 이 덤프만으로 '어떤 모드가 크래시 원인'인지 판단하기 어렵습니다.");
    r.recommendations.push_back(en
      ? L"[Snapshot] Capture during a real issue for diagnosis: (1) real CTD dump, (2) manual capture during freeze/infinite loading (Ctrl+Shift+F12) or an auto hang dump."
      : L"[정상/스냅샷] 문제 상황에서 캡처해야 진단이 가능합니다: (1) 실제 크래시 덤프, (2) 프리징/무한로딩 중 수동 캡처(Ctrl+Shift+F12) 또는 자동 감지 덤프");
  }

  if (r.exc_code != 0) {
    if (r.exc_code == 0xC0000005u) {
      r.recommendations.push_back(en
        ? L"[Basics] ExceptionCode=0xC0000005 (Access Violation). Often caused by DLL hooks / invalid memory access."
        : L"[기본] ExceptionCode=0xC0000005(접근 위반)입니다. 보통 DLL 후킹/메모리 접근 문제로 발생합니다.");
    } else {
      wchar_t buf[128]{};
      swprintf_s(buf, en ? L"[Basics] ExceptionCode=0x%08X." : L"[기본] ExceptionCode=0x%08X 입니다.", r.exc_code);
      r.recommendations.push_back(buf);
    }

    if (r.exc_code == 0xE06D7363u) {
      r.recommendations.push_back(en
        ? L"[Interpretation] 0xE06D7363 is a common C++ exception (throw) code. It can occur during normal throw/catch."
        : L"[해석] 0xE06D7363은 흔한 C++ 예외(throw) 코드입니다. 정상 동작 중에도 throw/catch로 발생할 수 있습니다.");
      r.recommendations.push_back(en
        ? L"[Interpretation] If the game did not actually crash, this dump may be a handled exception false positive."
        : L"[해석] 게임이 실제로 튕기지 않았다면, 이 덤프는 '실제 CTD'가 아니라 'handled exception 오탐'일 수 있습니다.");
      r.recommendations.push_back(en
        ? L"[Config] Using SkyrimDiag.ini CrashHookMode=1 (fatal only) greatly reduces these false positives."
        : L"[설정] SkyrimDiag.ini의 CrashHookMode=1(치명 예외만)로 두면 이런 오탐을 크게 줄일 수 있습니다.");
    }
  }

  if (ctx.isHookFramework) {
    r.recommendations.push_back(en
      ? L"[Hook framework] This mod extensively hooks the game engine. It may be a victim of memory corruption caused by another mod, not the root cause itself. Check other suspect candidates first."
      : L"[훅 프레임워크] 이 모드는 게임 엔진을 광범위하게 훅합니다. 다른 모드의 메모리 오염으로 인한 피해자일 수 있으며, 이 모드 자체가 원인이 아닐 수 있습니다. 다른 후보 모드를 먼저 점검하세요.");
  }

  AddActionableCandidateRecommendations(r, en, topCandidate, secondCandidate);

  const bool allowTopSuspectActionRecommendations = !isSnapshotLike;
  if (!hasActionableCandidates && allowTopSuspectActionRecommendations && !r.inferred_mod_name.empty() && !preferStackCandidateOverFault && allowFaultModuleTopSuspectRecommendations) {
    r.recommendations.push_back(en
      ? (L"[Top suspect] Reproduce after updating/reinstalling '" + r.inferred_mod_name + L"'.")
      : (L"[유력 후보] '" + r.inferred_mod_name + L"' 모드를 업데이트/재설치 후 재현 여부 확인"));
    r.recommendations.push_back(en
      ? (L"[Top suspect] If it repeats, disable the mod (or its SKSE plugin DLL) and retest: '" + r.inferred_mod_name + L"'.")
      : (L"[유력 후보] 동일 크래시가 반복되면 '" + r.inferred_mod_name + L"' 모드(또는 해당 모드의 SKSE 플러그인 DLL)를 비활성화 후 재현 여부 확인"));
  }

  if (!hasActionableCandidates && allowTopSuspectActionRecommendations && (r.inferred_mod_name.empty() || preferStackCandidateOverFault) && hasNonHookStackCandidate) {
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

  if (!hasActionableCandidates && !r.crash_logger_object_refs.empty()) {
    const auto& topRef = r.crash_logger_object_refs[0];
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

  if (hitch.count > 0) {
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
          : L"[DLL guidance] 이 DLL을 바로 근본 원인으로 단정하기 전에, 포함된 모드의 선행 모드/요구 버전(SKSE/Address Library/엔진 버전)부터 확인하세요."));
    if (topCandidateCrossValidatedFaultModule) {
      r.recommendations.push_back(en
        ? L"[Top suspect] Attach this report (*_SkyrimDiagReport.txt) and dump (*.dmp) when reporting to the mod author."
        : L"[유력 후보] 이 리포트 파일(*_SkyrimDiagReport.txt)과 덤프(*.dmp)를 모드 제작자에게 첨부");
    }
  } else if (allowTopSuspectActionRecommendations && hasModule && isGameExe) {
    if (!r.crash_logger_object_refs.empty()) {
      // ESP/ESM reference is the primary clue for EXE crashes — already emitted above as [ESP/ESM]
      if (hasStackCandidate && !r.suspects_from_stackwalk) {
        // Stack scan suspect exists but is weak — note it as supplementary
        r.recommendations.push_back(en
          ? (L"[Stack scan] " + r.suspects[0].module_filename + L" was also detected nearby via stack scan, but this is a weak signal. Prioritize the ESP/ESM clue above.")
          : (L"[스택 스캔] 스택 스캔에서 " + r.suspects[0].module_filename + L"도 감지되었으나 신뢰도가 낮습니다. 위의 ESP/ESM 단서를 우선 점검하세요."));
      }
    }
    r.recommendations.push_back(en
      ? L"[Check] Crash location is the game executable. Version mismatch (Address Library/SKSE) or hook conflicts are likely."
      : L"[점검] 크래시 위치가 게임 본체(EXE)로 나옵니다. Address Library/ SKSE 버전 불일치 또는 후킹 충돌 가능성이 큽니다.");
    r.recommendations.push_back(en
      ? L"[Check] Disable recently added/updated SKSE plugin DLLs one by one and retest."
      : L"[점검] 최근 추가/업데이트한 SKSE 플러그인(DLL)부터 하나씩 제외하며 재현 여부 확인");
  } else if (allowTopSuspectActionRecommendations && hasModule && isSystem) {
    r.recommendations.push_back(en
      ? L"[Check] When a Windows system DLL is shown, the real culprit is often another mod/DLL."
      : L"[점검] Windows 시스템 DLL로 표시될 때는 실제 원인이 다른 모드/DLL인 경우가 많습니다.");
    r.recommendations.push_back(en
      ? L"[Check] Disable recently added/updated SKSE plugin DLLs one by one and retest."
      : L"[점검] 최근 추가/업데이트한 SKSE 플러그인(DLL)부터 하나씩 제외하며 재현 여부 확인");
    r.recommendations.push_back(en
      ? L"[Check] Verify SKSE version, game runtime (AE/SE/VR), and Address Library all match."
      : L"[점검] SKSE 버전/게임 버전(AE/SE/VR)/Address Library 버전이 서로 맞는지 확인");
  } else {
    if (!isSnapshotLike) {
    r.recommendations.push_back(en
      ? L"[Check] Fault module could not be determined. Capture again with a richer crash recapture profile (full memory info / module headers / indirect memory) before escalating to FullMemory (DumpMode=2)."
      : L"[점검] 덤프에서 fault module을 특정하지 못했습니다. 바로 FullMemory(DumpMode=2)로 가지 말고 richer crash recapture profile(full memory info / module headers / indirect memory)로 먼저 다시 캡처하세요.");
    }
  }

  if ((r.state_flags & skydiag::kState_Loading) != 0u) {
    r.recommendations.push_back(en
      ? L"[Loading] Crashes right after load screens often involve animation/mesh/texture/skeleton/script initialization."
      : L"[로딩 중] 로딩 화면/세이브 로드 직후 크래시는 애니메이션/메쉬/텍스처/스켈레톤/스크립트 초기화 쪽이 흔합니다.");
    r.recommendations.push_back(en
      ? L"[Loading] Check mods affecting that stage first (animations/skeleton/body/physics/precaching)."
      : L"[로딩 중] 해당 시점에 개입하는 모드(애니메이션/스켈레톤/바디/물리/프리캐시)를 우선 점검");
  }

  if (r.freeze_analysis.has_analysis) {
    if (r.freeze_analysis.state_id == "deadlock_likely") {
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
        } else {
          r.recommendations.push_back(en
            ? L"[Hang] No WCT cycle: possible infinite loop / busy wait."
            : L"[프리징] WCT cycle이 없으면 무한루프/바쁜 대기(busy wait) 가능성도 있습니다.");
        }
      }
      r.recommendations.push_back(en
        ? L"[Hang] If it repeats, use Events tab (just before the freeze) to narrow related mods."
        : L"[프리징] 프리징이 반복되면 문제 상황 직전에 실행된 이벤트(이벤트 탭)를 기준으로 관련 모드를 점검");
    } else if (isManualCapture && isSnapshotLike) {
      if (wct && wct->has_capture && wct->thresholdSec > 0u && wct->secondsSinceHeartbeat < static_cast<double>(wct->thresholdSec)) {
        wchar_t buf[256]{};
        swprintf_s(
          buf,
          en
            ? L"[Manual] At capture time, heartbeatAge=%.1fs < threshold=%us, so it is not considered a hang."
            : L"[수동] 수동 캡처 당시 heartbeatAge=%.1fs < threshold=%us 이므로 '프리징/무한로딩'으로 판단되지 않습니다.",
          wct->secondsSinceHeartbeat,
          wct->thresholdSec);
        r.recommendations.push_back(buf);
      }
      r.recommendations.push_back(en
        ? L"[Manual] Manual captures include WCT. For real freezes/infinite loading, check the WCT tab from a capture taken during the issue."
        : L"[수동] 수동 캡처에는 WCT가 포함됩니다. 실제 프리징/무한로딩 중 캡처한 덤프에서 WCT 탭을 참고하세요.");
    }
  }

  if (!r.troubleshooting_steps.empty()) {
    r.recommendations.push_back(en
      ? (L"[Troubleshooting] See the troubleshooting checklist (" + std::to_wstring(r.troubleshooting_steps.size()) + L" steps) for this crash type.")
      : (L"[트러블슈팅] 이 크래시 유형에 대한 단계별 체크리스트(" + std::to_wstring(r.troubleshooting_steps.size()) + L"단계)를 확인하세요."));
  }
  }

}  // namespace skydiag::dump_tool::internal
