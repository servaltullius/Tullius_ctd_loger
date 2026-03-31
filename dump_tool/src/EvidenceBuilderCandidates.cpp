#include "EvidenceBuilderPrivate.h"

#include <algorithm>
#include <unordered_map>

#include "CandidateConsensus.h"

namespace skydiag::dump_tool::internal {
namespace {

constexpr std::size_t kMaxObjectRefs = 3;
constexpr std::size_t kMaxActionableStackSignals = 3;
constexpr std::size_t kMaxHistorySignals = 5;
constexpr std::size_t kMaxFirstChanceSignals = 4;

bool HasRicherStackCapture(const AnalysisResult& r)
{
  return r.incident_capture_profile_process_thread_data ||
         r.incident_capture_profile_full_memory_info ||
         r.incident_capture_profile_module_headers;
}

std::uint32_t CaptureQualityStackWeight(const AnalysisResult& r, std::size_t stackRank)
{
  if (!r.suspects_from_stackwalk) {
    return 0u;
  }

  std::uint32_t weight = 0u;
  if (HasRicherStackCapture(r)) {
    weight += 1u;
  }
  if (stackRank == 0u && r.symbol_runtime_degraded && r.incident_capture_profile_indirect_memory) {
    weight += 1u;
  }
  return weight;
}

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

std::wstring WideLowerLocal(std::wstring_view value)
{
  std::wstring out;
  out.reserve(value.size());
  for (const wchar_t ch : value) {
    out.push_back(static_cast<wchar_t>(towlower(ch)));
  }
  return out;
}

void PopulateFrameSignalCandidate(const AnalysisResult& r, std::wstring_view module, CandidateSignal* signal)
{
  if (!signal) {
    return;
  }

  const std::wstring moduleLower = WideLowerLocal(module);
  for (const auto& suspect : r.suspects) {
    if (!IsActionableSuspect(suspect)) {
      continue;
    }
    if (WideLowerLocal(suspect.module_filename) != moduleLower) {
      continue;
    }

    signal->candidate_key = CanonicalCandidateKey(
      !suspect.inferred_mod_name.empty() ? suspect.inferred_mod_name : suspect.module_filename);
    signal->display_name = !suspect.inferred_mod_name.empty() ? suspect.inferred_mod_name : suspect.module_filename;
    signal->mod_name = suspect.inferred_mod_name;
    signal->module_filename = suspect.module_filename;
    return;
  }

  signal->candidate_key = CanonicalCandidateKey(module);
  signal->display_name = std::wstring(module);
  signal->module_filename = std::wstring(module);
}

void AddCrashLoggerFrameSignal(
  const AnalysisResult& r,
  std::wstring_view module,
  std::uint32_t weight,
  std::wstring&& detail,
  std::vector<CandidateSignal>* out)
{
  if (!out || module.empty() || weight == 0u) {
    return;
  }

  CandidateSignal signal{};
  signal.family_id = "crash_logger_frame";
  PopulateFrameSignalCandidate(r, module, &signal);
  signal.detail = std::move(detail);
  signal.weight = weight;
  if (!signal.candidate_key.empty()) {
    out->push_back(std::move(signal));
  }
}

void AddCrashLoggerFrameSignals(const AnalysisResult& r, bool en, std::vector<CandidateSignal>* out)
{
  if (!out) {
    return;
  }

  if (!r.crash_logger_direct_fault_module.empty()) {
    AddCrashLoggerFrameSignal(
      r,
      r.crash_logger_direct_fault_module,
      8u,
      en
        ? (L"CrashLogger direct-fault frame: " + r.crash_logger_direct_fault_module)
        : (L"CrashLogger direct-fault 프레임: " + r.crash_logger_direct_fault_module),
      out);
  }

  if (!r.crash_logger_first_actionable_probable_module.empty()) {
    AddCrashLoggerFrameSignal(
      r,
      r.crash_logger_first_actionable_probable_module,
      6u,
      en
        ? (L"CrashLogger first actionable probable frame: " + r.crash_logger_first_actionable_probable_module)
        : (L"CrashLogger 첫 actionable probable 프레임: " + r.crash_logger_first_actionable_probable_module),
      out);
  }

  if (r.crash_logger_probable_streak_length >= 2u && !r.crash_logger_probable_streak_module.empty()) {
    AddCrashLoggerFrameSignal(
      r,
      r.crash_logger_probable_streak_module,
      (r.crash_logger_probable_streak_length >= 3u) ? 6u : 5u,
      en
        ? (L"CrashLogger probable frame streak (" + std::to_wstring(r.crash_logger_probable_streak_length) +
           L"x): " + r.crash_logger_probable_streak_module)
        : (L"CrashLogger probable 프레임 streak (" + std::to_wstring(r.crash_logger_probable_streak_length) +
           L"회): " + r.crash_logger_probable_streak_module),
      out);
  }
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

bool HasDenseFirstChanceLoadingWindow(const FirstChanceSummary& summary)
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

std::uint32_t FirstChanceWeight(const FirstChanceSummary& summary)
{
  if (summary.repeated_signature_count > 0u) {
    return 3u;
  }
  if (HasDenseFirstChanceLoadingWindow(summary)) {
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
      ? (r.symbol_runtime_degraded ? (added == 0 ? 3u : 2u) : (added == 0 ? 5u : 4u))
      : 2u;
    if (!signal.candidate_key.empty()) {
      out->push_back(std::move(signal));
      const auto captureQualityWeight = CaptureQualityStackWeight(r, added);
      if (captureQualityWeight > 0u) {
        CandidateSignal captureSignal{};
        captureSignal.family_id = "capture_quality_stack";
        captureSignal.candidate_key = CanonicalCandidateKey(!suspect.inferred_mod_name.empty() ? suspect.inferred_mod_name : suspect.module_filename);
        captureSignal.display_name = !suspect.inferred_mod_name.empty() ? suspect.inferred_mod_name : suspect.module_filename;
        captureSignal.mod_name = suspect.inferred_mod_name;
        captureSignal.module_filename = suspect.module_filename;
        captureSignal.detail = en
          ? (L"Richer capture profile preserved extra stackwalk context: " + captureSignal.display_name)
          : (L"강화된 캡처 프로필이 추가 stackwalk 문맥을 보존함: " + captureSignal.display_name);
        captureSignal.weight = captureQualityWeight;
        out->push_back(std::move(captureSignal));
      }
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
    signal.weight = (row.hitCount >= 2u && row.bestDistanceMs <= 150.0) ? 5u
                  : (((row.bestDistanceMs <= 300.0) || row.hitCount >= 2u) ? 4u : 3u);
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

void AddFirstChanceSignalForCandidate(
  std::wstring_view candidateKey,
  std::wstring_view displayName,
  std::wstring_view pluginName,
  std::wstring_view modName,
  std::wstring_view moduleFilename,
  std::uint32_t weight,
  const std::wstring& detail,
  std::unordered_map<std::wstring, CandidateSignal>* byKey)
{
  if (!byKey || candidateKey.empty() || weight == 0u) {
    return;
  }

  CandidateSignal signal{};
  signal.family_id = "first_chance_context";
  signal.candidate_key = std::wstring(candidateKey);
  signal.display_name = std::wstring(displayName);
  signal.plugin_name = std::wstring(pluginName);
  signal.mod_name = std::wstring(modName);
  signal.module_filename = std::wstring(moduleFilename);
  signal.detail = detail;
  signal.weight = weight;

  auto [it, inserted] = byKey->emplace(signal.candidate_key, signal);
  if (!inserted && weight > it->second.weight) {
    it->second = std::move(signal);
  }
}

void AddFirstChanceSignals(
  const AnalysisResult& r,
  bool en,
  const EvidenceBuildContext& ctx,
  std::vector<CandidateSignal>* out)
{
  if (!out) {
    return;
  }
  if (!(ctx.isGameExe || ctx.isSystem) || !ctx.isCrashLike || ctx.isHangLike || ctx.isSnapshotLike) {
    return;
  }
  if (!HasScorableFirstChanceContext(r.first_chance_summary)) {
    return;
  }

  const auto weight = FirstChanceWeight(r.first_chance_summary);
  if (weight == 0u) {
    return;
  }

  std::unordered_map<std::wstring, CandidateSignal> byKey;
  byKey.reserve(kMaxFirstChanceSignals);
  for (const auto& moduleName : r.first_chance_summary.recent_non_system_modules) {
    if (byKey.size() >= kMaxFirstChanceSignals) {
      break;
    }

    const auto moduleKey = CanonicalCandidateKey(moduleName);
    if (moduleKey.empty()) {
      continue;
    }

    const std::wstring detail = en
      ? (L"Repeated suspicious first-chance context matched " + moduleName)
      : (L"반복 suspicious first-chance 문맥이 " + moduleName + L" 과(와) 연결됨");

    AddFirstChanceSignalForCandidate(
      moduleKey,
      moduleName,
      L"",
      L"",
      moduleName,
      weight,
      detail,
      &byKey);

    for (const auto& ref : r.crash_logger_object_refs) {
      if (CanonicalCandidateKey(ref.esp_name) != moduleKey) {
        continue;
      }
      AddFirstChanceSignalForCandidate(
        CanonicalCandidateKey(ref.esp_name),
        ref.esp_name,
        ref.esp_name,
        L"",
        moduleName,
        weight,
        detail,
        &byKey);
    }

    for (const auto& suspect : r.suspects) {
      if (!IsActionableSuspect(suspect)) {
        continue;
      }
      const auto suspectModuleKey = CanonicalCandidateKey(suspect.module_filename);
      const auto suspectModKey = CanonicalCandidateKey(suspect.inferred_mod_name);
      if (suspectModuleKey != moduleKey && suspectModKey != moduleKey) {
        continue;
      }
      const auto candidateKey = CanonicalCandidateKey(
        !suspect.inferred_mod_name.empty() ? suspect.inferred_mod_name : suspect.module_filename);
      AddFirstChanceSignalForCandidate(
        candidateKey,
        !suspect.inferred_mod_name.empty() ? suspect.inferred_mod_name : suspect.module_filename,
        L"",
        suspect.inferred_mod_name,
        suspect.module_filename,
        weight,
        detail,
        &byKey);
    }
  }

  std::vector<CandidateSignal> ranked;
  ranked.reserve(byKey.size());
  for (auto& [_, signal] : byKey) {
    ranked.push_back(std::move(signal));
  }
  std::sort(ranked.begin(), ranked.end(), [](const CandidateSignal& lhs, const CandidateSignal& rhs) {
    if (lhs.weight != rhs.weight) {
      return lhs.weight > rhs.weight;
    }
    return lhs.display_name < rhs.display_name;
  });

  const auto limit = std::min<std::size_t>(ranked.size(), kMaxFirstChanceSignals);
  for (std::size_t i = 0; i < limit; ++i) {
    out->push_back(std::move(ranked[i]));
  }
}

}  // namespace

void BuildActionableCandidates(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx)
{
  r.actionable_candidates.clear();

  if (ctx.isSnapshotLike) {
    return;
  }

  const bool en = (lang == i18n::Language::kEnglish);
  std::vector<CandidateSignal> signals;
  signals.reserve(16);
  AddCrashLoggerFrameSignals(r, en, &signals);
  AddCrashLoggerSignals(r, en, &signals);
  AddStackSignals(r, en, &signals);
  AddResourceSignals(r, en, &signals);
  AddHistorySignals(r, en, &signals);
  AddFirstChanceSignals(r, en, ctx, &signals);

  r.actionable_candidates = BuildCandidateConsensus(signals, lang);
  for (auto& candidate : r.actionable_candidates) {
    if (!IsWeakFaultLocationActionableCandidate(r, candidate, ctx)) {
      continue;
    }

    const bool hasStackSupport =
      std::find(candidate.supporting_families.begin(), candidate.supporting_families.end(), "actionable_stack") !=
      candidate.supporting_families.end();
    candidate.status_id = hasStackSupport ? "related" : "reference_clue";
    candidate.confidence_level = i18n::ConfidenceLevel::kLow;
    candidate.confidence = i18n::ConfidenceText(lang, candidate.confidence_level);
    candidate.cross_validated = false;
    candidate.explanation = en
      ? (hasStackSupport
          ? L"Crash Logger frame and the same-dump stack both land in this DLL, but this is still only the current fault location."
          : L"Crash Logger frame support for this DLL is still limited to the current fault location.")
      : (hasStackSupport
          ? L"Crash Logger 프레임과 같은 덤프의 스택이 모두 이 DLL에 걸리지만, 여전히 현재 fault location 단서에 머뭅니다."
          : L"이 DLL에 대한 Crash Logger 프레임 근거는 여전히 현재 fault location 단서에 머뭅니다.");
  }
}

}  // namespace skydiag::dump_tool::internal
