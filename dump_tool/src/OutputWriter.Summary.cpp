#include "OutputWriterPipeline.h"

#include "DumpObjects.h"
#include "MainThreadWait.h"
#include "MemorySnapshot.h"
#include "NextAction.h"
#include "OutputWriterInternals.h"
#include "RecommendationClass.h"
#include "Utf.h"
#include "WctTypes.h"

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

using skydiag::dump_tool::internal::output_writer::DumpIdentityJson;
using skydiag::dump_tool::internal::output_writer::FindIncidentManifestForDump;
using skydiag::dump_tool::internal::output_writer::IsUnknownModuleField;
using skydiag::dump_tool::internal::output_writer::LoadExistingSummaryTriage;
using skydiag::dump_tool::internal::output_writer::MaybeRedactPath;
using skydiag::dump_tool::internal::output_writer::RedactAbsolutePathStrings;
using skydiag::dump_tool::internal::output_writer::RedactSymbolSearchPath;
using skydiag::dump_tool::internal::output_writer::TruncateUtf8;
using skydiag::dump_tool::internal::output_writer::TryLoadIncidentManifestJson;

namespace {

// ADR-0010: one object read from the dump, as dump_objects[] and
// freeze_analysis.thread_objects[].objects[] carry it.
nlohmann::json DumpObjectJson(const DumpObject& object)
{
  nlohmann::json files = nlohmann::json::array();
  for (const auto& file : object.source_files) {
    files.push_back(WideToUtf8(file));
  }
  char address[32]{};
  std::snprintf(address, sizeof(address), "0x%llX", static_cast<unsigned long long>(object.address));
  nlohmann::json entry = {
    { "location", WideToUtf8(object.location) },
    { "address", address },
    { "type", WideToUtf8(object.type_name) },
    { "module", WideToUtf8(object.module) },
    { "is_form", object.is_form },
    { "form_id", object.form_id },
    { "form_type", object.form_type },
    { "source_files", std::move(files) },
    { "name", WideToUtf8(object.name) },
    { "relevance", object.relevance },
  };
  if (object.has_base_object) {
    nlohmann::json baseFiles = nlohmann::json::array();
    for (const auto& file : object.base_source_files) {
      baseFiles.push_back(WideToUtf8(file));
    }
    entry["base_object"] = {
      { "type", WideToUtf8(object.base_type_name) },
      { "form_id", object.base_form_id },
      { "source_files", std::move(baseFiles) },
    };
  }
  return entry;
}

}  // namespace

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
  summary["dump_identity"] = DumpIdentityJson(r.dump_identity);
  summary["pid"] = r.pid;
  summary["state_flags"] = r.state_flags;
  summary["summary_sentence"] = WideToUtf8(r.summary_sentence);
  // Additive (schema v2 consumers ignore it): the summary in everyday words.
  if (!r.quick_answer.headline.empty()) {
    const auto& qa = r.quick_answer;
    summary["quick_answer"] = {
      { "kind", qa.kind },
      { "case", qa.case_id },
      { "suspect_mod", WideToUtf8(qa.suspect_mod) },
      { "suspect_file", WideToUtf8(qa.suspect_file) },
      { "likelihood", qa.likelihood },
      { "likelihood_text", WideToUtf8(qa.likelihood_text) },
      { "headline", WideToUtf8(qa.headline) },
      { "action", WideToUtf8(qa.action) },
      { "why_not_certain", WideToUtf8(qa.why_not_certain) },
      { "how_it_crashed", WideToUtf8(qa.how_it_crashed) },
    };
  }
  summary["crash_bucket_key"] = WideToUtf8(r.crash_bucket_key);
  summary["crash_bucket_version"] = r.crash_bucket_version;
  summary["analysis"] = {
    { "is_crash_like", r.is_crash_like },
    { "is_hang_like", r.is_hang_like },
    { "is_snapshot_like", r.is_snapshot_like },
    { "is_manual_capture", r.is_manual_capture },
    { "is_filtered_clean_exit", r.is_filtered_clean_exit },
  };
  if (r.is_filtered_clean_exit) {
    summary["clean_exit_evidence"] = {
      { "validated", true },
      { "dump_state", r.clean_exit_dump_state },
      { "filter_context", r.clean_exit_filter_context },
      { "sidecar_filename", WideToUtf8(r.clean_exit_evidence_filename) },
    };
  }
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
    if (!captureProfile.contains("include_process_thread_data")) {
      captureProfile["include_process_thread_data"] = false;
    }
    if (!captureProfile.contains("include_full_memory_info")) {
      captureProfile["include_full_memory_info"] = false;
    }
    if (!captureProfile.contains("include_module_headers")) {
      captureProfile["include_module_headers"] = false;
    }
    if (!captureProfile.contains("include_indirect_memory")) {
      captureProfile["include_indirect_memory"] = false;
    }
    if (!captureProfile.contains("ignore_inaccessible_memory")) {
      captureProfile["ignore_inaccessible_memory"] = false;
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
  LoadExistingSummaryTriage(summaryPath, outBase, r.dump_identity, &triage);
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
  summary["exception"]["external_software"] = WideToUtf8(r.fault_module_external_software);
  summary["game_version"] = r.game_version;

  summary["crash_logger"] = nlohmann::json::object();
  summary["crash_logger"]["log_path"] = WideToUtf8(MaybeRedactPath(r.crash_logger_log_path, redactPaths));
  summary["crash_logger"]["version"] = WideToUtf8(r.crash_logger_version);
  summary["crash_logger"]["pairing"] = {
    { "time_delta_ms", r.crash_logger_pairing_time_delta_ms },
    { "runner_up_time_delta_ms", r.crash_logger_pairing_runner_up_time_delta_ms },
    { "eligible_candidate_count", r.crash_logger_pairing_eligible_candidate_count },
    { "nearby_competitor_count", r.crash_logger_pairing_nearby_competitor_count },
    { "selected_kind", r.crash_logger_pairing_selected_kind },
    { "same_directory", r.crash_logger_pairing_same_directory },
    { "ambiguous", r.crash_logger_pairing_ambiguous },
    { "location_mismatch", r.crash_logger_location_mismatch },
    { "confidence", r.crash_logger_pairing_confidence },
  };
  summary["crash_logger"]["top_modules"] = nlohmann::json::array();
  for (const auto& m : r.crash_logger_top_modules) {
    summary["crash_logger"]["top_modules"].push_back(WideToUtf8(m));
  }
  summary["crash_logger"]["direct_fault_module"] = WideToUtf8(r.crash_logger_direct_fault_module);
  summary["crash_logger"]["first_actionable_probable_module"] = WideToUtf8(r.crash_logger_first_actionable_probable_module);
  summary["crash_logger"]["probable_streak_module"] = WideToUtf8(r.crash_logger_probable_streak_module);
  summary["crash_logger"]["probable_streak_length"] = r.crash_logger_probable_streak_length;
  summary["crash_logger"]["direct_fault_eligible"] = r.crash_logger_direct_fault_eligible;
  summary["crash_logger"]["first_actionable_probable_eligible"] = r.crash_logger_first_actionable_probable_eligible;
  summary["crash_logger"]["probable_streak_eligible"] = r.crash_logger_probable_streak_eligible;
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
  if (!r.crash_logger_exception_type.empty()) {
    summary["crash_logger"]["exception"] = {
      { "type", WideToUtf8(r.crash_logger_exception_type) },
      { "location", WideToUtf8(r.crash_logger_exception_location) },
      { "instruction", WideToUtf8(r.crash_logger_exception_instruction) },
    };
  }
  if (!r.crash_logger_unattributed_objects.empty()) {
    summary["crash_logger"]["objects_without_plugin"] = nlohmann::json::array();
    for (const auto& object : r.crash_logger_unattributed_objects) {
      summary["crash_logger"]["objects_without_plugin"].push_back({
        { "location", WideToUtf8(object.best_location) },
        { "object_type", WideToUtf8(object.best_object_type) },
        { "object_name", WideToUtf8(object.object_name) },
        { "form_id", WideToUtf8(object.form_id) },
      });
    }
  }

  if (!r.dump_objects.empty()) {
    // ADR-0010: objects read from the dump itself.
    summary["dump_objects"] = nlohmann::json::array();
    for (const auto& object : r.dump_objects) {
      summary["dump_objects"].push_back(DumpObjectJson(object));
    }
  }

  if (r.memory.has_system || r.memory.has_process || r.memory.has_gpu) {
    const MemoryPressure pressure = AssessMemoryPressure(r.memory);
    nlohmann::json memory = nlohmann::json::object();
    if (r.memory.has_system) {
      memory["system"] = {
        { "ram_total_bytes", r.memory.ram_total_bytes },
        { "ram_available_bytes", r.memory.ram_available_bytes },
        { "commit_used_bytes", r.memory.commit_used_bytes },
        { "commit_limit_bytes", r.memory.commit_limit_bytes },
        { "commit_percent", pressure.commit_percent },
        { "commit_includes_snapshot_clone", r.memory.commit_includes_snapshot_clone },
      };
    }
    if (r.memory.has_process) {
      memory["process"] = {
        { "private_bytes", r.memory.process_private_bytes },
        { "working_set_bytes", r.memory.process_working_set_bytes },
        { "peak_working_set_bytes", r.memory.process_peak_working_set_bytes },
      };
    }
    if (r.memory.has_gpu) {
      memory["gpu"] = {
        { "name", WideToUtf8(r.memory.gpu_name) },
        { "vram_usage_bytes", r.memory.vram_usage_bytes },
        { "vram_budget_bytes", r.memory.vram_budget_bytes },
        { "vram_dedicated_bytes", r.memory.vram_dedicated_bytes },
        { "shared_usage_bytes", r.memory.gpu_shared_usage_bytes },
        { "vram_percent", pressure.vram_percent },
      };
    }
    memory["commit_high"] = pressure.commit_high;
    memory["vram_high"] = pressure.vram_high;
    summary["memory"] = std::move(memory);
  }

  if (!r.problematic_modules.empty()) {
    summary["problematic_modules"] = nlohmann::json::array();
    for (const auto& module : r.problematic_modules) {
      summary["problematic_modules"].push_back({
        { "module", WideToUtf8(module.filename) },
        { "name", WideToUtf8(module.name) },
        { "mod_name", WideToUtf8(module.mod_name) },
      });
    }
  }

  if (r.signature_match.has_value()) {
    summary["signature_match"] = {
      { "id", r.signature_match->id },
      { "scope", r.signature_match->scope },
      { "mechanism", r.signature_match->scope == "mechanism" ? WideToUtf8(r.signature_match->cause) : "" },
      { "cause", WideToUtf8(r.signature_match->cause) },
      { "match_confidence", WideToUtf8(r.signature_match->confidence) },
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
      // Enough to see what went wrong; the full scan stays in the dump. The
      // text cannot be parsed, so its paths (the MO2 folders come first)
      // cannot be redacted one by one: a redacted summary leaves it out.
      if (!redactPaths) {
        constexpr std::size_t kRawPreviewBytes = 4096;
        summary["plugin_scan_raw"] = TruncateUtf8(r.plugin_scan_json_utf8, kRawPreviewBytes);
      }
    } else {
      // The per-plugin list (about 1 MB for a 4,000-plugin load order) is
      // input to the analysis, which reads it from the dump's own stream; the
      // summary keeps counts. Missing masters and plugin rules are reported
      // separately below.
      if (parsedPluginScan.is_object() && parsedPluginScan.contains("plugins") && parsedPluginScan["plugins"].is_array()) {
        std::size_t active = 0;
        std::size_t light = 0;
        for (const auto& plugin : parsedPluginScan["plugins"]) {
          active += plugin.value("is_active", false) ? 1u : 0u;
          light += plugin.value("is_esl", false) ? 1u : 0u;
        }
        parsedPluginScan["plugin_count"] = parsedPluginScan["plugins"].size();
        parsedPluginScan["active_plugin_count"] = active;
        parsedPluginScan["light_plugin_count"] = light;
        parsedPluginScan.erase("plugins");
      }
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

  if (!r.stackwalk_truncation.empty()) {
    char addr[32]{};
    std::snprintf(addr, sizeof(addr), "0x%llX", static_cast<unsigned long long>(r.stackwalk_truncation_address));
    summary["stackwalk_truncation"] = {
      { "reason", r.stackwalk_truncation },
      { "depth", r.stackwalk_truncation_depth },
      { "address", addr },
      { "module", WideToUtf8(r.stackwalk_truncation_module) },
      { "unloaded_module", WideToUtf8(r.stackwalk_truncation_unloaded_module) },
    };
  }
  if (!r.fault_unloaded_module.empty()) {
    summary["fault_unloaded_module"] = WideToUtf8(r.fault_unloaded_module);
  }
  // Additive (ADR-0019): what the faulting instruction of an access violation
  // did. Only the instruction's own bytes and register values; no memory.
  if (!r.fault_instruction.verdict.empty()) {
    const auto& fi = r.fault_instruction;
    const auto hex = [](std::uint64_t value) {
      char buf[32]{};
      std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(value));
      return std::string(buf);
    };
    nlohmann::json j = {
      { "verdict", fi.verdict },
      { "access", fi.access },
      { "address", hex(fi.address) },
      { "mnemonic", fi.mnemonic },
      { "operand", fi.operand },
      { "bytes", fi.bytes },
      { "access_size", fi.access_size },
      { "address_matches", fi.address_matches },
      { "text", WideToUtf8(DescribeFaultInstruction(fi, r.fault_module_plus_offset, r.language == i18n::Language::kEnglish)) },
    };
    if (fi.address_matches) {
      j["effective_address"] = hex(fi.effective_address);
      // A register the operand does not use is left out, not written empty.
      if (!fi.base_register.empty()) {
        j["base_register"] = fi.base_register;
        j["base_value"] = hex(fi.base_value);
      }
      if (!fi.index_register.empty()) {
        j["index_register"] = fi.index_register;
        j["index_value"] = hex(fi.index_value);
        j["scale"] = fi.scale;
      }
    }
    if (!fi.call_site.empty()) {
      j["call_site"] = WideToUtf8(fi.call_site);
      j["call_instruction"] = fi.call_instruction;
    }
    summary["fault_instruction"] = std::move(j);
  }
  if (r.hook_stub_race.detected) {
    const auto& race = r.hook_stub_race;
    char stub[32]{};
    char target[32]{};
    std::snprintf(stub, sizeof(stub), "0x%llX", static_cast<unsigned long long>(race.stub_address));
    std::snprintf(target, sizeof(target), "0x%llX", static_cast<unsigned long long>(race.target_offset));
    summary["hook_stub_race"] = {
      { "stub_address", stub },
      { "target_module", WideToUtf8(race.target_module) },
      { "target_offset", target },
      { "target_mod_name", WideToUtf8(race.target_mod_name) },
      { "call_site", WideToUtf8(race.call_site) },
    };
  }

  if (!r.thread_entry_hook_modules.empty()) {
    nlohmann::json hooks = nlohmann::json::array();
    for (const auto& module : r.thread_entry_hook_modules) {
      hooks.push_back(WideToUtf8(module));
    }
    summary["thread_entry_hooks"] = std::move(hooks);
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
  const std::wstring redactedSymbolSearchPath = RedactSymbolSearchPath(r.symbol_search_path, redactPaths);
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

  // crash_time: providers from the MO2 profile captured with the dump;
  // analysis_time: from the installation found when the dump was analyzed.
  summary["mo2"] = {
    { "providers_source", r.mo2_providers_source },
    { "profile", WideToUtf8(r.mo2_profile_name) },
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
  summary["freeze_analysis"]["thread_module_consensus"] = {
    { "has_consensus", r.freeze_analysis.thread_module_consensus.has_consensus },
    { "main_thread_id", r.freeze_analysis.thread_module_consensus.main_thread_id },
    { "module_filename", WideToUtf8(r.freeze_analysis.thread_module_consensus.module_filename) },
    { "matching_thread_count", r.freeze_analysis.thread_module_consensus.matching_thread_count },
    { "stable_thread_count", r.freeze_analysis.thread_module_consensus.stable_thread_count },
    { "os_lock_cycle_proven", r.freeze_analysis.thread_module_consensus.os_lock_cycle_proven },
  };
  summary["freeze_analysis"]["modal_dialog_wait"] = {
    { "detected", r.modal_dialog_wait.detected },
    { "window_evidence", r.modal_dialog_wait.window_evidence },
    { "stack_evidence", r.modal_dialog_wait.stack_evidence },
    { "main_thread_id", r.modal_dialog_wait.main_thread_id },
    { "wait_api", WideToUtf8(r.modal_dialog_wait.wait_api) },
    { "dialog_title", WideToUtf8(r.modal_dialog_wait.dialog_title) },
    { "dialog_text", WideToUtf8(r.modal_dialog_wait.dialog_text) },
    { "caller_module_filename", WideToUtf8(r.modal_dialog_wait.caller_module_filename) },
    { "caller_inferred_mod_name", WideToUtf8(r.modal_dialog_wait.caller_inferred_mod_name) },
    { "caller_kind", r.modal_dialog_wait.caller_kind },
    { "address_library_issue", r.modal_dialog_wait.address_library_issue },
    { "other_thread_dialog_count", r.modal_dialog_wait.other_thread_dialog_count },
  };
  {
    // File and mod names only (never paths), read from the already masked
    // dialog title and text.
    const auto& modal = r.modal_dialog_wait;
    auto& out = summary["freeze_analysis"]["modal_dialog_wait"];
    out["dialog_named_module"] = WideToUtf8(modal.named_module_filename);
    out["dialog_named_mod_name"] = WideToUtf8(modal.named_mod_name);
    out["dialog_named_module_source"] = modal.named_module_source;
    out["dialog_named_module_likelihood"] = modal.named_module_filename.empty() ? ""
      : (modal.named_module_level == i18n::ConfidenceLevel::kHigh ? "high" : "medium");
    auto loaded = nlohmann::json::array();
    for (const auto& name : modal.named_loaded_modules) {
      loaded.push_back(WideToUtf8(name));
    }
    out["dialog_named_loaded_modules"] = std::move(loaded);
    auto unloaded = nlohmann::json::array();
    for (const auto& name : modal.named_unloaded_modules) {
      unloaded.push_back(WideToUtf8(name));
    }
    out["dialog_named_unloaded_modules"] = std::move(unloaded);
  }
  {
    auto path = nlohmann::json::array();
    for (const auto& module : r.main_thread_wait.path_modules) {
      path.push_back(WideToUtf8(module));
    }
    summary["freeze_analysis"]["main_thread_wait"] = {
      { "kind", r.main_thread_wait.kind },
      { "wait_class", r.main_thread_wait.wait_class },
      { "engine_wait_detail", r.main_thread_wait.engine_wait_detail },
      { "wait_api", WideToUtf8(r.main_thread_wait.wait_api) },
      { "waiting_module", WideToUtf8(r.main_thread_wait.waiting_module) },
      { "waiting_mod_name", WideToUtf8(r.main_thread_wait.waiting_mod_name) },
      { "path_modules", std::move(path) },
    };
    if (r.main_thread_wait.blocked_thread_tid != 0u) {
      summary["freeze_analysis"]["main_thread_wait"]["blocked_thread"] = {
        { "tid", r.main_thread_wait.blocked_thread_tid },
        { "module", WideToUtf8(r.main_thread_wait.blocked_module) },
        { "mod_name", WideToUtf8(r.main_thread_wait.blocked_mod_name) },
        { "state", r.main_thread_wait.blocked_state },
        { "top", WideToUtf8(r.main_thread_wait.blocked_top) },
        { "thread_count", r.main_thread_wait.blocked_thread_count },
      };
      if (!r.main_thread_wait.blocked_path_modules.empty()) {
        auto below = nlohmann::json::array();
        for (const auto& module : r.main_thread_wait.blocked_path_modules) {
          below.push_back(WideToUtf8(module));
        }
        summary["freeze_analysis"]["main_thread_wait"]["blocked_thread"]["path_modules"] = std::move(below);
      }
    }
  }
  if (r.engine_spin.loop_found) {
    const auto& spin = r.engine_spin;
    auto callPath = nlohmann::json::array();
    for (const auto& module : spin.call_path_modules) {
      callPath.push_back(WideToUtf8(module));
    }
    char bits[32]{};
    std::snprintf(bits, sizeof(bits), "0x%llx", static_cast<unsigned long long>(spin.nan_bits));
    summary["freeze_analysis"]["engine_spin"] = {
      { "state", spin.nan_detected ? "engine_spin_nan" : "float_compare_loop" },
      { "nan_detected", spin.nan_detected },
      { "tid", spin.tid },
      { "is_main_thread", spin.is_main_thread },
      { "rip", WideToUtf8(EngineSpinLocation(spin)) },
      { "rip_module", WideToUtf8(spin.rip_module) },
      { "rip_offset", spin.rip_offset },
      { "compare", spin.compare },
      { "nan_register", spin.nan_register },
      { "nan_bits", spin.nan_detected ? std::string(bits) : std::string{} },
      { "mxcsr", spin.mxcsr },
      { "call_path_modules", std::move(callPath) },
    };
  }
  if (r.main_thread_lock_owner.tid != 0u) {
    const auto& owner = r.main_thread_lock_owner;
    auto frames = nlohmann::json::array();
    for (const auto& frame : owner.top_frames) {
      frames.push_back(WideToUtf8(frame));
    }
    summary["freeze_analysis"]["lock_owner"] = {
      { "tid", owner.tid },
      { "thread_name", WideToUtf8(owner.thread_name) },
      { "lock_type", owner.lock_type },
      { "confirmed", owner.confirmed },
      { "top_module", WideToUtf8(owner.top_module) },
      { "top_mod_name", WideToUtf8(owner.top_mod_name) },
      { "top_frames", std::move(frames) },
      { "blocked_without_wait_object", owner.blocked_without_wait_object },
      { "context_switches_stable", owner.context_switches_stable },
      { "ip_state", owner.ip_state },
    };
    const auto& deadlock = r.crash_logger_thread_dump_deadlock;
    summary["freeze_analysis"]["crash_logger_thread_dump_deadlock"] = {
      { "detected", deadlock.detected },
      { "crash_logger_tid", deadlock.crash_logger_tid },
      { "crash_logger_module", WideToUtf8(deadlock.crash_logger_module) },
      { "thread_dump_truncated", deadlock.thread_dump_truncated },
      { "thread_dump_records", deadlock.thread_dump_records },
      { "thread_dump_total_threads", deadlock.thread_dump_total_threads },
      { "thread_dump_before_capture", deadlock.thread_dump_before_capture },
      { "freeze_started_after_thread_dump", deadlock.freeze_started_after_thread_dump },
      { "thread_dump_stopped_before_capture", deadlock.thread_dump_stopped_before_capture },
    };
  }
  if (!r.freeze_thread_objects.empty()) {
    // ADR-0010 addendum: what the threads that matter were handling.
    auto threads = nlohmann::json::array();
    for (const auto& thread : r.freeze_thread_objects) {
      auto objects = nlohmann::json::array();
      for (const auto& object : thread.objects) {
        objects.push_back(DumpObjectJson(object));
      }
      threads.push_back({ { "tid", thread.tid }, { "roles", thread.roles }, { "objects", std::move(objects) } });
    }
    summary["freeze_analysis"]["thread_objects"] = std::move(threads);
    if (const auto* form = FreezeVerdictForm(r)) {
      // The form the quick answer's clue names, for the thread the verdict is about.
      auto entry = DumpObjectJson(*form);
      entry["tid"] = FreezeVerdictThreadId(r);
      entry["text"] = WideToUtf8(DescribeDumpObjectTarget(*form));
      summary["freeze_analysis"]["verdict_thread_object"] = std::move(entry);
    }
  }
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
  {
    // Game state at the capture (ADR-0009), from the full blackbox summary.
    const auto& state = r.blackbox_freeze_summary;
    auto menus = nlohmann::json::array();
    for (const auto& menu : state.open_menus) {
      menus.push_back(WideToUtf8(menu));
    }
    summary["freeze_analysis"]["game_state"] = {
      { "open_menus", std::move(menus) },
      { "seconds_since_load_end", state.seconds_since_load_end },
      { "pause_gap_seconds", state.pause_gap_seconds },
      { "pause_gap_ended_seconds_before", state.pause_gap_ended_seconds_before },
    };
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
  const auto freezeWct = internal::TryParseWctFreezeSummary(r.wct_json_utf8);
  summary["freeze_analysis"]["wct_consensus"] = {
    { "capture_passes", freezeWct ? freezeWct->capture_passes : 0u },
    { "cycle_consensus", freezeWct ? freezeWct->cycle_consensus : false },
    { "repeated_cycle_thread_count", freezeWct ? freezeWct->repeated_cycle_tids.size() : 0u },
    { "consistent_loading_signal", freezeWct ? freezeWct->consistent_loading_signal : false },
    { "longest_wait_tid_consensus", freezeWct ? freezeWct->longest_wait_tid_consensus : false },
  };

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
  // recommendation_classes[i] is the viewer section and tag id of
  // recommendations[i] (ADR-0018).
  summary["recommendation_classes"] = nlohmann::json::array();
  for (const auto& s : r.recommendations) {
    summary["recommendations"].push_back(WideToUtf8(s));
    const auto cls = ClassifyRecommendation(s);
    summary["recommendation_classes"].push_back({
      { "group", std::string(cls.group) },
      { "kind", std::string(cls.kind) },
    });
  }
  // Index into "recommendations" of the report's NextAction; -1 when there is none.
  const auto nextAction = SelectNextActionIndex(r.recommendations);
  summary["next_action_index"] = nextAction == std::wstring::npos ? -1 : static_cast<int>(nextAction);

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

  if (redactPaths) {
    // Besides the path fields above: the plugin scan's MO2 folders
    // (mo2.ini_path, base_dir, ...), and paths quoted in text from the dump
    // and its logs (a C++ exception's message, an error dialog) and in the
    // evidence and advice that repeat it. The triage notes are the
    // reviewer's own words and stay as written.
    for (auto it = summary.begin(); it != summary.end(); ++it) {
      if (it.key() != "triage") {
        RedactAbsolutePathStrings(&it.value());
      }
    }
  }

  return summary;
}

}  // namespace skydiag::dump_tool
