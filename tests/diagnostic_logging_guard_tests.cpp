#include <cassert>
#include <filesystem>
#include <string>

#include "SourceGuardTestUtils.h"

using skydiag::tests::source_guard::AssertContains;
using skydiag::tests::source_guard::ReadProjectText;

int main()
{
  // AnalysisResult must have a diagnostics vector
  const auto analyzerH = ReadProjectText("dump_tool/src/Analyzer.h");
  AssertContains(analyzerH, "std::vector<std::wstring> diagnostics",
    "AnalysisResult must contain 'diagnostics' vector for diagnostic logging.");

  // Analyzer.cpp must push diagnostic messages for key failure paths
  const auto analyzerCpp = ReadProjectText("dump_tool/src/Analyzer.cpp");
  AssertContains(analyzerCpp, "out.diagnostics.push_back",
    "Analyzer.cpp must push diagnostic messages on failure paths.");
  AssertContains(analyzerCpp, "[CrashLogger]",
    "Analyzer.cpp must log CrashLogger integration failures.");
  AssertContains(analyzerCpp, "[Data] failed to load",
    "Analyzer.cpp must log data file load failures.");
  AssertContains(analyzerCpp, "[Stackwalk]",
    "Analyzer.cpp must log stackwalk fallback.");
  AssertContains(analyzerCpp, "[History]",
    "Analyzer.cpp must log history persistence failures.");

  // OutputWriter must serialize diagnostics to JSON and text
  const auto outputWriter = ReadProjectText("dump_tool/src/OutputWriter.cpp");
  AssertContains(outputWriter, "r.diagnostics",
    "OutputWriter must reference r.diagnostics for output.");
  AssertContains(outputWriter, "\"diagnostics\"",
    "OutputWriter must write 'diagnostics' key to JSON.");

  // WinUI AnalysisSummary must deserialize diagnostics
  const auto analysisSummaryCs = ReadProjectText("dump_tool_winui/AnalysisSummary.cs");
  AssertContains(analysisSummaryCs, "Diagnostics",
    "WinUI AnalysisSummary must include Diagnostics property.");

  return 0;
}
