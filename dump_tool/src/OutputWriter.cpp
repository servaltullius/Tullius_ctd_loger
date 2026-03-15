#include "OutputWriter.h"
#include "OutputWriterPipeline.h"
#include "OutputWriterInternals.h"
#include "Utf.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>
namespace skydiag::dump_tool {

using skydiag::dump_tool::internal::output_writer::DefaultOutDirForDump;
using skydiag::dump_tool::internal::output_writer::WriteTextUtf8;

bool WriteOutputs(const AnalysisResult& r, std::wstring* err)
{
  const std::filesystem::path dumpFs(r.dump_path);
  std::filesystem::path outBase = r.out_dir.empty() ? DefaultOutDirForDump(dumpFs) : std::filesystem::path(r.out_dir);
  std::error_code ec;
  std::filesystem::create_directories(outBase, ec);

  const std::wstring stem = dumpFs.stem().wstring();
  const bool redactPaths = r.path_redaction_applied;

  const auto summaryPath = outBase / (stem + L"_SkyrimDiagSummary.json");
  const auto reportPath = outBase / (stem + L"_SkyrimDiagReport.txt");
  const auto blackboxPath = outBase / (stem + L"_SkyrimDiagBlackbox.jsonl");
  const auto wctPath = outBase / (stem + L"_SkyrimDiagWct.json");

  nlohmann::json summary = BuildSummaryJson(r, outBase, stem, redactPaths);

  std::wstring writeErr;
  if (!WriteTextUtf8(summaryPath, summary.dump(2), &writeErr)) {
    if (err) *err = writeErr;
    return false;
  }

  if (!WriteTextUtf8(reportPath, BuildReportText(r, summary, redactPaths), &writeErr)) {
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
