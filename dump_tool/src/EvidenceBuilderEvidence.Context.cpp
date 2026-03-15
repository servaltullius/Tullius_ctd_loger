#include "EvidenceBuilderEvidencePipeline.h"

#include <algorithm>
#include <cwchar>
#include <vector>

namespace skydiag::dump_tool::internal {

std::wstring DescribeCaptureProfileEvidence(const AnalysisResult& r, bool en)
{
  if (!r.incident_capture_profile_present) {
    return {};
  }

  const std::wstring captureKind = ToWideAscii(r.incident_capture_kind);
  const std::wstring baseMode = ToWideAscii(r.incident_capture_profile_base_mode);
  if (r.incident_capture_kind == "crash_recapture") {
    return en
      ? (L"Dump was collected by the crash_recapture profile (base_mode=" + baseMode +
          L", full_memory=" + (r.incident_capture_profile_full_memory ? L"1" : L"0") +
          L") after earlier analysis weakness or repeated bucket activity.")
      : (L"이 덤프는 crash_recapture 프로필로 다시 수집되었습니다 (base_mode=" + baseMode +
          L", full_memory=" + (r.incident_capture_profile_full_memory ? L"1" : L"0") +
          L"). 앞선 분석 약점 또는 반복 버킷 때문에 재수집된 것입니다.");
  }

  return en
    ? (L"Dump used capture profile " + captureKind + L" (base_mode=" + baseMode +
        L", full_memory=" + (r.incident_capture_profile_full_memory ? L"1" : L"0") + L").")
    : (L"이 덤프는 " + captureKind + L" 캡처 프로필로 수집되었습니다 (base_mode=" + baseMode +
        L", full_memory=" + (r.incident_capture_profile_full_memory ? L"1" : L"0") + L").");
}

static std::wstring DescribeRecaptureReason(std::string_view reasonId, bool en)
{
  if (reasonId == "unknown_fault_module") {
    return en ? L"unknown fault module" : L"fault module 미확정";
  }
  if (reasonId == "candidate_conflict") {
    return en ? L"candidate conflict" : L"후보 충돌";
  }
  if (reasonId == "reference_clue_only") {
    return en ? L"reference clue only" : L"참조 단서 단독";
  }
  if (reasonId == "stackwalk_degraded") {
    return en ? L"stackwalk degraded" : L"stackwalk 저하";
  }
  if (reasonId == "symbol_runtime_degraded") {
    return en ? L"symbol runtime degraded" : L"심볼 런타임 저하";
  }
  if (reasonId == "first_chance_candidate_weak") {
    return en ? L"first-chance candidate weak" : L"first-chance 후보 약함";
  }
  if (reasonId == "freeze_ambiguous") {
    return en ? L"freeze ambiguous" : L"프리징 해석 애매";
  }
  if (reasonId == "freeze_snapshot_fallback") {
    return en ? L"freeze snapshot fallback" : L"프리징 snapshot fallback";
  }
  if (reasonId == "freeze_candidate_weak") {
    return en ? L"freeze candidate weak" : L"프리징 후보 약함";
  }
  return ToWideAscii(reasonId);
}

std::wstring DescribeRecaptureEvaluationEvidence(const AnalysisResult& r, bool en)
{
  std::vector<std::wstring> parts;
  parts.push_back((en ? L"target_profile=" : L"target_profile=") + ToWideAscii(r.incident_recapture_target_profile));
  parts.push_back((en ? L"escalation_level=" : L"escalation_level=") +
                  std::to_wstring(r.incident_recapture_escalation_level));
  if (!r.incident_recapture_reasons.empty()) {
    std::vector<std::wstring> reasonLabels;
    reasonLabels.reserve(r.incident_recapture_reasons.size());
    for (const auto& reason : r.incident_recapture_reasons) {
      reasonLabels.push_back(DescribeRecaptureReason(reason, en));
    }
    parts.push_back((en ? L"reasons=" : L"reasons=") + JoinList(reasonLabels, reasonLabels.size(), L", "));
  }
  return JoinList(parts, parts.size(), L" | ");
}

std::wstring DescribeSymbolRuntimeEvidence(const AnalysisResult& r, bool en)
{
  std::vector<std::wstring> parts;
  if (r.dbghelp_path.empty()) {
    parts.push_back(en ? L"dbghelp.dll runtime unresolved" : L"dbghelp.dll 런타임 경로 미확인");
  } else if (r.dbghelp_version.empty()) {
    parts.push_back(en ? L"dbghelp.dll version unreadable" : L"dbghelp.dll 버전 미확인");
  } else {
    parts.push_back(en ? (L"dbghelp " + r.dbghelp_version) : (L"dbghelp " + r.dbghelp_version));
  }

  if (!r.msdia_available) {
    parts.push_back(en ? L"msdia140.dll missing" : L"msdia140.dll 누락");
  }
  if (!r.symbol_cache_ready && !r.symbol_cache_path.empty()) {
    parts.push_back(en ? L"symbol cache unavailable" : L"심볼 캐시 준비 실패");
  }
  if (!r.online_symbol_source_allowed) {
    parts.push_back(en ? L"offline symbol policy active" : L"오프라인 심볼 정책 적용");
  }

  return JoinList(parts, parts.size(), L" | ");
}

std::wstring DescribeFreezeSupportQuality(std::string_view supportQuality, bool en)
{
  if (supportQuality == "snapshot_backed") {
    return en ? L"PSS snapshot-backed freeze capture" : L"PSS 스냅샷 기반 프리징 캡처";
  }
  if (supportQuality == "snapshot_fallback") {
    return en ? L"PSS snapshot requested but live-process fallback was used" : L"PSS snapshot 요청 후 live-process fallback 사용";
  }
  if (supportQuality == "live_process") {
    return en ? L"live-process capture quality" : L"live-process 캡처 품질";
  }
  return en ? L"unknown capture quality" : L"캡처 품질 정보 없음";
}

bool CandidateHasFamily(const ActionableCandidate& candidate, std::string_view familyId)
{
  return std::find(candidate.supporting_families.begin(), candidate.supporting_families.end(), familyId) !=
         candidate.supporting_families.end();
}

static bool HasDenseFirstChanceLoadingWindow(const FirstChanceSummary& summary)
{
  return summary.recent_count >= 3u &&
         summary.loading_window_count >= 2u &&
         summary.loading_window_count * 2u >= summary.recent_count;
}

bool HasScorableFirstChanceContext(const FirstChanceSummary& summary)
{
  return summary.has_context &&
         !summary.recent_non_system_modules.empty() &&
         (summary.repeated_signature_count > 0u || HasDenseFirstChanceLoadingWindow(summary));
}

std::wstring DescribeFirstChanceSupport(const FirstChanceSummary& summary, bool en)
{
  std::wstring detail = en
    ? (L"Repeated suspicious first-chance exceptions were observed")
    : (L"반복 suspicious first-chance 예외가 관측되었습니다");
  detail += en
    ? (L" (repeated=" + std::to_wstring(summary.repeated_signature_count) +
        L", loading-window=" + std::to_wstring(summary.loading_window_count) + L")")
    : (L" (반복=" + std::to_wstring(summary.repeated_signature_count) +
        L", 로딩창=" + std::to_wstring(summary.loading_window_count) + L")");
  if (!summary.recent_non_system_modules.empty()) {
    detail += en
      ? (L" | modules: " + JoinList(summary.recent_non_system_modules, 3, L", "))
      : (L" | 모듈: " + JoinList(summary.recent_non_system_modules, 3, L", "));
  }
  return detail;
}

void BuildHistoryEvidence(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx)
{
  if (!r.history_stats.empty()) {
    std::wstring details;
    const std::size_t showN = std::min<std::size_t>(r.history_stats.size(), 3);
    for (std::size_t i = 0; i < showN; ++i) {
      const auto& ms = r.history_stats[i];
      if (ms.module_name.empty()) {
        continue;
      }
      if (!details.empty()) {
        details += L"\n";
      }
      const std::wstring modW = ToWideAscii(ms.module_name);
      if (ctx.en) {
        details += modW + L": " + std::to_wstring(ms.total_appearances) + L"/" +
          std::to_wstring(ms.total_crashes) + L" crashes, top " + std::to_wstring(ms.as_top_suspect) + L"x";
      } else {
        details += modW + L": " + std::to_wstring(ms.total_crashes) + L"회 중 " +
          std::to_wstring(ms.total_appearances) + L"회 등장, 1위 " + std::to_wstring(ms.as_top_suspect) + L"회";
      }
    }
    if (!details.empty()) {
      EvidenceItem e{};
      e.confidence_level = i18n::ConfidenceLevel::kMedium;
      e.confidence = ConfidenceText(lang, e.confidence_level);
      e.title = ctx.en ? L"Crash history pattern" : L"크래시 이력 패턴";
      e.details = details;
      r.evidence.push_back(std::move(e));
    }
  }

  if (r.history_correlation.count > 1) {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kHigh;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = ctx.en ? L"Repeated crash pattern" : L"반복 크래시 패턴";
    wchar_t buf[256]{};
    swprintf_s(buf,
      ctx.en ? L"Same bucket_key matched %zu times (first: %s)"
             : L"동일 패턴이 %zu회 발생 (최초: %s)",
      r.history_correlation.count,
      ToWideAscii(r.history_correlation.first_seen).c_str());
    e.details = buf;
    r.evidence.push_back(std::move(e));
  }
}

}  // namespace skydiag::dump_tool::internal
