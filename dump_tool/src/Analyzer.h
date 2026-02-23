#pragma once

#include "CrashHistory.h"
#include "GraphicsInjectionDiag.h"
#include "PluginRules.h"
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "I18nCore.h"
#include "SignatureDatabase.h"

namespace skydiag::dump_tool {

struct EvidenceItem
{
  i18n::ConfidenceLevel confidence_level = i18n::ConfidenceLevel::kUnknown;
  std::wstring confidence;  // "높음/중간/낮음"
  std::wstring title;
  std::wstring details;
};

struct SuspectItem
{
  i18n::ConfidenceLevel confidence_level = i18n::ConfidenceLevel::kUnknown;
  std::wstring confidence;  // "높음/중간/낮음"
  std::wstring module_filename;
  std::wstring module_path;
  std::wstring inferred_mod_name;  // best-effort (MO2 mods\<modname>\...)
  std::uint32_t score = 0;  // stack-hit count (heuristic)
  std::wstring reason;
};

struct EventRow
{
  std::uint32_t i = 0;
  double t_ms = 0.0;
  std::uint32_t tid = 0;
  std::uint16_t type = 0;
  std::wstring type_name;
  std::uint64_t a = 0;
  std::uint64_t b = 0;
  std::uint64_t c = 0;
  std::uint64_t d = 0;
};

struct ResourceRow
{
  double t_ms = 0.0;
  std::uint32_t tid = 0;
  std::wstring path;  // e.g. "meshes\\foo\\bar.nif" (normalized best-effort)
  std::wstring kind;  // e.g. "nif/hkx/tri"
  std::vector<std::wstring> providers;  // MO2 providers (mods/overwrite) best-effort
  bool is_conflict = false;  // providers.size() >= 2
};

struct BucketCorrelation
{
  std::size_t count = 0;
  std::string first_seen;
  std::string last_seen;
};

struct AnalysisResult
{
  i18n::Language language = i18n::DefaultLanguage();
  std::wstring dump_path;
  std::wstring out_dir;

  std::uint32_t pid = 0;
  std::uint32_t state_flags = 0;

  std::uint32_t exc_code = 0;
  std::uint32_t exc_tid = 0;
  std::uint64_t exc_addr = 0;
  std::uint64_t fault_module_offset = 0;  // offset in fault module (if resolved)
  std::vector<std::uint64_t> exc_info;  // MINIDUMP_EXCEPTION.ExceptionInformation (best-effort)
  std::wstring crash_bucket_key;  // stable key for repeated CTD grouping (best-effort)
  std::string game_version;  // best-effort game executable version (e.g. "1.5.97.0")

  std::wstring fault_module_path;      // full path if available
  std::wstring fault_module_filename;  // basename
  std::wstring fault_module_plus_offset;
  std::wstring inferred_mod_name;  // best-effort (MO2 mods\<modname>\...)

  // Optional: Crash Logger SSE/AE integration (best-effort)
  std::wstring crash_logger_log_path;
  std::wstring crash_logger_version;
  std::vector<std::wstring> crash_logger_top_modules;  // e.g. "hdtSMP64.dll", "MuJointFix.dll"
  std::wstring crash_logger_cpp_exception_type;
  std::wstring crash_logger_cpp_exception_info;
  std::wstring crash_logger_cpp_exception_throw_location;
  std::wstring crash_logger_cpp_exception_module;

  // Heuristic: suspects inferred from stack/module scanning
  std::vector<SuspectItem> suspects;
  bool suspects_from_stackwalk = false;
  std::optional<SignatureMatch> signature_match;
  std::unordered_map<std::uint64_t, std::string> resolved_functions;
  std::vector<ModuleStats> history_stats;
  BucketCorrelation history_correlation;
  GraphicsEnvironment graphics_env;
  std::optional<GraphicsDiagResult> graphics_diag;
  bool has_plugin_scan = false;
  std::string plugin_scan_json_utf8;
  std::vector<std::wstring> missing_masters;
  bool needs_bees = false;
  std::vector<PluginRuleDiagnosis> plugin_diagnostics;

  // Best-effort callstack (primary thread: crash thread, WCT cycle thread, or inferred main thread)
  std::uint32_t stackwalk_primary_tid = 0;
  std::vector<std::wstring> stackwalk_primary_frames;
  std::uint32_t stackwalk_total_frames = 0;
  std::uint32_t stackwalk_symbolized_frames = 0;
  std::uint32_t stackwalk_source_line_frames = 0;
  std::wstring symbol_search_path;  // effective DbgHelp search path (best-effort)
  std::wstring symbol_cache_path;   // inferred/selected local cache path (best-effort)
  bool online_symbol_source_allowed = false;
  bool online_symbol_source_used = false;
  bool path_redaction_applied = true;

  bool has_blackbox = false;
  std::vector<EventRow> events;

  // Optional: recent resource loads (best-effort; nif/hkx/tri)
  std::vector<ResourceRow> resources;

  bool has_wct = false;
  std::string wct_json_utf8;

  std::wstring summary_sentence;
  std::vector<EvidenceItem> evidence;
  std::vector<std::wstring> recommendations;
  std::vector<std::wstring> troubleshooting_steps;
  std::wstring troubleshooting_title;
};

struct AnalyzeOptions
{
  bool debug = false;
  bool allow_online_symbols = false;
  bool redact_paths = true;
  std::wstring data_dir;  // Optional analyzer data directory (e.g. "<exe>/data")
  std::string game_version;  // Optional override (e.g. "1.6.640.0")
  std::wstring output_dir;   // Optional output base directory for crash history
  i18n::Language language = i18n::DefaultLanguage();
};

bool AnalyzeDump(const std::wstring& dumpPath, const std::wstring& outDir, const AnalyzeOptions& opt, AnalysisResult& out, std::wstring* err);

}  // namespace skydiag::dump_tool
