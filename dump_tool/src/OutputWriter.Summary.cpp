#include "OutputWriterPipeline.h"

#include "OutputWriterInternals.h"
#include "Utf.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace skydiag::dump_tool {

using skydiag::dump_tool::internal::output_writer::DefaultOutDirForDump;
using skydiag::dump_tool::internal::output_writer::FindIncidentManifestForDump;
using skydiag::dump_tool::internal::output_writer::IsUnknownModuleField;
using skydiag::dump_tool::internal::output_writer::LoadExistingSummaryTriage;
using skydiag::dump_tool::internal::output_writer::MaybeRedactPath;
using skydiag::dump_tool::internal::output_writer::ReplaceAll;
using skydiag::dump_tool::internal::output_writer::TryLoadIncidentManifestJson;

nlohmann::json BuildSummaryJson(
  const AnalysisResult& r,
  const std::filesystem::path& outBase,
  const std::wstring& stem,
  bool redactPaths)
{
  nlohmann::json summary = nlohmann::json::object();
  constexpr std::uint32_t kSummarySchemaVersion = 2;
  summary["schema"] = {
    { "name", "SkyrimDiagSummary" },
    { "version", kSummarySchemaVersion },
  };
  summary["dump_path"] = WideToUtf8(MaybeRedactPath(r.dump_path, redactPaths));
  summary["pid"] = r.pid;
  summary["state_flags"] = r.state_flags;
  summary["summary_sentence"] = WideToUtf8(r.summary_sentence);
  summary["crash_bucket_key"] = WideToUtf8(r.crash_bucket_key);
  summary["analysis"] = {
    { "is_crash_like", r.is_crash_like },
    { "is_hang_like", r.is_hang_like },
    { "is_snapshot_like", r.is_snapshot_like },
    { "is_manual_capture", r.is_manual_capture },
  };
  summary["privacy"] = {
    { "path_redaction_applied", redactPaths },
    { "online_symbol_source_allowed", r.online_symbol_source_allowed },
    { "online_symbol_source_used", r.online_symbol_source_used },
  };
  const auto incidentManifestPath = FindIncidentManifestForDump(outBase, stem);
  nlohmann::json incidentManifest;
  if (!incidentManifestPath.empty() && TryLoadIncidentManifestJson(incidentManifestPath, &incidentManifest)) {
    nlohmann::json artifacts = nlohmann::json::object();
    if (incidentManifest.contains("artifacts") && incidentManifest["artifacts"].is_object()) {
      artifacts = incidentManifest["artifacts"];
    }
    if (!artifacts.contains("etw")) {
      artifacts["etw"] = "";
    }

    nlohmann::json privacy = nlohmann::json::object();
    if (incidentManifest.contains("privacy") && incidentManifest["privacy"].is_object()) {
      privacy = incidentManifest["privacy"];
    }
    nlohmann::json captureProfile = nlohmann::json::object();
    if (incidentManifest.contains("capture_profile") && incidentManifest["capture_profile"].is_object()) {
      captureProfile = incidentManifest["capture_profile"];
    }
    if (!captureProfile.contains("capture_kind")) {
      captureProfile["capture_kind"] = "";
    }
    if (!captureProfile.contains("base_mode")) {
      captureProfile["base_mode"] = "";
    }
    if (!captureProfile.contains("include_code_segments")) {
      captureProfile["include_code_segments"] = false;
    }
    if (!captureProfile.contains("include_full_memory")) {
      captureProfile["include_full_memory"] = false;
    }

    nlohmann::json recaptureEvaluation = nlohmann::json::object();
    if (incidentManifest.contains("recapture_evaluation") && incidentManifest["recapture_evaluation"].is_object()) {
      recaptureEvaluation = incidentManifest["recapture_evaluation"];
    }
    if (!recaptureEvaluation.contains("triggered")) {
      recaptureEvaluation["triggered"] = false;
    }
    if (!recaptureEvaluation.contains("target_profile")) {
      recaptureEvaluation["target_profile"] = "none";
    }
    if (!recaptureEvaluation.contains("reasons")) {
      recaptureEvaluation["reasons"] = nlohmann::json::array();
    }
    if (!recaptureEvaluation.contains("escalation_level")) {
      recaptureEvaluation["escalation_level"] = 0;
    }

    summary["incident"] = {
      { "incident_id", incidentManifest.value("incident_id", "") },
      { "capture_kind", incidentManifest.value("capture_kind", "") },
      { "artifacts", std::move(artifacts) },
      { "capture_profile", std::move(captureProfile) },
      { "recapture_evaluation", std::move(recaptureEvaluation) },
      { "manifest_path", WideToUtf8(MaybeRedactPath(incidentManifestPath.wstring(), redactPaths)) },
      { "privacy", std::move(privacy) },
    };
  }
  const auto summaryPath = outBase / (stem + L"_SkyrimDiagSummary.json");
  nlohmann::json triage;
  LoadExistingSummaryTriage(summaryPath, &triage);
  summary["triage"] = std::move(triage);
  summary["triage"]["signature_matched"] = r.signature_match.has_value();
  if (!summary["triage"].contains("reviewed")) {
    summary["triage"]["reviewed"] = false;
  }
  if (!summary["triage"].contains("verdict")) {
    summary["triage"]["verdict"] = "";
  }
  if (!summary["triage"].contains("actual_cause")) {
    summary["triage"]["actual_cause"] = "";
  }

  summary["exception"] = nlohmann::json::object();
  summary["exception"]["code"] = r.exc_code;
  summary["exception"]["thread_id"] = r.exc_tid;
  summary["exception"]["address"] = r.exc_addr;
  summary["exception"]["fault_module_offset"] = r.fault_module_offset;
  summary["exception"]["module_plus_offset"] = WideToUtf8(r.fault_module_plus_offset);
  summary["exception"]["fault_module_unknown"] = IsUnknownModuleField(r.fault_module_plus_offset);
  summary["exception"]["module_path"] = WideToUtf8(MaybeRedactPath(r.fault_module_path, redactPaths));
  summary["exception"]["inferred_mod_name"] = WideToUtf8(r.inferred_mod_name);
  summary["game_version"] = r.game_version;

  summary["crash_logger"] = nlohmann::json::object();
  summary["crash_logger"]["log_path"] = WideToUtf8(MaybeRedactPath(r.crash_logger_log_path, redactPaths));
  summary["crash_logger"]["version"] = WideToUtf8(r.crash_logger_version);
  summary["crash_logger"]["top_modules"] = nlohmann::json::array();
  for (const auto& m : r.crash_logger_top_modules) {
    summary["crash_logger"]["top_modules"].push_back(WideToUtf8(m));
  }
  summary["crash_logger"]["direct_fault_module"] = WideToUtf8(r.crash_logger_direct_fault_module);
  summary["crash_logger"]["first_actionable_probable_module"] = WideToUtf8(r.crash_logger_first_actionable_probable_module);
  summary["crash_logger"]["probable_streak_module"] = WideToUtf8(r.crash_logger_probable_streak_module);
  summary["crash_logger"]["probable_streak_length"] = r.crash_logger_probable_streak_length;
  summary["crash_logger"]["frame_signal_strength"] = r.crash_logger_frame_signal_strength;
  if (!r.crash_logger_cpp_exception_type.empty() ||
      !r.crash_logger_cpp_exception_info.empty() ||
      !r.crash_logger_cpp_exception_throw_location.empty() ||
      !r.crash_logger_cpp_exception_module.empty()) {
    summary["crash_logger"]["cpp_exception"] = nlohmann::json::object();
    summary["crash_logger"]["cpp_exception"]["type"] = WideToUtf8(r.crash_logger_cpp_exception_type);
    summary["crash_logger"]["cpp_exception"]["info"] = WideToUtf8(r.crash_logger_cpp_exception_info);
    summary["crash_logger"]["cpp_exception"]["throw_location"] = WideToUtf8(r.crash_logger_cpp_exception_throw_location);
    summary["crash_logger"]["cpp_exception"]["module"] = WideToUtf8(r.crash_logger_cpp_exception_module);
  }

  if (!r.crash_logger_object_refs.empty()) {
    summary["crash_logger"]["object_refs"] = nlohmann::json::array();
    for (const auto& ref : r.crash_logger_object_refs) {
      summary["crash_logger"]["object_refs"].push_back({
        { "esp_name", WideToUtf8(ref.esp_name) },
        { "best_object_type", WideToUtf8(ref.best_object_type) },
        { "best_location", WideToUtf8(ref.best_location) },
        { "object_name", WideToUtf8(ref.object_name) },
        { "form_id", WideToUtf8(ref.form_id) },
        { "ref_count", ref.ref_count },
        { "relevance_score", ref.relevance_score },
      });
    }
  }

  if (r.signature_match.has_value()) {
    summary["signature_match"] = {
      { "id", r.signature_match->id },
      { "cause", WideToUtf8(r.signature_match->cause) },
      { "confidence", WideToUtf8(r.signature_match->confidence) },
    };
  } else {
    summary["signature_match"] = nullptr;
  }

  summary["graphics_environment"] = {
    { "enb_detected", r.graphics_env.enb_detected },
    { "reshade_detected", r.graphics_env.reshade_detected },
    { "dxvk_detected", r.graphics_env.dxvk_detected },
  };
  summary["graphics_environment"]["injection_modules"] = nlohmann::json::array();
  for (const auto& mod : r.graphics_env.injection_modules) {
    summary["graphics_environment"]["injection_modules"].push_back(WideToUtf8(mod));
  }
  if (r.graphics_diag.has_value()) {
    summary["graphics_diagnosis"] = {
      { "rule_id", r.graphics_diag->rule_id },
      { "cause", WideToUtf8(r.graphics_diag->cause) },
      { "confidence", WideToUtf8(r.graphics_diag->confidence) },
    };
  } else {
    summary["graphics_diagnosis"] = nullptr;
  }

  if (r.has_plugin_scan) {
    auto parsedPluginScan = nlohmann::json::parse(r.plugin_scan_json_utf8, nullptr, false);
    if (parsedPluginScan.is_discarded()) {
      summary["plugin_scan"] = nullptr;
      summary["plugin_scan_raw"] = r.plugin_scan_json_utf8;
    } else {
      summary["plugin_scan"] = std::move(parsedPluginScan);
    }

    if (!r.missing_masters.empty()) {
      auto mm = nlohmann::json::array();
      for (const auto& m : r.missing_masters) {
        mm.push_back(WideToUtf8(m));
      }
      summary["missing_masters"] = std::move(mm);
    } else {
      summary["missing_masters"] = nlohmann::json::array();
    }
    summary["needs_bees"] = r.needs_bees && r.is_crash_like;

    if (!r.plugin_diagnostics.empty()) {
      auto diags = nlohmann::json::array();
      for (const auto& d : r.plugin_diagnostics) {
        diags.push_back({
          { "rule_id", d.rule_id },
          { "cause", WideToUtf8(d.cause) },
          { "confidence", WideToUtf8(d.confidence) },
        });
      }
      summary["plugin_diagnostics"] = std::move(diags);
    }
  }

  if (!r.resolved_functions.empty()) {
    nlohmann::json funcs = nlohmann::json::object();
    for (const auto& [offset, name] : r.resolved_functions) {
      char key[32]{};
      std::snprintf(key, sizeof(key), "0x%llX", static_cast<unsigned long long>(offset));
      funcs[key] = name;
    }
    summary["resolved_functions"] = std::move(funcs);
  }

  summary["suspects"] = nlohmann::json::array();
  for (const auto& s : r.suspects) {
    summary["suspects"].push_back({
      { "confidence", WideToUtf8(s.confidence) },
      { "module_filename", WideToUtf8(s.module_filename) },
      { "module_path", WideToUtf8(MaybeRedactPath(s.module_path, redactPaths)) },
      { "inferred_mod_name", WideToUtf8(s.inferred_mod_name) },
      { "score", s.score },
      { "reason", WideToUtf8(s.reason) },
    });
  }

  summary["callstack"] = nlohmann::json::object();
  summary["callstack"]["thread_id"] = r.stackwalk_primary_tid;
  summary["callstack"]["frames"] = nlohmann::json::array();
  for (const auto& f : r.stackwalk_primary_frames) {
    summary["callstack"]["frames"].push_back(WideToUtf8(f));
  }
  const std::wstring redactedSymbolCachePath = MaybeRedactPath(r.symbol_cache_path, redactPaths);
  const std::wstring redactedSymbolSearchPath = ReplaceAll(
    std::wstring(r.symbol_search_path),
    std::wstring_view(r.symbol_cache_path),
    std::wstring_view(redactedSymbolCachePath));
  const std::wstring redactedDbghelpPath = MaybeRedactPath(r.dbghelp_path, redactPaths);
  const std::wstring redactedMsdiaPath = MaybeRedactPath(r.msdia_path, redactPaths);
  summary["symbolization"] = {
    { "search_path", WideToUtf8(redactedSymbolSearchPath) },
    { "cache_path", WideToUtf8(redactedSymbolCachePath) },
    { "dbghelp_path", WideToUtf8(redactedDbghelpPath) },
    { "dbghelp_version", WideToUtf8(r.dbghelp_version) },
    { "msdia_path", WideToUtf8(redactedMsdiaPath) },
    { "msdia_available", r.msdia_available },
    { "cache_ready", r.symbol_cache_ready },
    { "runtime_degraded", r.symbol_runtime_degraded },
    { "total_frames", r.stackwalk_total_frames },
    { "symbolized_frames", r.stackwalk_symbolized_frames },
    { "source_line_frames", r.stackwalk_source_line_frames },
    { "online_symbol_source_allowed", r.online_symbol_source_allowed },
    { "online_symbol_source_used", r.online_symbol_source_used },
  };

  summary["resources"] = nlohmann::json::array();
  for (const auto& rr : r.resources) {
    nlohmann::json providers = nlohmann::json::array();
    for (const auto& p : rr.providers) {
      providers.push_back(WideToUtf8(p));
    }
    summary["resources"].push_back({
      { "t_ms", rr.t_ms },
      { "tid", rr.tid },
      { "kind", WideToUtf8(rr.kind) },
      { "path", WideToUtf8(MaybeRedactPath(rr.path, redactPaths)) },
      { "providers", std::move(providers) },
      { "is_conflict", rr.is_conflict },
    });
  }

  summary["actionable_candidates"] = nlohmann::json::array();
  for (const auto& c : r.actionable_candidates) {
    nlohmann::json supportingFamilies = nlohmann::json::array();
    for (const auto& family : c.supporting_families) {
      supportingFamilies.push_back(family);
    }
    nlohmann::json conflictingFamilies = nlohmann::json::array();
    for (const auto& family : c.conflicting_families) {
      conflictingFamilies.push_back(family);
    }
    summary["actionable_candidates"].push_back({
      { "status_id", c.status_id },
      { "confidence", WideToUtf8(c.confidence) },
      { "display_name", WideToUtf8(c.display_name) },
      { "primary_identifier", WideToUtf8(c.primary_identifier) },
      { "secondary_label", WideToUtf8(c.secondary_label) },
      { "plugin_name", WideToUtf8(c.plugin_name) },
      { "mod_name", WideToUtf8(c.mod_name) },
      { "module_filename", WideToUtf8(c.module_filename) },
      { "explanation", WideToUtf8(c.explanation) },
      { "family_count", c.family_count },
      { "score", c.score },
      { "cross_validated", c.cross_validated },
      { "has_conflict", c.has_conflict },
      { "supporting_families", std::move(supportingFamilies) },
      { "conflicting_families", std::move(conflictingFamilies) },
    });
  }

  summary["freeze_analysis"] = nlohmann::json::object();
  summary["freeze_analysis"]["state_id"] = r.freeze_analysis.state_id;
  summary["freeze_analysis"]["confidence"] = WideToUtf8(r.freeze_analysis.confidence);
  summary["freeze_analysis"]["support_quality"] = r.freeze_analysis.support_quality;
  summary["freeze_analysis"]["primary_reasons"] = nlohmann::json::array();
  for (const auto& reason : r.freeze_analysis.primary_reasons) {
    summary["freeze_analysis"]["primary_reasons"].push_back(WideToUtf8(reason));
  }
  summary["freeze_analysis"]["related_candidates"] = nlohmann::json::array();
  for (const auto& candidate : r.freeze_analysis.related_candidates) {
    summary["freeze_analysis"]["related_candidates"].push_back({
      { "confidence", WideToUtf8(candidate.confidence) },
      { "display_name", WideToUtf8(candidate.display_name) },
    });
  }
  summary["freeze_analysis"]["blackbox_context"] = {
    { "loading_window", r.freeze_analysis.blackbox_context.loading_window },
    { "recent_module_loads", r.freeze_analysis.blackbox_context.recent_module_loads },
    { "recent_module_unloads", r.freeze_analysis.blackbox_context.recent_module_unloads },
    { "recent_thread_creates", r.freeze_analysis.blackbox_context.recent_thread_creates },
    { "recent_thread_exits", r.freeze_analysis.blackbox_context.recent_thread_exits },
    { "module_churn_score", r.freeze_analysis.blackbox_context.module_churn_score },
    { "thread_churn_score", r.freeze_analysis.blackbox_context.thread_churn_score },
    { "recent_non_system_modules", nlohmann::json::array() },
  };
  for (const auto& moduleName : r.freeze_analysis.blackbox_context.recent_non_system_modules) {
    summary["freeze_analysis"]["blackbox_context"]["recent_non_system_modules"].push_back(WideToUtf8(moduleName));
  }
  summary["freeze_analysis"]["first_chance_context"] = {
    { "has_context", r.freeze_analysis.first_chance_context.has_context },
    { "recent_count", r.freeze_analysis.first_chance_context.recent_count },
    { "unique_signature_count", r.freeze_analysis.first_chance_context.unique_signature_count },
    { "loading_window_count", r.freeze_analysis.first_chance_context.loading_window_count },
    { "repeated_signature_count", r.freeze_analysis.first_chance_context.repeated_signature_count },
    { "recent_non_system_modules", nlohmann::json::array() },
  };
  for (const auto& moduleName : r.freeze_analysis.first_chance_context.recent_non_system_modules) {
    summary["freeze_analysis"]["first_chance_context"]["recent_non_system_modules"].push_back(WideToUtf8(moduleName));
  }

  summary["first_chance_context"] = {
    { "has_context", r.first_chance_summary.has_context },
    { "recent_count", r.first_chance_summary.recent_count },
    { "unique_signature_count", r.first_chance_summary.unique_signature_count },
    { "loading_window_count", r.first_chance_summary.loading_window_count },
    { "repeated_signature_count", r.first_chance_summary.repeated_signature_count },
    { "recent_non_system_modules", nlohmann::json::array() },
  };
  for (const auto& moduleName : r.first_chance_summary.recent_non_system_modules) {
    summary["first_chance_context"]["recent_non_system_modules"].push_back(WideToUtf8(moduleName));
  }

  summary["evidence"] = nlohmann::json::array();
  for (const auto& e : r.evidence) {
    summary["evidence"].push_back({
      { "confidence", WideToUtf8(e.confidence) },
      { "title", WideToUtf8(e.title) },
      { "details", WideToUtf8(e.details) },
    });
  }
  summary["recommendations"] = nlohmann::json::array();
  for (const auto& s : r.recommendations) {
    summary["recommendations"].push_back(WideToUtf8(s));
  }

  if (!r.history_stats.empty()) {
    auto stats = nlohmann::json::array();
    for (const auto& ms : r.history_stats) {
      stats.push_back({
        { "module", ms.module_name },
        { "total_appearances", ms.total_appearances },
        { "as_top_suspect", ms.as_top_suspect },
        { "total_crashes", ms.total_crashes },
      });
    }
    summary["crash_history_stats"] = std::move(stats);
  }

  if (r.history_correlation.count > 1) {
    summary["history_correlation"] = {
      { "bucket_key", WideToUtf8(r.crash_bucket_key) },
      { "count", r.history_correlation.count },
      { "first_seen", r.history_correlation.first_seen },
      { "last_seen", r.history_correlation.last_seen },
    };
  }

  if (!r.troubleshooting_steps.empty()) {
    auto steps = nlohmann::json::array();
    for (const auto& step : r.troubleshooting_steps) {
      steps.push_back(WideToUtf8(step));
    }
    summary["troubleshooting_steps"] = {
      { "title", WideToUtf8(r.troubleshooting_title) },
      { "steps", std::move(steps) },
    };
  }

  if (!r.diagnostics.empty()) {
    auto diags = nlohmann::json::array();
    for (const auto& d : r.diagnostics) {
      diags.push_back(WideToUtf8(d));
    }
    summary["diagnostics"] = std::move(diags);
  }

  return summary;
}

}  // namespace skydiag::dump_tool
