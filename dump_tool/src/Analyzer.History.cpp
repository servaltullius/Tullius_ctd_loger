#include "AnalyzerPipeline.h"

#include "CandidateConsensus.h"
#include "CrashHistory.h"
#include "OutputWriterInternals.h"
#include "ScopedHistoryFileLock.h"
#include "Utf.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace skydiag::dump_tool {

using skydiag::dump_tool::internal::output_writer::DefaultOutDirForDump;
using skydiag::dump_tool::internal::output_writer::FindIncidentManifestForDump;
using skydiag::dump_tool::internal::output_writer::TryLoadIncidentManifestJson;

std::filesystem::path ResolveCrashHistoryPath(
  const std::wstring& dumpPath,
  const std::wstring& outDir,
  const AnalyzeOptions& opt)
{
  std::filesystem::path historyDir;
  if (!opt.output_dir.empty()) {
    historyDir = opt.output_dir;
  } else if (!outDir.empty()) {
    historyDir = outDir;
  } else {
    historyDir = std::filesystem::path(dumpPath).parent_path();
  }

  if (historyDir.empty()) {
    return {};
  }

  return historyDir / "crash_history.json";
}

void LoadIncidentCaptureProfile(
  const std::wstring& dumpPath,
  const std::wstring& outDir,
  AnalysisResult& out)
{
  const auto dumpFs = std::filesystem::path(dumpPath);
  const auto outBase = !outDir.empty()
    ? std::filesystem::path(outDir)
    : DefaultOutDirForDump(dumpFs);
  const auto manifestPath = FindIncidentManifestForDump(outBase, dumpFs.stem().wstring());
  if (manifestPath.empty()) {
    return;
  }

  nlohmann::json incidentManifest;
  if (!TryLoadIncidentManifestJson(manifestPath, &incidentManifest) || !incidentManifest.is_object()) {
    return;
  }

  out.incident_capture_kind = incidentManifest.value("capture_kind", "");
  if (incidentManifest.contains("capture_profile") && incidentManifest["capture_profile"].is_object()) {
    const auto& capture_profile = incidentManifest["capture_profile"];
    out.incident_capture_profile_present = true;
    if (out.incident_capture_kind.empty()) {
      out.incident_capture_kind = capture_profile.value("capture_kind", "");
    }
    out.incident_capture_profile_base_mode = capture_profile.value("base_mode", "");
    out.incident_capture_profile_code_segments = capture_profile.value("include_code_segments", false);
    out.incident_capture_profile_process_thread_data = capture_profile.value("include_process_thread_data", false);
    out.incident_capture_profile_full_memory_info = capture_profile.value("include_full_memory_info", false);
    out.incident_capture_profile_module_headers = capture_profile.value("include_module_headers", false);
    out.incident_capture_profile_indirect_memory = capture_profile.value("include_indirect_memory", false);
    out.incident_capture_profile_ignore_inaccessible_memory =
      capture_profile.value("ignore_inaccessible_memory", false);
    out.incident_capture_profile_full_memory = capture_profile.value("include_full_memory", false);
  }
  if (incidentManifest.contains("recapture_evaluation") && incidentManifest["recapture_evaluation"].is_object()) {
    const auto& recapture_evaluation = incidentManifest["recapture_evaluation"];
    out.incident_recapture_evaluation_present = true;
    out.incident_recapture_triggered = recapture_evaluation.value("triggered", false);
    out.incident_recapture_kind = recapture_evaluation.value("kind", "");
    out.incident_recapture_target_profile = recapture_evaluation.value("target_profile", "");
    out.incident_recapture_escalation_level = recapture_evaluation.value("escalation_level", 0u);
    out.incident_recapture_reasons.clear();
    if (recapture_evaluation.contains("reasons") && recapture_evaluation["reasons"].is_array()) {
      for (const auto& reason : recapture_evaluation["reasons"]) {
        if (reason.is_string()) {
          out.incident_recapture_reasons.push_back(reason.get<std::string>());
        }
      }
    }
  }
}

namespace {

void AppendHistoryCandidateKey(
  std::wstring_view rawValue,
  std::unordered_set<std::string>* seen,
  std::vector<std::string>* outKeys)
{
  if (!seen || !outKeys || rawValue.empty()) {
    return;
  }

  const auto canonical = WideToUtf8(CanonicalCandidateKey(rawValue));
  if (!canonical.empty() && seen->insert(canonical).second) {
    outKeys->push_back(canonical);
  }
}

}  // namespace

std::vector<std::string> CollectHistoryCandidateKeys(const AnalysisResult& out)
{
  std::vector<std::string> keys;
  std::unordered_set<std::string> seen;

  if (!out.actionable_candidates.empty()) {
    const auto limit = std::min<std::size_t>(out.actionable_candidates.size(), 5u);
    for (std::size_t i = 0; i < limit; ++i) {
      const auto& candidate = out.actionable_candidates[i];
      AppendHistoryCandidateKey(candidate.plugin_name, &seen, &keys);
      AppendHistoryCandidateKey(candidate.mod_name, &seen, &keys);
      AppendHistoryCandidateKey(candidate.module_filename, &seen, &keys);
      AppendHistoryCandidateKey(candidate.display_name, &seen, &keys);
    }
  }

  if (keys.empty()) {
    for (const auto& ref : out.crash_logger_object_refs) {
      AppendHistoryCandidateKey(ref.esp_name, &seen, &keys);
    }
    for (const auto& suspect : out.suspects) {
      AppendHistoryCandidateKey(!suspect.inferred_mod_name.empty() ? suspect.inferred_mod_name : suspect.module_filename, &seen, &keys);
    }
  }

  return keys;
}

void LoadCrashHistoryContext(
  const std::filesystem::path& historyPath,
  const std::string& analysisTimestamp,
  AnalysisResult& out)
{
  if (historyPath.empty()) {
    return;
  }

  ScopedHistoryFileLock historyLock(historyPath);
  if (!historyLock.Acquire(/*timeoutMs=*/2000)) {
    out.diagnostics.push_back(L"[History] failed to acquire lock for crash_history.json");
    return;
  }

  CrashHistory history;
  history.LoadFromFile(historyPath);

  out.history_stats = history.GetModuleStats(20);

  const auto bucketKey = WideToUtf8(out.crash_bucket_key);
  for (const auto& stats : history.GetBucketCandidateStats(bucketKey)) {
    if (stats.count == 0 || stats.candidate_key.empty()) {
      continue;
    }
    out.bucket_candidate_repeats.push_back({ stats.candidate_key, stats.count });
  }

  const auto bucketStats = history.GetBucketStats(bucketKey);
  if (bucketStats.count > 0) {
    out.history_correlation.count = bucketStats.count + 1;
    out.history_correlation.first_seen = bucketStats.first_seen.empty() ? analysisTimestamp : bucketStats.first_seen;
    out.history_correlation.last_seen = analysisTimestamp;
  }
}

void AppendCrashHistoryEntry(
  const std::filesystem::path& historyPath,
  const std::wstring& dumpPath,
  const std::string& analysisTimestamp,
  AnalysisResult& out)
{
  if (historyPath.empty()) {
    return;
  }

  ScopedHistoryFileLock historyLock(historyPath);
  if (!historyLock.Acquire(/*timeoutMs=*/2000)) {
    out.diagnostics.push_back(L"[History] failed to acquire lock for crash_history.json");
    return;
  }

  CrashHistory history;
  history.LoadFromFile(historyPath);

  CrashHistoryEntry entry{};
  entry.timestamp_utc = analysisTimestamp;
  entry.dump_file = WideToUtf8(std::filesystem::path(dumpPath).filename().wstring());
  entry.bucket_key = WideToUtf8(out.crash_bucket_key);
  if (!out.suspects.empty()) {
    entry.top_suspect = WideToUtf8(out.suspects[0].module_filename);
    entry.confidence = WideToUtf8(out.suspects[0].confidence);
    for (const auto& s : out.suspects) {
      if (!s.module_filename.empty()) {
        entry.all_suspects.push_back(WideToUtf8(s.module_filename));
      }
    }
  }
  if (out.signature_match) {
    entry.signature_id = out.signature_match->id;
  }
  entry.candidate_keys = CollectHistoryCandidateKeys(out);

  history.AddEntry(std::move(entry));
  if (!history.SaveToFile(historyPath)) {
    out.diagnostics.push_back(L"[History] failed to save crash_history.json");
  }
}

}  // namespace skydiag::dump_tool
