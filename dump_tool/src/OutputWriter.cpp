#include "OutputWriter.h"
#include "OutputWriterInternals.h"
#include "Utf.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cwctype>
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

using skydiag::dump_tool::internal::output_writer::DefaultOutDirForDump;
using skydiag::dump_tool::internal::output_writer::FindIncidentManifestForDump;
using skydiag::dump_tool::internal::output_writer::IsUnknownModuleField;
using skydiag::dump_tool::internal::output_writer::JoinList;
using skydiag::dump_tool::internal::output_writer::LoadExistingSummaryTriage;
using skydiag::dump_tool::internal::output_writer::MaybeRedactPath;
using skydiag::dump_tool::internal::output_writer::ReplaceAll;
using skydiag::dump_tool::internal::output_writer::TryLoadIncidentManifestJson;
using skydiag::dump_tool::internal::output_writer::WriteTextUtf8;

}  // namespace

bool WriteOutputs(const AnalysisResult& r, std::wstring* err)
{
  const std::filesystem::path dumpFs(r.dump_path);
  std::filesystem::path outBase = r.out_dir.empty() ? DefaultOutDirForDump(dumpFs) : std::filesystem::path(r.out_dir);
  std::error_code ec;
  std::filesystem::create_directories(outBase, ec);

  const std::wstring stem = dumpFs.stem().wstring();
  const bool en = (r.language == i18n::Language::kEnglish);
  const bool redactPaths = r.path_redaction_applied;

  const auto summaryPath = outBase / (stem + L"_SkyrimDiagSummary.json");
  const auto reportPath = outBase / (stem + L"_SkyrimDiagReport.txt");
  const auto blackboxPath = outBase / (stem + L"_SkyrimDiagBlackbox.jsonl");
  const auto wctPath = outBase / (stem + L"_SkyrimDiagWct.json");

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
    // Ensure stable schema keys in the output even if the manifest is missing fields.
    if (!artifacts.contains("etw")) {
      artifacts["etw"] = "";
    }

    nlohmann::json privacy = nlohmann::json::object();
    if (incidentManifest.contains("privacy") && incidentManifest["privacy"].is_object()) {
      privacy = incidentManifest["privacy"];
    }

    summary["incident"] = {
      { "incident_id", incidentManifest.value("incident_id", "") },
      { "capture_kind", incidentManifest.value("capture_kind", "") },
      { "artifacts", std::move(artifacts) },
      { "manifest_path", WideToUtf8(MaybeRedactPath(incidentManifestPath.wstring(), redactPaths)) },
      { "privacy", std::move(privacy) },
    };
  }
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
  summary["symbolization"] = {
    { "search_path", WideToUtf8(redactedSymbolSearchPath) },
    { "cache_path", WideToUtf8(redactedSymbolCachePath) },
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

  std::wstring writeErr;
  if (!WriteTextUtf8(summaryPath, summary.dump(2), &writeErr)) {
    if (err) *err = writeErr;
    return false;
  }

  // Report (human-friendly)
  std::ostringstream rpt;
  rpt << (en ? "SkyrimDiag Report\n" : "SkyrimDiag 리포트\n");
  rpt << (en ? "Dump: " : "덤프: ") << WideToUtf8(MaybeRedactPath(r.dump_path, redactPaths)) << "\n";
  rpt << (en ? "Summary: " : "결론: ") << WideToUtf8(r.summary_sentence) << "\n";
  rpt << (en ? "PathRedactionApplied: " : "경로 마스킹 적용: ") << (redactPaths ? "1" : "0") << "\n";
  rpt << (en ? "OnlineSymbolSourceAllowed: " : "온라인 심볼 소스 허용: ") << (r.online_symbol_source_allowed ? "1" : "0") << "\n";
  rpt << (en ? "OnlineSymbolSourceUsed: " : "온라인 심볼 소스 사용: ") << (r.online_symbol_source_used ? "1" : "0") << "\n";
  if (summary.contains("incident") && summary["incident"].is_object()) {
    const auto& inc = summary["incident"];
    if (inc.contains("incident_id") && inc["incident_id"].is_string()) {
      rpt << (en ? "IncidentId: " : "IncidentId: ") << inc["incident_id"].get<std::string>() << "\n";
    }
    if (inc.contains("capture_kind") && inc["capture_kind"].is_string()) {
      rpt << (en ? "CaptureKind: " : "CaptureKind: ") << inc["capture_kind"].get<std::string>() << "\n";
    }
    if (inc.contains("manifest_path") && inc["manifest_path"].is_string()) {
      rpt << (en ? "IncidentManifest: " : "Incident manifest: ") << inc["manifest_path"].get<std::string>() << "\n";
    }
    if (inc.contains("artifacts") && inc["artifacts"].is_object()) {
      const auto& art = inc["artifacts"];
      if (art.contains("etw") && art["etw"].is_string() && !art["etw"].get<std::string>().empty()) {
        rpt << (en ? "ETW: " : "ETW: ") << art["etw"].get<std::string>() << "\n";
      }
    }
  }
  if (!r.crash_bucket_key.empty()) {
    rpt << (en ? "CrashBucketKey: " : "크래시 버킷 키: ") << WideToUtf8(r.crash_bucket_key) << "\n";
  }
  if (r.has_plugin_scan) {
    rpt << (en ? "PluginScan: 1" : "플러그인 스캔: 1") << "\n";
  }
  if (!r.missing_masters.empty()) {
    rpt << (en ? "MissingMasters: " : "누락 마스터: ")
        << WideToUtf8(JoinList(r.missing_masters, 8, L", ")) << "\n";
  }
  if (r.needs_bees && r.is_crash_like) {
    rpt << (en ? "NeedsBEES: 1" : "BEES 필요: 1") << "\n";
  }
  if (!r.plugin_diagnostics.empty()) {
    std::vector<std::wstring> diagIds;
    diagIds.reserve(r.plugin_diagnostics.size());
    for (const auto& d : r.plugin_diagnostics) {
      diagIds.push_back(Utf8ToWide(d.rule_id));
    }
    rpt << (en ? "PluginRules: " : "플러그인 규칙: ")
        << WideToUtf8(JoinList(diagIds, 8, L", ")) << "\n";
  }
  rpt << "\n";
  rpt << (en ? "ExceptionCode: 0x" : "ExceptionCode: 0x") << std::hex << r.exc_code << std::dec << "\n";
  rpt << (en ? "ExceptionAddress: 0x" : "ExceptionAddress: 0x") << std::hex << r.exc_addr << std::dec << "\n";
  rpt << (en ? "ThreadId: " : "ThreadId: ") << r.exc_tid << "\n";
  rpt << (en ? "Module+Offset: " : "Module+Offset: ") << WideToUtf8(r.fault_module_plus_offset) << "\n";
  if (!r.inferred_mod_name.empty()) {
    rpt << (en ? "InferredMod: " : "추정 모드: ") << WideToUtf8(r.inferred_mod_name) << "\n";
  }
  if (!r.crash_logger_log_path.empty()) {
    rpt << (en ? "CrashLoggerLog: " : "Crash Logger 로그: ") << WideToUtf8(MaybeRedactPath(r.crash_logger_log_path, redactPaths)) << "\n";
  }
  if (!r.crash_logger_version.empty()) {
    rpt << (en ? "CrashLoggerVersion: " : "Crash Logger 버전: ") << WideToUtf8(r.crash_logger_version) << "\n";
  }
  if (!r.crash_logger_top_modules.empty()) {
    rpt << (en ? "CrashLoggerTopModules: " : "Crash Logger 상위 모듈: ")
        << WideToUtf8(JoinList(r.crash_logger_top_modules, 6, L", ")) << "\n";
  }
  if (!r.crash_logger_cpp_exception_type.empty() ||
      !r.crash_logger_cpp_exception_info.empty() ||
      !r.crash_logger_cpp_exception_throw_location.empty() ||
      !r.crash_logger_cpp_exception_module.empty()) {
    rpt << (en ? "CrashLoggerCppExceptionType: " : "Crash Logger C++ 예외 Type: ") << WideToUtf8(r.crash_logger_cpp_exception_type) << "\n";
    rpt << (en ? "CrashLoggerCppExceptionInfo: " : "Crash Logger C++ 예외 Info: ") << WideToUtf8(r.crash_logger_cpp_exception_info) << "\n";
    rpt << (en ? "CrashLoggerCppExceptionThrowLocation: " : "Crash Logger C++ 예외 Throw Location: ") << WideToUtf8(r.crash_logger_cpp_exception_throw_location) << "\n";
    rpt << (en ? "CrashLoggerCppExceptionModule: " : "Crash Logger C++ 예외 Module: ") << WideToUtf8(r.crash_logger_cpp_exception_module) << "\n";
  }
  rpt << (en ? "StateFlags: " : "StateFlags: ") << r.state_flags << "\n";
  rpt << (en ? "HasBlackbox: " : "HasBlackbox: ") << (r.has_blackbox ? "1" : "0") << "\n";
  rpt << (en ? "HasWCT: " : "HasWCT: ") << (r.has_wct ? "1" : "0") << "\n";
  rpt << (en ? "Suspects: " : "후보 개수: ") << r.suspects.size() << "\n";
  rpt << (en ? "SuspectsFromStackwalk: " : "콜스택 기반 후보: ") << (r.suspects_from_stackwalk ? "1" : "0") << "\n";
  rpt << (en ? "\nEvidence:\n" : "\n근거:\n");
  for (const auto& e : r.evidence) {
    rpt << "- [" << WideToUtf8(e.confidence) << "] " << WideToUtf8(e.title) << "\n";
    rpt << "  " << WideToUtf8(e.details) << "\n";
  }
  if (!r.stackwalk_primary_frames.empty()) {
    rpt << (en ? "\nCallstack (primary, tid=" : "\n콜스택(대표, tid=") << r.stackwalk_primary_tid << "):\n";
    for (const auto& f : r.stackwalk_primary_frames) {
      rpt << "  " << WideToUtf8(f) << "\n";
    }
  }
  if (!r.suspects.empty()) {
    rpt << (en ? "\nSuspects (" : "\n후보(") << (r.suspects_from_stackwalk ? "callstack" : "stack scan") << "):\n";
    for (const auto& s : r.suspects) {
      rpt << "- [" << WideToUtf8(s.confidence) << "] " << WideToUtf8(s.module_filename);
      if (!s.inferred_mod_name.empty()) {
        rpt << " (" << WideToUtf8(s.inferred_mod_name) << ")";
      }
      rpt << " score=" << s.score << "\n";
      rpt << "  " << WideToUtf8(s.reason) << "\n";
      if (!s.module_path.empty()) {
        rpt << "  path=" << WideToUtf8(MaybeRedactPath(s.module_path, redactPaths)) << "\n";
      }
    }
  }
  if (!r.resources.empty()) {
    rpt << (en ? "\nRecent resources (.nif/.hkx/.tri):\n" : "\n최근 리소스(.nif/.hkx/.tri):\n");
    for (const auto& rr : r.resources) {
      rpt << "- t_ms=" << rr.t_ms << " tid=" << rr.tid << " [" << WideToUtf8(rr.kind) << "] "
          << WideToUtf8(MaybeRedactPath(rr.path, redactPaths));
      if (!rr.providers.empty()) {
        rpt << " providers=" << WideToUtf8(JoinList(rr.providers, 10, L", "));
      }
      if (rr.is_conflict) {
        rpt << " (conflict)";
      }
      rpt << "\n";
    }
  }
  rpt << (en ? "\nRecommendations (checklist):\n" : "\n권장 조치(체크리스트):\n");
  for (const auto& s : r.recommendations) {
    rpt << "- " << WideToUtf8(s) << "\n";
  }
  rpt << (en ? "\nLast events (most recent last):\n" : "\n최근 이벤트(최신이 마지막):\n");
  for (const auto& ev : r.events) {
    rpt << "[" << ev.i << "] t_ms=" << ev.t_ms << " tid=" << ev.tid << " " << WideToUtf8(ev.type_name);
    if (!ev.detail.empty()) {
      rpt << " | " << WideToUtf8(ev.detail);
    }
    rpt << " a=" << ev.a << " b=" << ev.b << " c=" << ev.c << " d=" << ev.d << "\n";
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
      if (!ev.detail.empty()) {
        j["detail"] = WideToUtf8(ev.detail);
      }
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
