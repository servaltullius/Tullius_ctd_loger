#include "CandidateConsensus.h"

#include <algorithm>
#include <cwctype>
#include <unordered_map>
#include <utility>

namespace skydiag::dump_tool {
namespace {

constexpr const char* kFamilyCrashLogger = "crash_logger_object_ref";
constexpr const char* kFamilyStack = "actionable_stack";
constexpr const char* kFamilyResource = "resource_provider";
constexpr const char* kFamilyHistory = "history_repeat";

struct CandidateRow
{
  ActionableCandidate candidate;
  std::unordered_map<std::string, std::uint32_t> family_weight;
};

bool RowHasFamily(const CandidateRow& row, std::string_view familyId)
{
  return row.family_weight.find(std::string(familyId)) != row.family_weight.end();
}

bool IsBoostOnlyFamily(std::string_view familyId)
{
  return familyId == kFamilyHistory;
}

bool HasFamily(const ActionableCandidate& candidate, std::string_view familyId)
{
  return std::find(candidate.supporting_families.begin(), candidate.supporting_families.end(), familyId) !=
         candidate.supporting_families.end();
}

bool HasStrongFamily(const ActionableCandidate& candidate)
{
  return HasFamily(candidate, kFamilyCrashLogger) || HasFamily(candidate, kFamilyStack);
}

bool HasActionableCrossValidation(const CandidateRow& row)
{
  return RowHasFamily(row, kFamilyCrashLogger) && RowHasFamily(row, kFamilyStack);
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
  if (!candidate.mod_name.empty()) {
    return candidate.mod_name;
  }
  if (!candidate.module_filename.empty()) {
    return candidate.module_filename;
  }
  return candidate.display_name;
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
  candidate.display_name = PickDisplayName(candidate);

  const bool hasCrashLogger = HasFamily(candidate, kFamilyCrashLogger);
  const bool hasStack = HasFamily(candidate, kFamilyStack);
  const bool hasResource = HasFamily(candidate, kFamilyResource);
  const bool conflict = candidate.has_conflict;
  const bool crossValidated = !conflict && hasCrashLogger && hasStack && candidate.family_count >= 2;

  if (conflict) {
    candidate.status_id = "conflicting";
    candidate.confidence_level = i18n::ConfidenceLevel::kMedium;
    candidate.cross_validated = false;
  } else if (crossValidated) {
    candidate.status_id = "cross_validated";
    candidate.confidence_level = i18n::ConfidenceLevel::kHigh;
    candidate.cross_validated = true;
  } else if (candidate.family_count >= 2 && candidate.score >= 7 && HasStrongFamily(candidate)) {
    candidate.status_id = "related";
    candidate.confidence_level = i18n::ConfidenceLevel::kMedium;
    candidate.cross_validated = false;
  } else if (hasCrashLogger) {
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

  std::wstring topCrashLoggerKey;
  std::uint32_t topCrashLoggerWeight = 0;
  std::wstring topStackKey;
  std::uint32_t topStackWeight = 0;
  for (const auto& signal : signals) {
    if (signal.family_id == kFamilyCrashLogger && signal.weight > topCrashLoggerWeight) {
      topCrashLoggerKey = signal.candidate_key;
      topCrashLoggerWeight = signal.weight;
    } else if (signal.family_id == kFamilyStack && signal.weight > topStackWeight) {
      topStackKey = signal.candidate_key;
      topStackWeight = signal.weight;
    }
  }

  if (!topCrashLoggerKey.empty() && !topStackKey.empty() && topCrashLoggerKey != topStackKey) {
    if (auto it = rowsByKey.find(topCrashLoggerKey); it != rowsByKey.end() &&
        !HasActionableCrossValidation(it->second)) {
      it->second.candidate.has_conflict = true;
      it->second.candidate.conflicting_families = { kFamilyStack };
    }
    if (auto it = rowsByKey.find(topStackKey); it != rowsByKey.end() &&
        !HasActionableCrossValidation(it->second)) {
      it->second.candidate.has_conflict = true;
      it->second.candidate.conflicting_families = { kFamilyCrashLogger };
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
