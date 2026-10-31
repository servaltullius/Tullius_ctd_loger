#include "AnalyzerPipeline.h"

#include "CandidateConsensus.h"
#include "CrashHistory.h"
#include "CrashLoggerParseCore.h"
#include "JsonValue.h"
#include "MainThreadWait.h"
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

  out.incident_capture_kind = JsonValueOr(incidentManifest, "capture_kind", std::string{});
  if (incidentManifest.contains("capture_profile") && incidentManifest["capture_profile"].is_object()) {
    const auto& capture_profile = incidentManifest["capture_profile"];
    out.incident_capture_profile_present = true;
    if (out.incident_capture_kind.empty()) {
      out.incident_capture_kind = JsonValueOr(capture_profile, "capture_kind", std::string{});
    }
    out.incident_capture_profile_base_mode = JsonValueOr(capture_profile, "base_mode", std::string{});
    out.incident_capture_profile_code_segments = JsonValueOr(capture_profile, "include_code_segments", false);
    out.incident_capture_profile_process_thread_data = JsonValueOr(capture_profile, "include_process_thread_data", false);
    out.incident_capture_profile_full_memory_info = JsonValueOr(capture_profile, "include_full_memory_info", false);
    out.incident_capture_profile_module_headers = JsonValueOr(capture_profile, "include_module_headers", false);
    out.incident_capture_profile_indirect_memory = JsonValueOr(capture_profile, "include_indirect_memory", false);
    out.incident_capture_profile_ignore_inaccessible_memory =
      JsonValueOr(capture_profile, "ignore_inaccessible_memory", false);
    out.incident_capture_profile_full_memory = JsonValueOr(capture_profile, "include_full_memory", false);
  }
  if (incidentManifest.contains("recapture_evaluation") && incidentManifest["recapture_evaluation"].is_object()) {
    const auto& recapture_evaluation = incidentManifest["recapture_evaluation"];
    out.incident_recapture_evaluation_present = true;
    out.incident_recapture_triggered = JsonValueOr(recapture_evaluation, "triggered", false);
    out.incident_recapture_kind = JsonValueOr(recapture_evaluation, "kind", std::string{});
    out.incident_recapture_target_profile = JsonValueOr(recapture_evaluation, "target_profile", std::string{});
    out.incident_recapture_escalation_level = JsonValueOr(recapture_evaluation, "escalation_level", 0u);
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

std::string CrashHistoryIdentityKey(const AnalysisResult& out)
{
  if (!out.dump_identity.IsValid()) {
    return {};
  }
  return out.dump_identity.sha256 + "." + out.dump_identity.StorageMetadataKey();
}

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

  // A lock-owner freeze is bucketed by the owner thread or the deadlock; the
  // main thread's bystanders would vote for themselves in every repeat.
  // So is an engine loop on a NaN value, by the loop: its stack plugins are
  // the call path. So is a deadlock on the engine's own locks.
  if (IsLockOwnerFreezeState(out.freeze_analysis.state_id) || out.engine_spin.nan_detected ||
      out.engine_lock_deadlock.detected) {
    return keys;
  }

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

  // A freeze whose main thread waited in a dialog, the engine or the driver
  // has bystanders on its stack (ADR-0006, ADR-0009). Recording them would
  // make every repeat of the same freeze vote for them through the bucket.
  const bool stackIsBystander = out.modal_dialog_wait.detected ||
    (IsBystanderWait(out.main_thread_wait) && out.suspects_from_main_thread);
  if (keys.empty()) {
    // Nor the refs of a log that may be another crash's (FirstCulpritObjectRef).
    for (const auto& ref : out.crash_logger_object_refs) {
      if (out.crash_logger_pairing_ambiguous) {
        break;
      }
      if (crashlogger_core::IsLocationObjectTypeAscii(WideToUtf8(ref.best_object_type))) {
        continue;  // where the player was, not a candidate
      }
      AppendHistoryCandidateKey(ref.esp_name, &seen, &keys);
    }
    for (const auto& suspect : out.suspects) {
      if (stackIsBystander) {
        // Except the plugin the main thread was waiting on (it leads).
        if (HasBlockedGameThread(out.main_thread_wait)) {
          AppendHistoryCandidateKey(!suspect.inferred_mod_name.empty() ? suspect.inferred_mod_name : suspect.module_filename, &seen, &keys);
        }
        break;
      }
      AppendHistoryCandidateKey(!suspect.inferred_mod_name.empty() ? suspect.inferred_mod_name : suspect.module_filename, &seen, &keys);
    }
  }

  return keys;
}

void LoadCrashHistoryContext(
  const std::filesystem::path& historyPath,
  const std::wstring& dumpPath,
  const std::string& analysisTimestamp,
  AnalysisResult& out)
{
  // Read again when a lock-owner freeze changes the bucket key.
  out.history_stats.clear();
  out.bucket_candidate_repeats.clear();
  out.history_correlation = {};
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
  history.RemoveEntriesForDumpFile(
    WideToUtf8(std::filesystem::path(dumpPath).filename().wstring()),
    CrashHistoryIdentityKey(out));

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
  entry.dump_identity_key = CrashHistoryIdentityKey(out);
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
