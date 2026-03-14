#pragma once

#include "Analyzer.h"

#include <filesystem>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace skydiag::dump_tool {

nlohmann::json BuildSummaryJson(
  const AnalysisResult& r,
  const std::filesystem::path& outBase,
  const std::wstring& stem,
  bool redactPaths);

std::string BuildReportText(
  const AnalysisResult& r,
  const nlohmann::json& summary,
  bool redactPaths);

}  // namespace skydiag::dump_tool
