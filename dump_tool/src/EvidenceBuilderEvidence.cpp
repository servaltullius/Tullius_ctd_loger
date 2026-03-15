#include "EvidenceBuilderEvidencePipeline.h"
#include "SkyrimDiagShared.h"

namespace skydiag::dump_tool::internal {

void BuildEvidenceItems(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx)
{
  const bool en = ctx.en;

  if (r.signature_match.has_value()) {
    const auto& sig = *r.signature_match;
    EvidenceItem e{};
    e.confidence_level = sig.confidence_level;
    e.confidence = sig.confidence.empty() ? ConfidenceText(lang, sig.confidence_level) : sig.confidence;
    e.title = ctx.en
      ? (L"Known crash pattern: " + ToWideAscii(sig.id))
      : (L"알려진 크래시 패턴: " + ToWideAscii(sig.id));
    e.details = sig.cause;
    r.evidence.push_back(std::move(e));
  }

  if (r.graphics_diag.has_value()) {
    const auto& gd = *r.graphics_diag;
    EvidenceItem e{};
    e.confidence_level = gd.confidence_level;
    e.confidence = gd.confidence.empty() ? ConfidenceText(lang, gd.confidence_level) : gd.confidence;
    e.title = en
      ? (L"Graphics injection crash: " + ToWideAscii(gd.rule_id))
      : (L"그래픽 인젝션 크래시: " + ToWideAscii(gd.rule_id));
    e.details = gd.cause;
    r.evidence.push_back(std::move(e));
  }

  if (!r.plugin_diagnostics.empty()) {
    for (const auto& pd : r.plugin_diagnostics) {
      EvidenceItem e{};
      e.confidence_level = pd.confidence_level;
      e.confidence = pd.confidence.empty() ? ConfidenceText(lang, pd.confidence_level) : pd.confidence;
      e.title = en
        ? (L"Plugin diagnostics: " + ToWideAscii(pd.rule_id))
        : (L"플러그인 진단: " + ToWideAscii(pd.rule_id));
      e.details = pd.cause;
      r.evidence.push_back(std::move(e));
    }
  }

  if (!r.missing_masters.empty()) {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kHigh;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"Missing plugin masters detected"
      : L"누락된 마스터 플러그인 감지";
    e.details = JoinList(r.missing_masters, 4, L", ");
    r.evidence.push_back(std::move(e));
  }

  if (r.needs_bees && ctx.isCrashLike) {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kHigh;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"BEES requirement detected"
      : L"BEES 필요 조건 감지";
    e.details = en
      ? L"Header 1.71 plugin(s) found on pre-1.6.1130 runtime without bees.dll."
      : L"1.71 헤더 플러그인이 있으나 1.6.1130 미만 런타임에서 bees.dll이 로드되지 않았습니다.";
    r.evidence.push_back(std::move(e));
  }

  if (r.incident_capture_profile_present) {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kMedium;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"Capture profile metadata"
      : L"캡처 프로필 메타데이터";
    e.details = DescribeCaptureProfileEvidence(r, en);
    r.evidence.push_back(std::move(e));
  }

  if (r.incident_recapture_evaluation_present && r.incident_recapture_triggered) {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kMedium;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en ? L"Capture recapture context" : L"재수집 캡처 문맥";
    e.details = DescribeRecaptureEvaluationEvidence(r, en);
    r.evidence.push_back(std::move(e));
  }

  if (r.symbol_runtime_degraded) {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kMedium;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"Symbol/runtime environment limited stackwalk quality"
      : L"심볼/런타임 환경 때문에 스택워크 품질이 제한됨";
    e.details = DescribeSymbolRuntimeEvidence(r, en);
    r.evidence.push_back(std::move(e));
  }

  if (ctx.hasException) {
    if (auto info = TryExplainExceptionInfo(r, en)) {
      EvidenceItem e{};
      e.confidence_level = i18n::ConfidenceLevel::kHigh;
      e.confidence = ConfidenceText(lang, e.confidence_level);
      e.title = en ? L"Exception parameter analysis" : L"예외 파라미터 분석";
      e.details = *info;
      r.evidence.push_back(std::move(e));
    }
  }

  if (ctx.isSnapshotLike) {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kHigh;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"This dump looks like a state snapshot (not a crash/hang dump)"
      : L"이 덤프는 크래시 덤프가 아니라 '상태 스냅샷'으로 보임";
    e.details = en
      ? (ctx.isManualCapture
          ? L"Likely a manual snapshot. This alone does not prove there is a problem. (For state inspection)"
          : L"Captured without crash/hang signals. Treat it as a snapshot, not a root-cause dump.")
      : (ctx.isManualCapture
          ? L"수동 캡처로 추정됩니다. 이 결과만으로 '문제가 있다'고 단정할 수 없습니다. (상태 확인용)"
          : L"크래시/행 신호 없이 캡처된 덤프입니다. 원인 확정용이 아니라 '상태 확인용'입니다.");
    r.evidence.push_back(std::move(e));
  }

  if (r.freeze_analysis.has_analysis) {
    EvidenceItem e{};
    e.confidence_level = r.freeze_analysis.confidence_level;
    e.confidence = r.freeze_analysis.confidence.empty()
      ? ConfidenceText(lang, r.freeze_analysis.confidence_level)
      : r.freeze_analysis.confidence;
    e.title = en ? L"Freeze analysis" : L"프리징 분석";
    std::wstring details = ToWideAscii(r.freeze_analysis.state_id);
    details += L" | " + DescribeFreezeSupportQuality(r.freeze_analysis.support_quality, en);
    if (!r.freeze_analysis.primary_reasons.empty()) {
      details += L" | " + JoinList(r.freeze_analysis.primary_reasons, 3, L" | ");
    }
    if (r.freeze_analysis.blackbox_context.has_context) {
      details += en
        ? (L" | blackbox module churn=" + std::to_wstring(r.freeze_analysis.blackbox_context.module_churn_score) +
            L", thread churn=" + std::to_wstring(r.freeze_analysis.blackbox_context.thread_churn_score))
        : (L" | blackbox module churn=" + std::to_wstring(r.freeze_analysis.blackbox_context.module_churn_score) +
            L", thread churn=" + std::to_wstring(r.freeze_analysis.blackbox_context.thread_churn_score));
      if (!r.freeze_analysis.blackbox_context.recent_non_system_modules.empty()) {
        details += en
          ? (L" | recent non-system modules: " + JoinList(r.freeze_analysis.blackbox_context.recent_non_system_modules, 3, L", "))
          : (L" | 최근 비시스템 모듈: " + JoinList(r.freeze_analysis.blackbox_context.recent_non_system_modules, 3, L", "));
      }
    }
    if (r.freeze_analysis.first_chance_context.has_context) {
      details += en
        ? (L" | repeated suspicious first-chance=" + std::to_wstring(r.freeze_analysis.first_chance_context.repeated_signature_count) +
            L", loading-window count=" + std::to_wstring(r.freeze_analysis.first_chance_context.loading_window_count))
        : (L" | 반복 suspicious first-chance=" + std::to_wstring(r.freeze_analysis.first_chance_context.repeated_signature_count) +
            L", loading-window count=" + std::to_wstring(r.freeze_analysis.first_chance_context.loading_window_count));
      if (!r.freeze_analysis.first_chance_context.recent_non_system_modules.empty()) {
        details += en
          ? (L" | first-chance modules: " + JoinList(r.freeze_analysis.first_chance_context.recent_non_system_modules, 3, L", "))
          : (L" | first-chance 모듈: " + JoinList(r.freeze_analysis.first_chance_context.recent_non_system_modules, 3, L", "));
      }
    }
    e.details = std::move(details);
    r.evidence.push_back(std::move(e));
  }

  if (ctx.isCrashLike &&
      !ctx.isHangLike &&
      !ctx.isSnapshotLike &&
      (ctx.isGameExe || ctx.isSystem) &&
      !r.actionable_candidates.empty() &&
      CandidateHasFamily(r.actionable_candidates[0], "first_chance_context") &&
      HasScorableFirstChanceContext(r.first_chance_summary)) {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kMedium;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"Actionable candidate is supported by repeated suspicious first-chance context"
      : L"행동 우선 후보가 반복 suspicious first-chance 문맥으로 보강됨";
    e.details = DescribeFirstChanceSupport(r.first_chance_summary, en);
    r.evidence.push_back(std::move(e));
  }

  BuildCrashLoggerEvidence(r, lang, ctx);
  BuildSuspectEvidence(r, lang, ctx);
  BuildResourceEvidence(r, lang, ctx);
  BuildHitchAndFreezeEvidence(r, lang, ctx);
  BuildModuleClassificationEvidence(r, lang, ctx);

  if (!r.inferred_mod_name.empty()) {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kMedium;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"Inferred mod name from MO2 mod path"
      : L"MO2 모드 폴더 경로에서 모드명 추정";
    e.details = en
      ? (L"Detected a \\mods\\<modname>\\ path pattern; inferred '" + r.inferred_mod_name + L"'.")
      : (L"모듈 경로에 \\mods\\<모드명>\\ 패턴이 있어 '" + r.inferred_mod_name + L"' 로 추정했습니다.");
    r.evidence.push_back(std::move(e));
  }

  if ((r.state_flags & skydiag::kState_Loading) != 0u) {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kMedium;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"Capture appears to have happened during loading"
      : L"크래시 당시 로딩 상태로 추정";
    e.details = en
      ? L"The Loading flag is set in state_flags. (Likely mesh/texture/script init stage)"
      : L"state_flags에 Loading 플래그가 설정되어 있습니다. (메쉬/텍스처/스크립트 초기화 단계일 수 있음)";
    r.evidence.push_back(std::move(e));
  }

  BuildWctEvidence(r, lang, ctx);
  BuildHistoryEvidence(r, lang, ctx);
}

}  // namespace skydiag::dump_tool::internal
