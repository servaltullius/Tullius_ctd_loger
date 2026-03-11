#include "EvidenceBuilderPrivate.h"

#include <algorithm>
#include <unordered_map>

#include "CandidateConsensus.h"

namespace skydiag::dump_tool::internal {
namespace {

constexpr std::size_t kMaxObjectRefs = 3;
constexpr std::size_t kMaxActionableStackSignals = 3;
constexpr std::size_t kMaxHistorySignals = 5;

std::uint32_t CrashLoggerWeight(const AnalysisResult::CrashLoggerModReference& ref)
{
  if (ref.relevance_score >= 16u) {
    return 6u;
  }
  if (ref.relevance_score >= 10u) {
    return 5u;
  }
  if (ref.relevance_score >= 6u) {
    return 4u;
  }
  return 3u;
}

std::uint32_t HistoryWeight(std::size_t priorCount)
{
  if (priorCount >= 3u) {
    return 3u;
  }
  if (priorCount >= 1u) {
    return 2u;
  }
  return 0u;
}

void AddCrashLoggerSignals(const AnalysisResult& r, bool en, std::vector<CandidateSignal>* out)
{
  if (!out) {
    return;
  }

  const std::size_t limit = std::min<std::size_t>(r.crash_logger_object_refs.size(), kMaxObjectRefs);
  for (std::size_t i = 0; i < limit; ++i) {
    const auto& ref = r.crash_logger_object_refs[i];
    CandidateSignal signal{};
    signal.family_id = "crash_logger_object_ref";
    signal.candidate_key = CanonicalCandidateKey(ref.esp_name);
    signal.display_name = ref.esp_name;
    signal.plugin_name = ref.esp_name;
    signal.detail = en
      ? (L"CrashLogger object ref: " + ref.esp_name)
      : (L"CrashLogger 오브젝트 참조: " + ref.esp_name);
    signal.weight = CrashLoggerWeight(ref);
    if (!signal.candidate_key.empty()) {
      out->push_back(std::move(signal));
    }
  }
}

void AddStackSignals(const AnalysisResult& r, bool en, std::vector<CandidateSignal>* out)
{
  if (!out) {
    return;
  }

  std::size_t added = 0;
  for (std::size_t i = 0; i < r.suspects.size() && added < kMaxActionableStackSignals; ++i) {
    const auto& suspect = r.suspects[i];
    if (!IsActionableSuspect(suspect)) {
      continue;
    }

    CandidateSignal signal{};
    signal.family_id = "actionable_stack";
    signal.candidate_key = CanonicalCandidateKey(!suspect.inferred_mod_name.empty() ? suspect.inferred_mod_name : suspect.module_filename);
    signal.display_name = !suspect.inferred_mod_name.empty() ? suspect.inferred_mod_name : suspect.module_filename;
    signal.mod_name = suspect.inferred_mod_name;
    signal.module_filename = suspect.module_filename;
    signal.detail = en
      ? (L"Actionable stack candidate: " + signal.display_name)
      : (L"실행 가능한 스택 후보: " + signal.display_name);
    signal.weight = r.suspects_from_stackwalk
      ? (added == 0 ? 5u : 4u)
      : 2u;
    if (!signal.candidate_key.empty()) {
      out->push_back(std::move(signal));
      ++added;
    }
  }
}

void AddResourceSignals(const AnalysisResult& r, bool en, std::vector<CandidateSignal>* out)
{
  if (!out) {
    return;
  }

  const auto anchorMs = InferCaptureAnchorMs(r);
  if (!anchorMs) {
    return;
  }

  constexpr double kWindowBeforeMs = 1000.0;
  constexpr double kWindowAfterMs = 150.0;
  const auto hits = FindResourcesNearAnchor(r.resources, *anchorMs, kWindowBeforeMs, kWindowAfterMs);
  if (hits.empty()) {
    return;
  }

  struct ProviderRow
  {
    std::wstring provider;
    double bestDistanceMs = 0.0;
    std::uint32_t hitCount = 0;
  };
  std::unordered_map<std::wstring, ProviderRow> byProvider;
  for (const auto* hit : hits) {
    if (!hit) {
      continue;
    }
    const double distanceMs = std::abs(hit->t_ms - *anchorMs);
    for (const auto& provider : hit->providers) {
      if (provider.empty()) {
        continue;
      }
      auto& row = byProvider[provider];
      row.provider = provider;
      row.hitCount += 1u;
      if (row.hitCount == 1u || distanceMs < row.bestDistanceMs) {
        row.bestDistanceMs = distanceMs;
      }
    }
  }

  std::vector<ProviderRow> rows;
  rows.reserve(byProvider.size());
  for (auto& [provider, row] : byProvider) {
    (void)provider;
    rows.push_back(std::move(row));
  }
  std::sort(rows.begin(), rows.end(), [](const ProviderRow& a, const ProviderRow& b) {
    if (a.hitCount != b.hitCount) {
      return a.hitCount > b.hitCount;
    }
    return a.bestDistanceMs < b.bestDistanceMs;
  });

  const std::size_t limit = std::min<std::size_t>(rows.size(), 3);
  for (std::size_t i = 0; i < limit; ++i) {
    const auto& row = rows[i];
    CandidateSignal signal{};
    signal.family_id = "resource_provider";
    signal.candidate_key = CanonicalCandidateKey(row.provider);
    signal.display_name = row.provider;
    signal.mod_name = row.provider;
    signal.detail = en
      ? (L"Near-timestamp resources from " + row.provider)
      : (row.provider + L" 제공 리소스가 사고 직전 감지됨");
    signal.weight = (row.bestDistanceMs <= 300.0) ? 4u : 3u;
    if (!signal.candidate_key.empty()) {
      out->push_back(std::move(signal));
    }
  }
}

void AddHistorySignals(const AnalysisResult& r, bool en, std::vector<CandidateSignal>* out)
{
  if (!out) {
    return;
  }

  std::size_t added = 0;
  for (const auto& repeat : r.bucket_candidate_repeats) {
    if (added >= kMaxHistorySignals) {
      break;
    }

    const auto weight = HistoryWeight(repeat.prior_count);
    if (weight == 0u || repeat.candidate_key.empty()) {
      continue;
    }

    CandidateSignal signal{};
    signal.family_id = "history_repeat";
    signal.candidate_key = ToWideAscii(repeat.candidate_key);
    signal.display_name = ToWideAscii(repeat.candidate_key);
    signal.detail = en
      ? (L"Repeated in the same crash bucket (" + std::to_wstring(repeat.prior_count + 1u) + L" incidents total)")
      : (L"같은 크래시 버킷에서 반복됨 (" + std::to_wstring(repeat.prior_count + 1u) + L"건)");
    signal.weight = weight;
    out->push_back(std::move(signal));
    ++added;
  }
}

}  // namespace

void BuildActionableCandidates(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx)
{
  r.actionable_candidates.clear();

  if (!(ctx.isGameExe || ctx.isSystem || ctx.isHookFramework || ctx.isHangLike)) {
    return;
  }

  const bool en = (lang == i18n::Language::kEnglish);
  std::vector<CandidateSignal> signals;
  signals.reserve(12);
  AddCrashLoggerSignals(r, en, &signals);
  AddStackSignals(r, en, &signals);
  AddResourceSignals(r, en, &signals);
  AddHistorySignals(r, en, &signals);

  r.actionable_candidates = BuildCandidateConsensus(signals, lang);
}

}  // namespace skydiag::dump_tool::internal
