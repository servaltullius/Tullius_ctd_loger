#include "OutputWriter.h"
#include "Utf.h"

#include <Windows.h>

#include <algorithm>
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

bool ReadTextUtf8(const std::filesystem::path& path, std::string* out)
{
  if (out) {
    out->clear();
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  if (out) {
    *out = ss.str();
  }
  return true;
}

nlohmann::json DefaultTriageFields()
{
  return {
    { "review_status", "unreviewed" },
    { "ground_truth_cause", "" },
    { "ground_truth_mod", "" },
    { "reviewer", "" },
    { "reviewed_at_utc", "" },
    { "notes", "" },
  };
}

void LoadExistingSummaryTriage(const std::filesystem::path& summaryPath, nlohmann::json* triage)
{
  if (!triage) {
    return;
  }

  *triage = DefaultTriageFields();
  std::string existingText;
  if (!ReadTextUtf8(summaryPath, &existingText)) {
    return;
  }
  const auto existing = nlohmann::json::parse(existingText, nullptr, false);
  if (existing.is_discarded() || !existing.is_object()) {
    return;
  }
  const auto it = existing.find("triage");
  if (it == existing.end() || !it->is_object()) {
    return;
  }

  for (const auto& [k, v] : it->items()) {
    (*triage)[k] = v;
  }
  for (const auto& [k, v] : DefaultTriageFields().items()) {
    if (!triage->contains(k)) {
      (*triage)[k] = v;
    }
  }
}

bool IsUnknownModuleField(std::wstring_view modulePlusOffset)
{
  if (modulePlusOffset.empty()) {
    return true;
  }
  std::wstring normalized;
  normalized.reserve(modulePlusOffset.size());
  for (const wchar_t ch : modulePlusOffset) {
    normalized.push_back(static_cast<wchar_t>(std::towlower(ch)));
  }
  auto trimPred = [](wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
  };
  while (!normalized.empty() && trimPred(normalized.front())) {
    normalized.erase(normalized.begin());
  }
  while (!normalized.empty() && trimPred(normalized.back())) {
    normalized.pop_back();
  }
  return normalized.empty() ||
    normalized == L"unknown" ||
    normalized == L"<unknown>" ||
    normalized == L"n/a" ||
    normalized == L"none";
}

bool LooksLikeAbsolutePath(std::wstring_view path)
{
  if (path.size() >= 3 &&
      ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
      path[1] == L':' &&
      (path[2] == L'\\' || path[2] == L'/')) {
    return true;
  }
  if (path.size() >= 2 &&
      ((path[0] == L'\\' && path[1] == L'\\') || (path[0] == L'/' && path[1] == L'/'))) {
    return true;
  }
  return false;
}

std::wstring RedactPathValue(std::wstring_view path)
{
  const std::filesystem::path p(path);
  const std::wstring filename = p.filename().wstring();
  if (filename.empty()) {
    return L"<redacted>";
  }
  return L"<redacted>\\" + filename;
}

std::wstring MaybeRedactPath(std::wstring_view path, bool redactPaths)
{
  if (!redactPaths || path.empty() || !LooksLikeAbsolutePath(path)) {
    return std::wstring(path);
  }
  return RedactPathValue(path);
}

std::wstring ReplaceAll(std::wstring text, std::wstring_view from, std::wstring_view to)
{
  if (text.empty() || from.empty()) {
    return text;
  }
  std::size_t pos = 0;
  while ((pos = text.find(from, pos)) != std::wstring::npos) {
    text.replace(pos, from.size(), to);
    pos += to.size();
  }
  return text;
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
  summary["privacy"] = {
    { "path_redaction_applied", redactPaths },
    { "online_symbol_source_allowed", r.online_symbol_source_allowed },
    { "online_symbol_source_used", r.online_symbol_source_used },
  };
  nlohmann::json triage;
  LoadExistingSummaryTriage(summaryPath, &triage);
  summary["triage"] = std::move(triage);

  summary["exception"] = nlohmann::json::object();
  summary["exception"]["code"] = r.exc_code;
  summary["exception"]["thread_id"] = r.exc_tid;
  summary["exception"]["address"] = r.exc_addr;
  summary["exception"]["module_plus_offset"] = WideToUtf8(r.fault_module_plus_offset);
  summary["exception"]["fault_module_unknown"] = IsUnknownModuleField(r.fault_module_plus_offset);
  summary["exception"]["module_path"] = WideToUtf8(MaybeRedactPath(r.fault_module_path, redactPaths));
  summary["exception"]["inferred_mod_name"] = WideToUtf8(r.inferred_mod_name);

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
  if (!r.crash_bucket_key.empty()) {
    rpt << (en ? "CrashBucketKey: " : "크래시 버킷 키: ") << WideToUtf8(r.crash_bucket_key) << "\n";
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
