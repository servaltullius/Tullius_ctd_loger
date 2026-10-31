#pragma once

// SkyrimDiagSummary v2 schema checks shared by the golden-fixture test
// (output_snapshot_tests) and the writer-output test that runs
// BuildSummaryJson (summary_writer_runtime_tests).

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

namespace skydiag::tests::summary_schema {

inline void AssertHasKey(const nlohmann::json& j, const char* key, const char* context)
{
  if (!j.contains(key)) {
    std::cerr << "FAIL: Missing key \"" << key << "\" in " << context << "\n";
    assert(false);
  }
}

inline void AssertIsType(const nlohmann::json& j, const char* key, const char* expectedType, const char* context)
{
  AssertHasKey(j, key, context);
  const auto& val = j[key];
  bool ok = false;
  if (std::string(expectedType) == "object") ok = val.is_object();
  else if (std::string(expectedType) == "array") ok = val.is_array();
  else if (std::string(expectedType) == "string") ok = val.is_string();
  else if (std::string(expectedType) == "number") ok = val.is_number();
  else if (std::string(expectedType) == "boolean") ok = val.is_boolean();
  else if (std::string(expectedType) == "null_or_object") ok = val.is_null() || val.is_object();
  if (!ok) {
    std::cerr << "FAIL: Key \"" << key << "\" in " << context
              << " expected type " << expectedType << " but got " << val.type_name() << "\n";
    assert(false);
  }
}

inline void ValidateSummarySchemaV2(const nlohmann::json& j)
{
  // ── Top-level required fields ──
  AssertIsType(j, "schema", "object", "root");
  AssertIsType(j, "dump_path", "string", "root");
  AssertIsType(j, "pid", "number", "root");
  AssertIsType(j, "state_flags", "number", "root");
  AssertIsType(j, "summary_sentence", "string", "root");
  AssertIsType(j, "crash_bucket_key", "string", "root");
  AssertIsType(j, "crash_bucket_version", "number", "root");
  AssertIsType(j, "analysis", "object", "root");
  AssertIsType(j, "privacy", "object", "root");
  AssertIsType(j, "triage", "object", "root");
  AssertIsType(j, "exception", "object", "root");
  AssertIsType(j, "game_version", "string", "root");
  AssertIsType(j, "crash_logger", "object", "root");
  AssertIsType(j, "signature_match", "null_or_object", "root");
  AssertIsType(j, "graphics_environment", "object", "root");
  AssertIsType(j, "graphics_diagnosis", "null_or_object", "root");
  AssertIsType(j, "suspects", "array", "root");
  AssertIsType(j, "callstack", "object", "root");
  AssertIsType(j, "symbolization", "object", "root");
  AssertIsType(j, "resources", "array", "root");
  AssertIsType(j, "actionable_candidates", "array", "root");
  AssertIsType(j, "freeze_analysis", "object", "root");
  AssertIsType(j, "evidence", "array", "root");
  AssertIsType(j, "recommendations", "array", "root");
  AssertIsType(j, "first_chance_context", "object", "root");

  // ── schema block ──
  const auto& schema = j["schema"];
  assert(schema["name"].get<std::string>() == "SkyrimDiagSummary");
  assert(schema["version"].get<int>() == 2);

  // ── analysis sub-object ──
  const auto& analysis = j["analysis"];
  AssertIsType(analysis, "is_crash_like", "boolean", "analysis");
  AssertIsType(analysis, "is_hang_like", "boolean", "analysis");
  AssertIsType(analysis, "is_snapshot_like", "boolean", "analysis");
  AssertIsType(analysis, "is_manual_capture", "boolean", "analysis");

  // ── privacy sub-object ──
  const auto& privacy = j["privacy"];
  AssertIsType(privacy, "path_redaction_applied", "boolean", "privacy");
  AssertIsType(privacy, "online_symbol_source_allowed", "boolean", "privacy");
  AssertIsType(privacy, "online_symbol_source_used", "boolean", "privacy");

  // ── triage sub-object ──
  const auto& triage = j["triage"];
  AssertIsType(triage, "review_status", "string", "triage");
  AssertIsType(triage, "reviewed", "boolean", "triage");
  AssertIsType(triage, "verdict", "string", "triage");
  AssertIsType(triage, "actual_cause", "string", "triage");
  AssertIsType(triage, "ground_truth_cause", "string", "triage");
  AssertIsType(triage, "ground_truth_mod", "string", "triage");
  AssertIsType(triage, "signature_matched", "boolean", "triage");
  AssertIsType(triage, "reviewer", "string", "triage");
  AssertIsType(triage, "reviewed_at_utc", "string", "triage");
  AssertIsType(triage, "notes", "string", "triage");

  // ── exception sub-object ──
  const auto& exc = j["exception"];
  AssertIsType(exc, "code", "number", "exception");
  AssertIsType(exc, "thread_id", "number", "exception");
  AssertIsType(exc, "address", "number", "exception");
  AssertIsType(exc, "fault_module_offset", "number", "exception");
  AssertIsType(exc, "module_plus_offset", "string", "exception");
  AssertIsType(exc, "fault_module_unknown", "boolean", "exception");
  AssertIsType(exc, "module_path", "string", "exception");
  AssertIsType(exc, "inferred_mod_name", "string", "exception");

  // ── crash_logger sub-object ──
  const auto& cl = j["crash_logger"];
  AssertIsType(cl, "log_path", "string", "crash_logger");
  AssertIsType(cl, "version", "string", "crash_logger");
  AssertIsType(cl, "pairing", "object", "crash_logger");
  AssertIsType(cl["pairing"], "time_delta_ms", "number", "crash_logger.pairing");
  AssertIsType(cl["pairing"], "eligible_candidate_count", "number", "crash_logger.pairing");
  AssertIsType(cl["pairing"], "nearby_competitor_count", "number", "crash_logger.pairing");
  AssertIsType(cl["pairing"], "ambiguous", "boolean", "crash_logger.pairing");
  AssertIsType(cl["pairing"], "location_mismatch", "boolean", "crash_logger.pairing");
  AssertIsType(cl["pairing"], "confidence", "string", "crash_logger.pairing");
  AssertIsType(cl, "top_modules", "array", "crash_logger");
  AssertIsType(cl, "direct_fault_eligible", "boolean", "crash_logger");
  AssertIsType(cl, "first_actionable_probable_eligible", "boolean", "crash_logger");
  AssertIsType(cl, "probable_streak_eligible", "boolean", "crash_logger");
  if (cl.contains("object_refs")) {
    assert(cl["object_refs"].is_array());
    for (const auto& ref : cl["object_refs"]) {
      AssertIsType(ref, "esp_name", "string", "crash_logger.object_refs[]");
      AssertIsType(ref, "best_object_type", "string", "crash_logger.object_refs[]");
      AssertIsType(ref, "best_location", "string", "crash_logger.object_refs[]");
      AssertIsType(ref, "object_name", "string", "crash_logger.object_refs[]");
      AssertIsType(ref, "form_id", "string", "crash_logger.object_refs[]");
      AssertIsType(ref, "ref_count", "number", "crash_logger.object_refs[]");
      AssertIsType(ref, "relevance_score", "number", "crash_logger.object_refs[]");
    }
  }

  if (j["signature_match"].is_object()) {
    const auto& signature = j["signature_match"];
    AssertIsType(signature, "id", "string", "signature_match");
    AssertIsType(signature, "scope", "string", "signature_match");
    AssertIsType(signature, "mechanism", "string", "signature_match");
    AssertIsType(signature, "match_confidence", "string", "signature_match");
  }

  // ── graphics_environment ──
  const auto& gfx = j["graphics_environment"];
  AssertIsType(gfx, "enb_detected", "boolean", "graphics_environment");
  AssertIsType(gfx, "reshade_detected", "boolean", "graphics_environment");
  AssertIsType(gfx, "dxvk_detected", "boolean", "graphics_environment");
  AssertIsType(gfx, "injection_modules", "array", "graphics_environment");

  // ── suspects array items ──
  for (const auto& s : j["suspects"]) {
    AssertIsType(s, "confidence", "string", "suspects[]");
    AssertIsType(s, "module_filename", "string", "suspects[]");
    AssertIsType(s, "module_path", "string", "suspects[]");
    AssertIsType(s, "inferred_mod_name", "string", "suspects[]");
    AssertIsType(s, "score", "number", "suspects[]");
    AssertIsType(s, "reason", "string", "suspects[]");
  }

  // ── callstack ──
  const auto& cs = j["callstack"];
  AssertIsType(cs, "thread_id", "number", "callstack");
  AssertIsType(cs, "frames", "array", "callstack");

  // ── symbolization ──
  const auto& sym = j["symbolization"];
  AssertIsType(sym, "search_path", "string", "symbolization");
  AssertIsType(sym, "cache_path", "string", "symbolization");
  AssertIsType(sym, "dbghelp_path", "string", "symbolization");
  AssertIsType(sym, "dbghelp_version", "string", "symbolization");
  AssertIsType(sym, "msdia_available", "boolean", "symbolization");
  AssertIsType(sym, "runtime_degraded", "boolean", "symbolization");
  AssertIsType(sym, "total_frames", "number", "symbolization");
  AssertIsType(sym, "symbolized_frames", "number", "symbolization");
  AssertIsType(sym, "source_line_frames", "number", "symbolization");

  // ── evidence items ──
  for (const auto& e : j["evidence"]) {
    AssertIsType(e, "confidence", "string", "evidence[]");
    AssertIsType(e, "title", "string", "evidence[]");
    AssertIsType(e, "details", "string", "evidence[]");
  }

  // ── actionable candidates ──
  for (const auto& c : j["actionable_candidates"]) {
    AssertIsType(c, "status_id", "string", "actionable_candidates[]");
    AssertIsType(c, "confidence", "string", "actionable_candidates[]");
    AssertIsType(c, "display_name", "string", "actionable_candidates[]");
    AssertIsType(c, "primary_identifier", "string", "actionable_candidates[]");
    AssertIsType(c, "secondary_label", "string", "actionable_candidates[]");
    AssertIsType(c, "plugin_name", "string", "actionable_candidates[]");
    AssertIsType(c, "mod_name", "string", "actionable_candidates[]");
    AssertIsType(c, "module_filename", "string", "actionable_candidates[]");
    AssertIsType(c, "family_count", "number", "actionable_candidates[]");
    AssertIsType(c, "score", "number", "actionable_candidates[]");
    AssertIsType(c, "standalone_callstack", "boolean", "actionable_candidates[]");
    AssertIsType(c, "supporting_families", "array", "actionable_candidates[]");
    AssertIsType(c, "conflicting_families", "array", "actionable_candidates[]");
  }

  // ── freeze_analysis ──
  const auto& freeze = j["freeze_analysis"];
  AssertIsType(freeze, "state_id", "string", "freeze_analysis");
  AssertIsType(freeze, "confidence", "string", "freeze_analysis");
  AssertIsType(freeze, "support_quality", "string", "freeze_analysis");
  AssertIsType(freeze, "primary_reasons", "array", "freeze_analysis");
  AssertIsType(freeze, "related_candidates", "array", "freeze_analysis");
  AssertIsType(freeze, "blackbox_context", "object", "freeze_analysis");
  AssertIsType(freeze, "first_chance_context", "object", "freeze_analysis");
  AssertIsType(freeze, "wct_consensus", "object", "freeze_analysis");
  const auto& blackbox = freeze["blackbox_context"];
  AssertIsType(blackbox, "loading_window", "boolean", "freeze_analysis.blackbox_context");
  AssertIsType(blackbox, "recent_module_loads", "number", "freeze_analysis.blackbox_context");
  AssertIsType(blackbox, "recent_module_unloads", "number", "freeze_analysis.blackbox_context");
  AssertIsType(blackbox, "recent_thread_creates", "number", "freeze_analysis.blackbox_context");
  AssertIsType(blackbox, "recent_thread_exits", "number", "freeze_analysis.blackbox_context");
  AssertIsType(blackbox, "module_churn_score", "number", "freeze_analysis.blackbox_context");
  AssertIsType(blackbox, "thread_churn_score", "number", "freeze_analysis.blackbox_context");
  AssertIsType(blackbox, "recent_non_system_modules", "array", "freeze_analysis.blackbox_context");
  const auto& firstChance = freeze["first_chance_context"];
  AssertIsType(firstChance, "has_context", "boolean", "freeze_analysis.first_chance_context");
  AssertIsType(firstChance, "recent_count", "number", "freeze_analysis.first_chance_context");
  AssertIsType(firstChance, "unique_signature_count", "number", "freeze_analysis.first_chance_context");
  AssertIsType(firstChance, "loading_window_count", "number", "freeze_analysis.first_chance_context");
  AssertIsType(firstChance, "repeated_signature_count", "number", "freeze_analysis.first_chance_context");
  AssertIsType(firstChance, "recent_non_system_modules", "array", "freeze_analysis.first_chance_context");
  const auto& wctConsensus = freeze["wct_consensus"];
  AssertIsType(wctConsensus, "capture_passes", "number", "freeze_analysis.wct_consensus");
  AssertIsType(wctConsensus, "cycle_consensus", "boolean", "freeze_analysis.wct_consensus");
  AssertIsType(wctConsensus, "repeated_cycle_thread_count", "number", "freeze_analysis.wct_consensus");
  AssertIsType(wctConsensus, "consistent_loading_signal", "boolean", "freeze_analysis.wct_consensus");
  AssertIsType(wctConsensus, "longest_wait_tid_consensus", "boolean", "freeze_analysis.wct_consensus");

  // ── recommendations ──
  for (const auto& r : j["recommendations"]) {
    assert(r.is_string());
  }
}

}  // namespace skydiag::tests::summary_schema
