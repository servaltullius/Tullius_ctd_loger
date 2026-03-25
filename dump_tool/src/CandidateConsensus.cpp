#include "CandidateConsensus.h"

#include <algorithm>
#include <cwctype>
#include <unordered_map>
#include <utility>

namespace skydiag::dump_tool {
namespace {

constexpr const char* kFamilyCrashLoggerFrame = "crash_logger_frame";
constexpr const char* kFamilyCrashLoggerObjectRef = "crash_logger_object_ref";
constexpr const char* kFamilyStack = "actionable_stack";
constexpr const char* kFamilyCaptureQualityStack = "capture_quality_stack";
constexpr const char* kFamilyResource = "resource_provider";
constexpr const char* kFamilyHistory = "history_repeat";
constexpr const char* kFamilyFirstChance = "first_chance_context";
constexpr std::uint32_t kCrossValidatedScoreThreshold = 10u;
constexpr std::uint32_t kFrameConflictWeightThreshold = 6u;
constexpr std::uint32_t kStackConflictWeightThreshold = 4u;
constexpr std::uint32_t kObjectRefConflictWeightThreshold = 5u;

struct CandidateRow
{
  ActionableCandidate candidate;
  std::unordered_map<std::string, std::uint32_t> family_weight;
};

bool RowHasFamily(const CandidateRow& row, std::string_view familyId)
{
  return row.family_weight.find(std::string(familyId)) != row.family_weight.end();
}

std::uint32_t FamilyWeight(const CandidateRow& row, std::string_view familyId)
{
  if (const auto it = row.family_weight.find(std::string(familyId)); it != row.family_weight.end()) {
    return it->second;
  }
  return 0u;
}

bool IsBoostOnlyFamily(std::string_view familyId)
{
  return familyId == kFamilyHistory || familyId == kFamilyCaptureQualityStack;
}

std::uint32_t RowScore(const CandidateRow& row)
{
  std::uint32_t score = 0;
  for (const auto& [_, weight] : row.family_weight) {
    score += weight;
  }
  return score;
}

std::size_t CountNonBoostFamilies(const CandidateRow& row)
{
  std::size_t count = 0;
  for (const auto& [familyId, _] : row.family_weight) {
    if (!IsBoostOnlyFamily(familyId)) {
      ++count;
    }
  }
  return count;
}

bool HasFamily(const ActionableCandidate& candidate, std::string_view familyId)
{
  return std::find(candidate.supporting_families.begin(), candidate.supporting_families.end(), familyId) !=
         candidate.supporting_families.end();
}

bool HasStrongFamily(const ActionableCandidate& candidate)
{
  return HasFamily(candidate, kFamilyCrashLoggerFrame) ||
         HasFamily(candidate, kFamilyCrashLoggerObjectRef) ||
         HasFamily(candidate, kFamilyStack);
}

bool HasActionableCrossValidation(const CandidateRow& row)
{
  return (RowHasFamily(row, kFamilyCrashLoggerFrame) || RowHasFamily(row, kFamilyCrashLoggerObjectRef)) &&
         RowHasFamily(row, kFamilyStack);
}

bool HasFrameAndStackConsensus(const CandidateRow& row)
{
  return RowHasFamily(row, kFamilyCrashLoggerFrame) &&
         RowHasFamily(row, kFamilyStack) &&
         CountNonBoostFamilies(row) >= 2 &&
         RowScore(row) >= kCrossValidatedScoreThreshold;
}

bool IsObjectRefOnlyClue(const CandidateRow& row)
{
  return RowHasFamily(row, kFamilyCrashLoggerObjectRef) &&
         !RowHasFamily(row, kFamilyCrashLoggerFrame) &&
         !RowHasFamily(row, kFamilyStack) &&
         !RowHasFamily(row, kFamilyResource);
}

int StatusRank(const ActionableCandidate& candidate)
{
  if (candidate.status_id == "cross_validated") {
    return 4;
  }
  if (candidate.status_id == "related") {
    return 3;
  }
  if (candidate.status_id == "conflicting") {
    return 2;
  }
  if (candidate.status_id == "reference_clue") {
    return 1;
  }
  return 0;
}

std::wstring PickDisplayName(const ActionableCandidate& candidate)
{
  if (!candidate.plugin_name.empty()) {
    return candidate.plugin_name;
  }
  if (!candidate.module_filename.empty()) {
    return candidate.module_filename;
  }
  if (!candidate.mod_name.empty()) {
    return candidate.mod_name;
  }
  return candidate.display_name;
}

std::wstring PickSecondaryLabel(const ActionableCandidate& candidate, std::wstring_view primaryIdentifier, std::wstring_view fallbackLabel)
{
  const std::wstring_view candidates[] = {
    fallbackLabel,
    candidate.mod_name,
    candidate.plugin_name,
    candidate.module_filename,
  };

  for (const auto& value : candidates) {
    if (!value.empty() && value != primaryIdentifier) {
      return std::wstring(value);
    }
  }
  return L"";
}

void RefreshCandidateFields(CandidateRow* row, i18n::Language language)
{
  if (!row) {
    return;
  }

  auto& candidate = row->candidate;
  candidate.supporting_families.clear();
  candidate.supporting_families.reserve(row->family_weight.size());
  candidate.score = 0;
  for (const auto& [familyId, weight] : row->family_weight) {
    candidate.supporting_families.push_back(familyId);
    candidate.score += weight;
  }
  std::sort(candidate.supporting_families.begin(), candidate.supporting_families.end());
  candidate.family_count = static_cast<std::uint32_t>(candidate.supporting_families.size());
  const std::wstring fallbackLabel = candidate.display_name;
  candidate.primary_identifier = PickDisplayName(candidate);
  candidate.secondary_label = PickSecondaryLabel(candidate, candidate.primary_identifier, fallbackLabel);
  candidate.display_name = candidate.primary_identifier;

  const bool hasCrashLoggerFrame = HasFamily(candidate, kFamilyCrashLoggerFrame);
  const bool hasCrashLoggerObjectRef = HasFamily(candidate, kFamilyCrashLoggerObjectRef);
  const bool hasStack = HasFamily(candidate, kFamilyStack);
  const bool hasCaptureQualityStack = HasFamily(candidate, kFamilyCaptureQualityStack);
  const bool hasResource = HasFamily(candidate, kFamilyResource);
  const bool conflict = candidate.has_conflict;
  const std::size_t nonBoostFamilyCount = CountNonBoostFamilies(*row);
  const bool crossValidated =
    !conflict &&
    (hasCrashLoggerFrame || hasCrashLoggerObjectRef) &&
    hasStack &&
    nonBoostFamilyCount >= 2 &&
    candidate.score >= kCrossValidatedScoreThreshold;
  const bool frameAndObjectRef = hasCrashLoggerFrame && hasCrashLoggerObjectRef;
  const bool frameOnly = hasCrashLoggerFrame && !hasStack && !hasCrashLoggerObjectRef && !hasResource;
  const bool strongFrameOnly = frameOnly && FamilyWeight(*row, kFamilyCrashLoggerFrame) >= 6u;
  const bool stackOnly = hasStack && !hasCrashLoggerFrame && !hasCrashLoggerObjectRef && !hasResource;
  const bool strongStackOnly = stackOnly && FamilyWeight(*row, kFamilyStack) >= 5u;
  const bool captureBackedStrongStackOnly =
    stackOnly && hasCaptureQualityStack && FamilyWeight(*row, kFamilyStack) >= 4u;
  const bool objectRefWithHistory =
    hasCrashLoggerObjectRef &&
    HasFamily(candidate, kFamilyHistory) &&
    !hasCrashLoggerFrame &&
    !hasStack &&
    !hasResource;

  if (conflict) {
    candidate.status_id = "conflicting";
    candidate.confidence_level = i18n::ConfidenceLevel::kMedium;
    candidate.cross_validated = false;
  } else if (crossValidated) {
    candidate.status_id = "cross_validated";
    candidate.confidence_level = i18n::ConfidenceLevel::kHigh;
    candidate.cross_validated = true;
  } else if (frameAndObjectRef) {
    candidate.status_id = "related";
    candidate.confidence_level = i18n::ConfidenceLevel::kMedium;
    candidate.cross_validated = false;
  } else if (objectRefWithHistory) {
    candidate.status_id = "related";
    candidate.confidence_level = i18n::ConfidenceLevel::kLow;
    candidate.cross_validated = false;
  } else if (strongFrameOnly) {
    candidate.status_id = "related";
    candidate.confidence_level = i18n::ConfidenceLevel::kMedium;
    candidate.cross_validated = false;
  } else if (strongStackOnly) {
    candidate.status_id = "related";
    candidate.confidence_level = i18n::ConfidenceLevel::kMedium;
    candidate.cross_validated = false;
  } else if (captureBackedStrongStackOnly) {
    candidate.status_id = "related";
    candidate.confidence_level = i18n::ConfidenceLevel::kMedium;
    candidate.cross_validated = false;
  } else if (nonBoostFamilyCount >= 2 && candidate.score >= 7 && HasStrongFamily(candidate)) {
    candidate.status_id = "related";
    candidate.confidence_level = i18n::ConfidenceLevel::kMedium;
    candidate.cross_validated = false;
  } else if (frameOnly) {
    candidate.status_id = "reference_clue";
    candidate.confidence_level = i18n::ConfidenceLevel::kMedium;
    candidate.cross_validated = false;
  } else if (hasCrashLoggerObjectRef) {
    candidate.status_id = "reference_clue";
    candidate.confidence_level = i18n::ConfidenceLevel::kLow;
    candidate.cross_validated = false;
  } else if (hasStack || hasResource) {
    candidate.status_id = "related";
    candidate.confidence_level = i18n::ConfidenceLevel::kLow;
    candidate.cross_validated = false;
  } else {
    candidate.status_id.clear();
    candidate.confidence_level = i18n::ConfidenceLevel::kUnknown;
    candidate.cross_validated = false;
  }

  candidate.confidence = i18n::ConfidenceText(language, candidate.confidence_level);
}

bool IsStandaloneCandidateAllowed(const ActionableCandidate& candidate)
{
  if (candidate.supporting_families.empty()) {
    return false;
  }
  if (candidate.supporting_families.size() == 1 && IsBoostOnlyFamily(candidate.supporting_families.front())) {
    return false;
  }
  return !candidate.status_id.empty();
}

}  // namespace

std::wstring CanonicalCandidateKey(std::wstring_view value)
{
  std::wstring key;
  key.reserve(value.size());
  for (wchar_t ch : value) {
    const wchar_t lower = static_cast<wchar_t>(towlower(ch));
    if ((lower >= L'a' && lower <= L'z') || (lower >= L'0' && lower <= L'9')) {
      key.push_back(lower);
    }
  }

  const std::wstring candidates[] = { L"esp", L"esm", L"esl", L"dll", L"exe" };
  for (const auto& suffix : candidates) {
    if (key.size() > suffix.size() && key.ends_with(suffix)) {
      key.erase(key.size() - suffix.size());
      break;
    }
  }
  return key;
}

std::vector<ActionableCandidate> BuildCandidateConsensus(const std::vector<CandidateSignal>& signals, i18n::Language language)
{
  std::unordered_map<std::wstring, CandidateRow> rowsByKey;
  rowsByKey.reserve(signals.size());

  for (const auto& signal : signals) {
    if (signal.candidate_key.empty() || signal.family_id.empty() || signal.weight == 0) {
      continue;
    }

    auto& row = rowsByKey[signal.candidate_key];
    if (row.candidate.display_name.empty()) {
      row.candidate.display_name = signal.display_name;
    }
    if (!signal.plugin_name.empty()) {
      row.candidate.plugin_name = signal.plugin_name;
    }
    if (!signal.mod_name.empty()) {
      row.candidate.mod_name = signal.mod_name;
    }
    if (!signal.module_filename.empty()) {
      row.candidate.module_filename = signal.module_filename;
    }
    if (!signal.detail.empty() && row.candidate.explanation.empty()) {
      row.candidate.explanation = signal.detail;
    }

    auto& familyWeight = row.family_weight[signal.family_id];
    familyWeight = std::max(familyWeight, signal.weight);
  }

  std::wstring topFrameKey;
  std::uint32_t topFrameWeight = 0;
  std::wstring topObjectRefKey;
  std::uint32_t topObjectRefWeight = 0;
  std::wstring topStackKey;
  std::uint32_t topStackWeight = 0;
  for (const auto& signal : signals) {
    if (signal.family_id == kFamilyCrashLoggerFrame && signal.weight > topFrameWeight) {
      topFrameKey = signal.candidate_key;
      topFrameWeight = signal.weight;
    } else if (signal.family_id == kFamilyCrashLoggerObjectRef && signal.weight > topObjectRefWeight) {
      topObjectRefKey = signal.candidate_key;
      topObjectRefWeight = signal.weight;
    } else if (signal.family_id == kFamilyStack && signal.weight > topStackWeight) {
      topStackKey = signal.candidate_key;
      topStackWeight = signal.weight;
    }
  }

  if (!topFrameKey.empty() && !topStackKey.empty() &&
      topFrameKey != topStackKey &&
      topFrameWeight >= kFrameConflictWeightThreshold &&
      topStackWeight >= kStackConflictWeightThreshold) {
    if (auto it = rowsByKey.find(topFrameKey); it != rowsByKey.end() &&
        !HasActionableCrossValidation(it->second)) {
      it->second.candidate.has_conflict = true;
      it->second.candidate.conflicting_families = { kFamilyStack };
    }
    if (auto it = rowsByKey.find(topStackKey); it != rowsByKey.end() &&
        !HasActionableCrossValidation(it->second)) {
      it->second.candidate.has_conflict = true;
      it->second.candidate.conflicting_families = { kFamilyCrashLoggerFrame };
    }
  }

  if (!topObjectRefKey.empty() && !topStackKey.empty() &&
      topObjectRefKey != topStackKey &&
      topObjectRefWeight >= kObjectRefConflictWeightThreshold &&
      topStackWeight >= kStackConflictWeightThreshold) {
    const auto topStackIt = rowsByKey.find(topStackKey);
    const bool preserveObjectRefAsSecondaryClue =
      topStackIt != rowsByKey.end() &&
      HasFrameAndStackConsensus(topStackIt->second);
    if (auto it = rowsByKey.find(topObjectRefKey); it != rowsByKey.end() &&
        !HasActionableCrossValidation(it->second) &&
        !(preserveObjectRefAsSecondaryClue && IsObjectRefOnlyClue(it->second))) {
      it->second.candidate.has_conflict = true;
      it->second.candidate.conflicting_families.push_back(kFamilyStack);
    }
    if (auto it = rowsByKey.find(topStackKey); it != rowsByKey.end() &&
        !HasActionableCrossValidation(it->second)) {
      it->second.candidate.has_conflict = true;
      it->second.candidate.conflicting_families.push_back(kFamilyCrashLoggerObjectRef);
    }
  }

  if (!topFrameKey.empty() && !topObjectRefKey.empty() &&
      topFrameKey != topObjectRefKey &&
      topFrameWeight >= kFrameConflictWeightThreshold &&
      topObjectRefWeight >= kObjectRefConflictWeightThreshold) {
    const auto topFrameIt = rowsByKey.find(topFrameKey);
    const bool preserveObjectRefAsSecondaryClue =
      topFrameIt != rowsByKey.end() &&
      HasFrameAndStackConsensus(topFrameIt->second);
    if (auto it = rowsByKey.find(topFrameKey); it != rowsByKey.end() &&
        !HasActionableCrossValidation(it->second)) {
      it->second.candidate.has_conflict = true;
      it->second.candidate.conflicting_families.push_back(kFamilyCrashLoggerObjectRef);
    }
    if (auto it = rowsByKey.find(topObjectRefKey); it != rowsByKey.end() &&
        !HasActionableCrossValidation(it->second) &&
        !(preserveObjectRefAsSecondaryClue && IsObjectRefOnlyClue(it->second))) {
      it->second.candidate.has_conflict = true;
      it->second.candidate.conflicting_families.push_back(kFamilyCrashLoggerFrame);
    }
  }

  std::vector<ActionableCandidate> candidates;
  candidates.reserve(rowsByKey.size());
  for (auto& [key, row] : rowsByKey) {
    (void)key;
    RefreshCandidateFields(&row, language);
    if (IsStandaloneCandidateAllowed(row.candidate)) {
      candidates.push_back(std::move(row.candidate));
    }
  }

  std::sort(candidates.begin(), candidates.end(), [](const ActionableCandidate& a, const ActionableCandidate& b) {
    const int rankA = StatusRank(a);
    const int rankB = StatusRank(b);
    if (rankA != rankB) {
      return rankA > rankB;
    }
    if (a.score != b.score) {
      return a.score > b.score;
    }
    if (a.family_count != b.family_count) {
      return a.family_count > b.family_count;
    }
    return a.display_name < b.display_name;
  });

  return candidates;
}

}  // namespace skydiag::dump_tool
