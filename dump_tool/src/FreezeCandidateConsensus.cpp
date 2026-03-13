#include "FreezeCandidateConsensus.h"

#include <algorithm>

namespace skydiag::dump_tool {
namespace {

std::wstring PickCandidateName(const ActionableCandidate& candidate)
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

std::string DetermineSupportQuality(const FreezeSignalInput& input)
{
  if (input.wct && input.wct->pss_snapshot_used) {
    return "snapshot_backed";
  }
  if (input.wct && input.wct->pss_snapshot_requested && !input.wct->pss_snapshot_used) {
    return "snapshot_fallback";
  }
  if (input.wct && input.wct->has_capture) {
    return "live_process";
  }
  return "unknown";
}

FreezeRelatedCandidate ToRelatedCandidate(const ActionableCandidate& candidate, i18n::Language language)
{
  FreezeRelatedCandidate row{};
  row.confidence_level = candidate.confidence_level;
  row.confidence = candidate.confidence.empty()
    ? i18n::ConfidenceText(language, candidate.confidence_level)
    : candidate.confidence;
  row.display_name = PickCandidateName(candidate);
  return row;
}

}  // namespace

FreezeAnalysisResult BuildFreezeCandidateConsensus(const FreezeSignalInput& input, i18n::Language language)
{
  FreezeAnalysisResult result{};
  const bool freezeLike = input.is_hang_like || input.is_snapshot_like || input.is_manual_capture;
  if (!freezeLike) {
    return result;
  }

  result.has_analysis = true;
  result.support_quality = DetermineSupportQuality(input);

  if (input.wct && input.wct->cycles > 0) {
    result.state_id = "deadlock_likely";
    result.confidence_level = input.wct->pss_snapshot_used
      ? i18n::ConfidenceLevel::kHigh
      : i18n::ConfidenceLevel::kMedium;
    result.primary_reasons.push_back(
      language == i18n::Language::kEnglish
        ? L"WCT reported cycle threads"
        : L"WCT에서 cycle thread가 감지됨");
    if (input.wct->longest_wait_tid != 0u) {
      result.primary_reasons.push_back(
        language == i18n::Language::kEnglish
          ? L"A blocked thread with the longest observed wait was identified"
          : L"가장 오래 기다린 blocked thread가 식별됨");
    }
  } else if ((input.loading_context || (input.wct && input.wct->isLoading)) && freezeLike) {
    result.state_id = "loader_stall_likely";
    result.confidence_level = i18n::ConfidenceLevel::kMedium;
    result.primary_reasons.push_back(
      language == i18n::Language::kEnglish
        ? L"Freeze was captured in a loading context"
        : L"로딩 문맥에서 프리징이 캡처됨");
    if (input.wct && input.wct->has_capture) {
      result.primary_reasons.push_back(
        language == i18n::Language::kEnglish
          ? L"WCT capture metadata stayed consistent with a loading stall"
          : L"WCT 캡처 메타데이터가 로딩 stall 정황과 일치함");
    }
  } else if (!input.actionable_candidates.empty()) {
    result.state_id = "freeze_candidate";
    result.confidence_level = i18n::ConfidenceLevel::kLow;
    result.primary_reasons.push_back(
      language == i18n::Language::kEnglish
        ? L"Freeze signals exist but do not isolate a deadlock or loader stall"
        : L"프리징 신호는 있으나 데드락/로더 stall로 단정되지는 않음");
  } else {
    result.state_id = "freeze_ambiguous";
    result.confidence_level = i18n::ConfidenceLevel::kLow;
    result.primary_reasons.push_back(
      language == i18n::Language::kEnglish
        ? L"Freeze evidence was too weak or conflicting"
        : L"프리징 근거가 약하거나 서로 충돌함");
  }

  result.confidence = i18n::ConfidenceText(language, result.confidence_level);
  const std::size_t maxCandidates = std::min<std::size_t>(input.actionable_candidates.size(), 2u);
  for (std::size_t i = 0; i < maxCandidates; ++i) {
    result.related_candidates.push_back(ToRelatedCandidate(input.actionable_candidates[i], language));
  }
  return result;
}

}  // namespace skydiag::dump_tool
