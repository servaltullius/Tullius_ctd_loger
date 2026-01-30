#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace skydiag::dump_tool {

struct EvidenceItem
{
  std::wstring confidence;  // "높음/중간/낮음"
  std::wstring title;
  std::wstring details;
};

struct SuspectItem
{
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

struct AnalysisResult
{
  std::wstring dump_path;
  std::wstring out_dir;

  std::uint32_t pid = 0;
  std::uint32_t state_flags = 0;

  std::uint32_t exc_code = 0;
  std::uint32_t exc_tid = 0;
  std::uint64_t exc_addr = 0;

  std::wstring fault_module_path;      // full path if available
  std::wstring fault_module_filename;  // basename
  std::wstring fault_module_plus_offset;
  std::wstring inferred_mod_name;  // best-effort (MO2 mods\<modname>\...)

  // Optional: Crash Logger SSE/AE integration (best-effort)
  std::wstring crash_logger_log_path;
  std::vector<std::wstring> crash_logger_top_modules;  // e.g. "hdtSMP64.dll", "MuJointFix.dll"

  // Heuristic: suspects inferred from stack/module scanning
  std::vector<SuspectItem> suspects;
  bool suspects_from_stackwalk = false;

  // Best-effort callstack (primary thread: crash thread, WCT cycle thread, or inferred main thread)
  std::uint32_t stackwalk_primary_tid = 0;
  std::vector<std::wstring> stackwalk_primary_frames;

  bool has_blackbox = false;
  std::vector<EventRow> events;

  // Optional: recent resource loads (best-effort; nif/hkx/tri)
  std::vector<ResourceRow> resources;

  bool has_wct = false;
  std::string wct_json_utf8;

  std::wstring summary_sentence;
  std::vector<EvidenceItem> evidence;
  std::vector<std::wstring> recommendations;
};

struct AnalyzeOptions
{
  bool debug = false;
};

bool AnalyzeDump(const std::wstring& dumpPath, const std::wstring& outDir, const AnalyzeOptions& opt, AnalysisResult& out, std::wstring* err);
bool WriteOutputs(const AnalysisResult& r, std::wstring* err);

}  // namespace skydiag::dump_tool
