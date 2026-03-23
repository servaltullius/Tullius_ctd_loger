// output_snapshot_tests.cpp — E2E JSON schema contract & OutputWriter guard tests
//
// Validates:
//  1. Golden JSON fixture conforms to SkyrimDiagSummary v2 schema
//  2. OutputWriter.cpp source contains all required JSON field writes
//  3. OutputWriter produces both summary JSON and report text

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "SourceGuardTestUtils.h"

namespace {
using skydiag::tests::source_guard::ProjectRoot;
using skydiag::tests::source_guard::ReadAllText;
using skydiag::tests::source_guard::ReadProjectText;

// ── Schema validation ──────────────────────────────────────────────────

void AssertHasKey(const nlohmann::json& j, const char* key, const char* context)
{
  if (!j.contains(key)) {
    std::cerr << "FAIL: Missing key \"" << key << "\" in " << context << "\n";
    assert(false);
  }
}

void AssertIsType(const nlohmann::json& j, const char* key, const char* expectedType, const char* context)
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

nlohmann::json LoadGoldenJson()
{
  const auto root = ProjectRoot();
  const auto goldenPath = root / "tests" / "data" / "golden_summary_v2.json";
  const std::string text = ReadAllText(goldenPath);
  return nlohmann::json::parse(text);
}

void TestGoldenJsonSchemaV2(const nlohmann::json& j)
{

  // ── Top-level required fields ──
  AssertIsType(j, "schema", "object", "root");
  AssertIsType(j, "dump_path", "string", "root");
  AssertIsType(j, "pid", "number", "root");
  AssertIsType(j, "state_flags", "number", "root");
  AssertIsType(j, "summary_sentence", "string", "root");
  AssertIsType(j, "crash_bucket_key", "string", "root");
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
  AssertIsType(cl, "top_modules", "array", "crash_logger");
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

  // ── graphics_environment ──
  const auto& gfx = j["graphics_environment"];
  AssertIsType(gfx, "enb_detected", "boolean", "graphics_environment");
  AssertIsType(gfx, "reshade_detected", "boolean", "graphics_environment");
  AssertIsType(gfx, "dxvk_detected", "boolean", "graphics_environment");
  AssertIsType(gfx, "injection_modules", "array", "graphics_environment");

  // ── suspects array items ──
  assert(!j["suspects"].empty());
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

  // ── recommendations ──
  for (const auto& r : j["recommendations"]) {
    assert(r.is_string());
  }

  std::cout << "  [PASS] Golden JSON v2 schema validation\n";
}

// ── Value validation: check golden fixture data integrity ──

void TestGoldenJsonValues(const nlohmann::json& j)
{
  // Verify specific values to catch accidental golden file corruption
  assert(j["exception"]["code"].get<std::uint32_t>() == 0xC0000005u);
  assert(j["analysis"]["is_crash_like"].get<bool>() == true);
  assert(j["suspects"].size() == 2);
  assert(j["suspects"][0]["confidence"].get<std::string>() == "High");
  assert(j["suspects"][0]["module_filename"].get<std::string>() == "hdtSMP64.dll");
  assert(j["callstack"]["frames"].size() == 4);
  assert(j["evidence"].size() == 1);
  assert(j["recommendations"].size() == 2);
  assert(j["freeze_analysis"]["state_id"].is_string());
  assert(j["freeze_analysis"]["first_chance_context"].is_object());

  // CrashLogger object_refs with FormID
  assert(j["crash_logger"]["object_refs"].size() == 1);
  const auto& ref = j["crash_logger"]["object_refs"][0];
  assert(ref["esp_name"].get<std::string>() == "AE_StellarBlade_Doro.esp");
  assert(ref["form_id"].get<std::string>() == "0xFEAD081B");
  assert(ref["object_name"].get<std::string>() == "도로롱");

  std::cout << "  [PASS] Golden JSON value integrity\n";
}

// ── Source guard: OutputWriter.cpp must emit all schema fields ──

void TestOutputWriterEmitsAllFields()
{
  const std::string src = ReadProjectText("dump_tool/src/OutputWriter.cpp");

  // All top-level JSON keys that BuildSummaryJson must produce
  const std::vector<std::string> requiredKeys = {
    "\"schema\"",
    "\"dump_path\"",
    "\"pid\"",
    "\"state_flags\"",
    "\"summary_sentence\"",
    "\"crash_bucket_key\"",
    "\"analysis\"",
    "\"privacy\"",
    "\"triage\"",
    "\"exception\"",
    "\"game_version\"",
    "\"crash_logger\"",
    "\"signature_match\"",
    "\"graphics_environment\"",
    "\"suspects\"",
    "\"callstack\"",
    "\"symbolization\"",
    "\"resources\"",
    "\"actionable_candidates\"",
    "\"freeze_analysis\"",
    "\"first_chance_context\"",
    "\"evidence\"",
    "\"recommendations\"",
  };

  for (const auto& key : requiredKeys) {
    if (src.find(key) == std::string::npos) {
      std::cerr << "FAIL: OutputWriter.cpp missing JSON key " << key << "\n";
      assert(false);
    }
  }

  // Nested fields that must be emitted
  const std::vector<std::string> nestedKeys = {
    "\"is_crash_like\"",
    "\"is_hang_like\"",
    "\"is_snapshot_like\"",
    "\"is_manual_capture\"",
    "\"path_redaction_applied\"",
    "\"code\"",
    "\"thread_id\"",
    "\"address\"",
    "\"fault_module_offset\"",
    "\"module_plus_offset\"",
    "\"fault_module_unknown\"",
    "\"module_path\"",
    "\"inferred_mod_name\"",
    "\"log_path\"",
    "\"version\"",
    "\"top_modules\"",
    "\"object_refs\"",
    "\"esp_name\"",
    "\"form_id\"",
    "\"status_id\"",
    "\"display_name\"",
    "\"primary_identifier\"",
    "\"secondary_label\"",
    "\"plugin_name\"",
    "\"mod_name\"",
    "\"module_filename\"",
    "\"family_count\"",
    "\"supporting_families\"",
    "\"conflicting_families\"",
    "\"state_id\"",
    "\"support_quality\"",
    "\"primary_reasons\"",
    "\"related_candidates\"",
    "\"blackbox_context\"",
    "\"first_chance_context\"",
    "\"loading_window\"",
    "\"recent_module_loads\"",
    "\"recent_module_unloads\"",
    "\"recent_thread_creates\"",
    "\"recent_thread_exits\"",
    "\"module_churn_score\"",
    "\"thread_churn_score\"",
    "\"recent_non_system_modules\"",
    "\"has_context\"",
    "\"recent_count\"",
    "\"unique_signature_count\"",
    "\"loading_window_count\"",
    "\"repeated_signature_count\"",
    "\"enb_detected\"",
    "\"reshade_detected\"",
    "\"dxvk_detected\"",
    "\"injection_modules\"",
    "\"total_frames\"",
    "\"symbolized_frames\"",
    "\"source_line_frames\"",
    "\"dbghelp_path\"",
    "\"dbghelp_version\"",
    "\"msdia_available\"",
    "\"runtime_degraded\"",
    "\"recapture_evaluation\"",
    "\"triggered\"",
    "\"target_profile\"",
    "\"reasons\"",
    "\"escalation_level\"",
    "\"include_code_segments\"",
  };

  for (const auto& key : nestedKeys) {
    if (src.find(key) == std::string::npos) {
      std::cerr << "FAIL: OutputWriter.cpp missing nested key " << key << "\n";
      assert(false);
    }
  }

  std::cout << "  [PASS] OutputWriter emits all required JSON fields\n";
}

// ── Source guard: report text must have sections ──

void TestOutputWriterReportTextSections()
{
  const std::string src = ReadProjectText("dump_tool/src/OutputWriter.cpp");

  const std::vector<std::string> requiredSections = {
    "SkyrimDiag Report",
    "ExceptionCode:",
    "ExceptionAddress:",
    "ThreadId:",
    "Module+Offset:",
    "CaptureProfileBaseMode:",
    "CaptureProfileCodeSegments:",
    "CaptureProfileFullMemory:",
    "RecaptureTriggered:",
    "RecaptureTargetProfile:",
    "RecaptureReasons:",
    "RecaptureEscalationLevel:",
    "Evidence:",
    "Recommendations",
    "Callstack",
    "Suspects",
    "FreezeAnalysis:",
    "HasBlackbox:",
    "HasWCT:",
    "blackbox",
    "module churn",
    "first_chance_context",
    "repeated suspicious first-chance",
  };

  for (const auto& section : requiredSections) {
    if (src.find(section) == std::string::npos) {
      std::cerr << "FAIL: OutputWriter.cpp report text missing section \"" << section << "\"\n";
      assert(false);
    }
  }

  std::cout << "  [PASS] OutputWriter report text has all required sections\n";
}

void TestFirstChanceCandidateExplanationSourceGuards()
{
  const std::string evidenceSrc = ReadProjectText("dump_tool/src/EvidenceBuilderEvidence.cpp");
  const std::string recommendationSrc = ReadProjectText("dump_tool/src/EvidenceBuilderRecommendations.cpp");
  const std::string summarySrc = ReadProjectText("dump_tool/src/EvidenceBuilderSummary.cpp");

  assert(evidenceSrc.find("first_chance_context") != std::string::npos);
  assert(evidenceSrc.find("repeated suspicious first-chance") != std::string::npos);
  assert(recommendationSrc.find("first-chance") != std::string::npos);
  assert(summarySrc.find("first_chance_context") != std::string::npos);

  std::cout << "  [PASS] CTD first-chance candidate explanation guards\n";
}

void TestRecaptureEvaluationConsumptionSourceGuards()
{
  const std::string outputSrc = ReadProjectText("dump_tool/src/OutputWriter.cpp");
  const std::string evidenceSrc = ReadProjectText("dump_tool/src/EvidenceBuilderEvidence.cpp");
  const std::string recommendationSrc = ReadProjectText("dump_tool/src/EvidenceBuilderRecommendations.cpp");

  assert(outputSrc.find("RecaptureReasons:") != std::string::npos);
  assert(outputSrc.find("RecaptureEscalationLevel:") != std::string::npos);
  assert(outputSrc.find("\"recapture_evaluation\"") != std::string::npos);
  assert(evidenceSrc.find("Capture recapture context") != std::string::npos);
  assert(evidenceSrc.find("incident_recapture_target_profile") != std::string::npos);
  assert(recommendationSrc.find("[Recapture]") != std::string::npos);
  assert(recommendationSrc.find("freeze_snapshot_richer") != std::string::npos);

  std::cout << "  [PASS] Recapture evaluation consumption guards\n";
}

// ── Source guard: WriteOutputs writes both JSON and text files ──

void TestOutputWriterWritesBothFiles()
{
  const std::string src = ReadProjectText("dump_tool/src/OutputWriter.cpp");

  assert(src.find("_SkyrimDiagSummary.json") != std::string::npos);
  assert(src.find("_SkyrimDiagReport.txt") != std::string::npos);
  assert(src.find("BuildSummaryJson") != std::string::npos);
  assert(src.find("BuildReportText") != std::string::npos);

  std::cout << "  [PASS] OutputWriter produces summary JSON + report text\n";
}

}  // namespace

int main()
{
  std::cout << "output_snapshot_tests:\n";
  const auto golden = LoadGoldenJson();
  TestGoldenJsonSchemaV2(golden);
  TestGoldenJsonValues(golden);
  TestOutputWriterEmitsAllFields();
  TestOutputWriterReportTextSections();
  TestFirstChanceCandidateExplanationSourceGuards();
  TestRecaptureEvaluationConsumptionSourceGuards();
  TestOutputWriterWritesBothFiles();
  std::cout << "All output snapshot tests passed.\n";
  return 0;
}
