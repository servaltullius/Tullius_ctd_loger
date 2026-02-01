#include "OutputWriter.h"
#include "Utf.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

namespace skydiag::dump_tool {
namespace {

std::wstring JoinList(const std::vector<std::wstring>& items, std::size_t maxN, std::wstring_view sep)
{
  if (items.empty() || maxN == 0) {
    return {};
  }
  const std::size_t n = std::min<std::size_t>(items.size(), maxN);
  std::wstring out;
  for (std::size_t i = 0; i < n; i++) {
    if (i > 0) {
      out += sep;
    }
    out += items[i];
  }
  if (items.size() > n) {
    out += sep;
    out += L"...";
  }
  return out;
}

bool WriteTextUtf8(const std::filesystem::path& path, const std::string& content, std::wstring* err)
{
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    if (err) *err = L"Failed to open output: " + path.wstring();
    return false;
  }
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!out) {
    if (err) *err = L"Failed to write output: " + path.wstring();
    return false;
  }
  if (err) err->clear();
  return true;
}

std::filesystem::path DefaultOutDirForDump(const std::filesystem::path& dumpPath)
{
  if (dumpPath.has_parent_path()) {
    return dumpPath.parent_path();
  }
  return std::filesystem::current_path();
}

}  // namespace

bool WriteOutputs(const AnalysisResult& r, std::wstring* err)
{
  const std::filesystem::path dumpFs(r.dump_path);
  std::filesystem::path outBase = r.out_dir.empty() ? DefaultOutDirForDump(dumpFs) : std::filesystem::path(r.out_dir);
  std::error_code ec;
  std::filesystem::create_directories(outBase, ec);

  const std::wstring stem = dumpFs.stem().wstring();

  const auto summaryPath = outBase / (stem + L"_SkyrimDiagSummary.json");
  const auto reportPath = outBase / (stem + L"_SkyrimDiagReport.txt");
  const auto blackboxPath = outBase / (stem + L"_SkyrimDiagBlackbox.jsonl");
  const auto wctPath = outBase / (stem + L"_SkyrimDiagWct.json");

  nlohmann::json summary = nlohmann::json::object();
  summary["dump_path"] = WideToUtf8(r.dump_path);
  summary["pid"] = r.pid;
  summary["state_flags"] = r.state_flags;
  summary["summary_sentence"] = WideToUtf8(r.summary_sentence);

  summary["exception"] = nlohmann::json::object();
  summary["exception"]["code"] = r.exc_code;
  summary["exception"]["thread_id"] = r.exc_tid;
  summary["exception"]["address"] = r.exc_addr;
  summary["exception"]["module_plus_offset"] = WideToUtf8(r.fault_module_plus_offset);
  summary["exception"]["module_path"] = WideToUtf8(r.fault_module_path);
  summary["exception"]["inferred_mod_name"] = WideToUtf8(r.inferred_mod_name);

  summary["crash_logger"] = nlohmann::json::object();
  summary["crash_logger"]["log_path"] = WideToUtf8(r.crash_logger_log_path);
  summary["crash_logger"]["top_modules"] = nlohmann::json::array();
  for (const auto& m : r.crash_logger_top_modules) {
    summary["crash_logger"]["top_modules"].push_back(WideToUtf8(m));
  }

  summary["suspects"] = nlohmann::json::array();
  for (const auto& s : r.suspects) {
    summary["suspects"].push_back({
      { "confidence", WideToUtf8(s.confidence) },
      { "module_filename", WideToUtf8(s.module_filename) },
      { "module_path", WideToUtf8(s.module_path) },
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
      { "path", WideToUtf8(rr.path) },
      { "providers", std::move(providers) },
      { "is_conflict", rr.is_conflict },
    });
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

  std::wstring writeErr;
  if (!WriteTextUtf8(summaryPath, summary.dump(2), &writeErr)) {
    if (err) *err = writeErr;
    return false;
  }

  // Report (human-friendly)
  std::ostringstream rpt;
  rpt << "SkyrimDiag Report\n";
  rpt << "Dump: " << WideToUtf8(r.dump_path) << "\n";
  rpt << "Summary: " << WideToUtf8(r.summary_sentence) << "\n";
  rpt << "\n";
  rpt << "ExceptionCode: 0x" << std::hex << r.exc_code << std::dec << "\n";
  rpt << "ExceptionAddress: 0x" << std::hex << r.exc_addr << std::dec << "\n";
  rpt << "ThreadId: " << r.exc_tid << "\n";
  rpt << "Module+Offset: " << WideToUtf8(r.fault_module_plus_offset) << "\n";
  if (!r.inferred_mod_name.empty()) {
    rpt << "InferredMod: " << WideToUtf8(r.inferred_mod_name) << "\n";
  }
  if (!r.crash_logger_log_path.empty()) {
    rpt << "CrashLoggerLog: " << WideToUtf8(r.crash_logger_log_path) << "\n";
  }
  if (!r.crash_logger_top_modules.empty()) {
    rpt << "CrashLoggerTopModules: " << WideToUtf8(JoinList(r.crash_logger_top_modules, 6, L", ")) << "\n";
  }
  rpt << "StateFlags: " << r.state_flags << "\n";
  rpt << "HasBlackbox: " << (r.has_blackbox ? "1" : "0") << "\n";
  rpt << "HasWCT: " << (r.has_wct ? "1" : "0") << "\n";
  rpt << "Suspects: " << r.suspects.size() << "\n";
  rpt << "SuspectsFromStackwalk: " << (r.suspects_from_stackwalk ? "1" : "0") << "\n";
  rpt << "\nEvidence:\n";
  for (const auto& e : r.evidence) {
    rpt << "- [" << WideToUtf8(e.confidence) << "] " << WideToUtf8(e.title) << "\n";
    rpt << "  " << WideToUtf8(e.details) << "\n";
  }
  if (!r.stackwalk_primary_frames.empty()) {
    rpt << "\nCallstack (primary, tid=" << r.stackwalk_primary_tid << "):\n";
    for (const auto& f : r.stackwalk_primary_frames) {
      rpt << "  " << WideToUtf8(f) << "\n";
    }
  }
  if (!r.suspects.empty()) {
    rpt << "\nSuspects (" << (r.suspects_from_stackwalk ? "callstack" : "stack scan") << "):\n";
    for (const auto& s : r.suspects) {
      rpt << "- [" << WideToUtf8(s.confidence) << "] " << WideToUtf8(s.module_filename);
      if (!s.inferred_mod_name.empty()) {
        rpt << " (" << WideToUtf8(s.inferred_mod_name) << ")";
      }
      rpt << " score=" << s.score << "\n";
      rpt << "  " << WideToUtf8(s.reason) << "\n";
      if (!s.module_path.empty()) {
        rpt << "  path=" << WideToUtf8(s.module_path) << "\n";
      }
    }
  }
  if (!r.resources.empty()) {
    rpt << "\nRecent resources (.nif/.hkx/.tri):\n";
    for (const auto& rr : r.resources) {
      rpt << "- t_ms=" << rr.t_ms << " tid=" << rr.tid << " [" << WideToUtf8(rr.kind) << "] " << WideToUtf8(rr.path);
      if (!rr.providers.empty()) {
        rpt << " providers=" << WideToUtf8(JoinList(rr.providers, 10, L", "));
      }
      if (rr.is_conflict) {
        rpt << " (conflict)";
      }
      rpt << "\n";
    }
  }
  rpt << "\nRecommendations (checklist):\n";
  for (const auto& s : r.recommendations) {
    rpt << "- " << WideToUtf8(s) << "\n";
  }
  rpt << "\nLast events (most recent last):\n";
  for (const auto& ev : r.events) {
    rpt << "[" << ev.i << "] t_ms=" << ev.t_ms << " tid=" << ev.tid << " " << WideToUtf8(ev.type_name)
        << " a=" << ev.a << " b=" << ev.b << " c=" << ev.c << " d=" << ev.d << "\n";
  }

  if (!WriteTextUtf8(reportPath, rpt.str(), &writeErr)) {
    if (err) *err = writeErr;
    return false;
  }

  // Blackbox JSONL (optional)
  if (r.has_blackbox) {
    std::ostringstream bb;
    for (const auto& ev : r.events) {
      nlohmann::json j = nlohmann::json::object();
      j["i"] = ev.i;
      j["t_ms"] = ev.t_ms;
      j["tid"] = ev.tid;
      j["type"] = static_cast<std::uint32_t>(ev.type);
      j["type_name"] = WideToUtf8(ev.type_name);
      j["a"] = ev.a;
      j["b"] = ev.b;
      j["c"] = ev.c;
      j["d"] = ev.d;
      bb << j.dump() << "\n";
    }
    if (!WriteTextUtf8(blackboxPath, bb.str(), &writeErr)) {
      if (err) *err = writeErr;
      return false;
    }
  }

  if (r.has_wct) {
    if (!WriteTextUtf8(wctPath, r.wct_json_utf8, &writeErr)) {
      if (err) *err = writeErr;
      return false;
    }
  }

  if (err) err->clear();
  return true;
}

}  // namespace skydiag::dump_tool
