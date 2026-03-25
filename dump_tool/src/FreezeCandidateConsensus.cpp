#include "FreezeCandidateConsensus.h"

#include <algorithm>
#include <unordered_set>

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

std::wstring JoinModules(const std::vector<std::wstring>& items, std::size_t maxCount)
{
  std::wstring joined;
  const std::size_t limit = std::min(items.size(), maxCount);
  for (std::size_t i = 0; i < limit; ++i) {
    if (i != 0u) {
      joined += L", ";
    }
    joined += items[i];
  }
  return joined;
}

bool HasSnapshotBackedCapture(const FreezeSignalInput& input)
{
  return input.wct && input.wct->pss_snapshot_used;
}

bool HasSnapshotFallbackCapture(const FreezeSignalInput& input)
{
  return input.wct && input.wct->pss_snapshot_requested && !input.wct->pss_snapshot_used;
}

bool HasLiveProcessCapture(const FreezeSignalInput& input)
{
  return input.wct && input.wct->has_capture;
}

bool HasConsensusBackedDeadlock(const FreezeSignalInput& input)
{
  return input.wct &&
         input.wct->cycle_consensus &&
         !input.wct->repeated_cycle_tids.empty() &&
         input.wct->pss_snapshot_used;
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

FreezeRelatedCandidate ToRelatedModuleCandidate(std::wstring name, i18n::Language language)
{
  FreezeRelatedCandidate row{};
  row.confidence_level = i18n::ConfidenceLevel::kLow;
  row.confidence = i18n::ConfidenceText(language, row.confidence_level);
  row.display_name = std::move(name);
  return row;
}

bool HasStrongFirstChanceLoaderSignal(const FreezeSignalInput& input, bool loadingSignal)
{
  return loadingSignal &&
         input.first_chance.has_value() &&
         input.first_chance->has_context &&
         (input.first_chance->loading_window_count >= 2u || input.first_chance->repeated_signature_count > 0u);
}

bool HasConsensusBackedLoaderSignal(const FreezeSignalInput& input, bool strongFirstChanceLoaderSignal)
{
  if (!input.wct || !input.wct->pss_snapshot_used || !input.wct->consistent_loading_signal) {
    return false;
  }

  const bool repeatedLoadingContext =
    (input.blackbox.has_value() && input.blackbox->loading_window) ||
    strongFirstChanceLoaderSignal;
  return repeatedLoadingContext;
}

std::string DetermineSupportQuality(
  const FreezeSignalInput& input,
  bool consensusBackedDeadlock,
  bool consensusBackedLoaderSignal)
{
  if (consensusBackedDeadlock || consensusBackedLoaderSignal) {
    return "snapshot_consensus_backed";
  }
  if (HasSnapshotBackedCapture(input)) {
    return "snapshot_backed";
  }
  if (HasSnapshotFallbackCapture(input)) {
    return "snapshot_fallback";
  }
  if (HasLiveProcessCapture(input)) {
    return "live_process";
  }
  return "unknown";
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
  if (input.blackbox.has_value()) {
    result.blackbox_context = *input.blackbox;
  } else {
    result.blackbox_context.loading_window = input.loading_context;
  }
  if (input.first_chance.has_value()) {
    result.first_chance_context = *input.first_chance;
  }

  const bool loadingSignal = input.loading_context || (input.wct && input.wct->isLoading);
  const bool hasBlackboxChurn = input.blackbox.has_value() &&
    (input.blackbox->module_churn_score > 0u || input.blackbox->thread_churn_score > 0u);
  const bool strongFirstChanceLoaderSignal = HasStrongFirstChanceLoaderSignal(input, loadingSignal);
  const bool consensusBackedDeadlock = HasConsensusBackedDeadlock(input);
  const bool consensusBackedLoaderSignal = HasConsensusBackedLoaderSignal(input, strongFirstChanceLoaderSignal);
  const bool strongLoaderContext =
    (loadingSignal && input.blackbox.has_value() && input.blackbox->module_churn_score >= 3u) ||
    strongFirstChanceLoaderSignal;
  result.support_quality = DetermineSupportQuality(input, consensusBackedDeadlock, consensusBackedLoaderSignal);

  if (input.wct && input.wct->cycles > 0) {
    result.state_id = "deadlock_likely";
    const bool repeatedCycleSupport =
      consensusBackedDeadlock || (input.wct->cycle_consensus && input.wct->longest_wait_tid_consensus);
    result.confidence_level = repeatedCycleSupport
      ? i18n::ConfidenceLevel::kHigh
      : i18n::ConfidenceLevel::kMedium;
    result.primary_reasons.push_back(
      language == i18n::Language::kEnglish
        ? L"WCT reported cycle threads"
        : L"WCT에서 cycle thread가 감지됨");
    if (input.wct->cycle_consensus && !input.wct->repeated_cycle_tids.empty()) {
      result.primary_reasons.push_back(
        language == i18n::Language::kEnglish
          ? (L"Repeated WCT captures preserved the same cycle thread set (count=" +
              std::to_wstring(input.wct->repeated_cycle_tids.size()) + L")")
          : (L"반복 WCT 캡처에서 같은 cycle thread 집합이 유지됨 (count=" +
              std::to_wstring(input.wct->repeated_cycle_tids.size()) + L")"));
    }
    if (input.wct->longest_wait_tid != 0u) {
      result.primary_reasons.push_back(
        language == i18n::Language::kEnglish
          ? L"A blocked thread with the longest observed wait was identified"
          : L"가장 오래 기다린 blocked thread가 식별됨");
      if (input.wct->longest_wait_tid_consensus) {
        result.primary_reasons.push_back(
          language == i18n::Language::kEnglish
            ? L"The longest-wait thread stayed stable across repeated WCT captures"
            : L"반복 WCT 캡처에서도 longest-wait thread가 유지됨");
      }
    }
  } else if (loadingSignal && freezeLike) {
    result.state_id = "loader_stall_likely";
    result.confidence_level = (strongLoaderContext || consensusBackedLoaderSignal)
      ? i18n::ConfidenceLevel::kHigh
      : i18n::ConfidenceLevel::kMedium;
    result.primary_reasons.push_back(
      language == i18n::Language::kEnglish
        ? L"Freeze was captured in a loading context"
        : L"로딩 문맥에서 프리징이 캡처됨");
    if (consensusBackedLoaderSignal) {
      result.primary_reasons.push_back(
        language == i18n::Language::kEnglish
          ? L"Repeated WCT captures stayed aligned with the loading window"
          : L"반복 WCT 캡처가 동일한 로딩 윈도우와 계속 일치함");
    } else if (input.wct && input.wct->has_capture) {
      result.primary_reasons.push_back(
        language == i18n::Language::kEnglish
          ? L"WCT capture metadata stayed consistent with a loading stall"
          : L"WCT 캡처 메타데이터가 로딩 stall 정황과 일치함");
    }
    if (input.blackbox.has_value()) {
      if (input.blackbox->module_churn_score > 0u) {
        result.primary_reasons.push_back(
          language == i18n::Language::kEnglish
            ? (L"blackbox module churn rose during loading (score=" + std::to_wstring(input.blackbox->module_churn_score) + L")")
            : (L"로딩 중 blackbox module churn이 증가함 (score=" + std::to_wstring(input.blackbox->module_churn_score) + L")"));
      }
      if (input.blackbox->thread_churn_score > 0u) {
        result.primary_reasons.push_back(
          language == i18n::Language::kEnglish
            ? (L"thread churn accompanied the loading stall (score=" + std::to_wstring(input.blackbox->thread_churn_score) + L")")
            : (L"thread churn이 로딩 stall과 함께 관찰됨 (score=" + std::to_wstring(input.blackbox->thread_churn_score) + L")"));
      }
    }
    if (strongFirstChanceLoaderSignal) {
      result.primary_reasons.push_back(
        language == i18n::Language::kEnglish
          ? (L"repeated suspicious first-chance exceptions appeared during loading (count=" +
              std::to_wstring(input.first_chance->loading_window_count) + L")")
          : (L"로딩 중 반복적인 suspicious first-chance 예외가 관찰됨 (count=" +
              std::to_wstring(input.first_chance->loading_window_count) + L")"));
      if (!input.first_chance->recent_non_system_modules.empty()) {
        result.primary_reasons.push_back(
          language == i18n::Language::kEnglish
            ? (L"first-chance context highlighted non-system modules: " +
                JoinModules(input.first_chance->recent_non_system_modules, 3))
            : (L"first-chance 문맥에서 비시스템 모듈이 강조됨: " +
                JoinModules(input.first_chance->recent_non_system_modules, 3)));
      }
    }
  } else if (!input.actionable_candidates.empty()) {
    result.state_id = "freeze_candidate";
    result.confidence_level = i18n::ConfidenceLevel::kLow;
    result.primary_reasons.push_back(
      language == i18n::Language::kEnglish
        ? L"Freeze signals exist but do not isolate a deadlock or loader stall"
        : L"프리징 신호는 있으나 데드락/로더 stall로 단정되지는 않음");
    if (hasBlackboxChurn) {
      result.primary_reasons.push_back(
        language == i18n::Language::kEnglish
          ? L"blackbox captured module/thread churn, but not inside a decisive loading window"
          : L"blackbox에서 module/thread churn이 보였지만 결정적인 로딩 윈도우와는 맞지 않음");
    }
  } else {
    result.state_id = "freeze_ambiguous";
    result.confidence_level = i18n::ConfidenceLevel::kLow;
    result.primary_reasons.push_back(
      language == i18n::Language::kEnglish
        ? L"Freeze evidence was too weak or conflicting"
        : L"프리징 근거가 약하거나 서로 충돌함");
    if (hasBlackboxChurn) {
      result.primary_reasons.push_back(
        language == i18n::Language::kEnglish
          ? L"blackbox saw churn, but not enough to distinguish deadlock from loader stall"
          : L"blackbox churn은 있으나 deadlock과 loader stall을 구분할 정도는 아님");
    }
  }

  result.confidence = i18n::ConfidenceText(language, result.confidence_level);
  std::unordered_set<std::wstring> seenNames;
  const std::size_t maxCandidates = std::min<std::size_t>(input.actionable_candidates.size(), 2u);
  for (std::size_t i = 0; i < maxCandidates; ++i) {
    auto related = ToRelatedCandidate(input.actionable_candidates[i], language);
    seenNames.insert(related.display_name);
    result.related_candidates.push_back(std::move(related));
  }
  if (input.blackbox.has_value()) {
    for (const auto& moduleName : input.blackbox->recent_non_system_modules) {
      if (moduleName.empty() || seenNames.contains(moduleName)) {
        continue;
      }
      result.related_candidates.push_back(ToRelatedModuleCandidate(moduleName, language));
      seenNames.insert(moduleName);
      if (result.related_candidates.size() >= 4u) {
        break;
      }
    }
  }
  if (input.first_chance.has_value()) {
    for (const auto& moduleName : input.first_chance->recent_non_system_modules) {
      if (moduleName.empty() || seenNames.contains(moduleName)) {
        continue;
      }
      result.related_candidates.push_back(ToRelatedModuleCandidate(moduleName, language));
      seenNames.insert(moduleName);
      if (result.related_candidates.size() >= 4u) {
        break;
      }
    }
  }
  return result;
}

}  // namespace skydiag::dump_tool
