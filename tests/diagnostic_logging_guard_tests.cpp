#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

static std::string ReadAllText(const std::filesystem::path& path)
{
  std::ostringstream ss;
  const auto append = [&](const std::filesystem::path& inputPath) {
    std::ifstream in(inputPath, std::ios::in | std::ios::binary);
    assert(in && "Failed to open file");
    ss << in.rdbuf();
  };
  append(path);
  if (path.filename() == "Analyzer.cpp") {
    append(path.parent_path() / "Analyzer.CaptureInputs.cpp");
    append(path.parent_path() / "Analyzer.History.cpp");
  }
  if (path.filename() == "OutputWriter.cpp") {
    append(path.parent_path() / "OutputWriter.Summary.cpp");
    append(path.parent_path() / "OutputWriter.Report.cpp");
  }
  return ss.str();
}

static void AssertContains(const std::string& haystack, const char* needle, const char* message)
{
  assert(haystack.find(needle) != std::string::npos && message);
}

int main()
{
  const std::filesystem::path repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  // AnalysisResult must have a diagnostics vector
  const auto analyzerH = ReadAllText(repoRoot / "dump_tool" / "src" / "Analyzer.h");
  AssertContains(analyzerH, "std::vector<std::wstring> diagnostics",
    "AnalysisResult must contain 'diagnostics' vector for diagnostic logging.");

  // Analyzer.cpp must push diagnostic messages for key failure paths
  const auto analyzerCpp = ReadAllText(repoRoot / "dump_tool" / "src" / "Analyzer.cpp");
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
  const auto outputWriter = ReadAllText(repoRoot / "dump_tool" / "src" / "OutputWriter.cpp");
  AssertContains(outputWriter, "r.diagnostics",
    "OutputWriter must reference r.diagnostics for output.");
  AssertContains(outputWriter, "\"diagnostics\"",
    "OutputWriter must write 'diagnostics' key to JSON.");

  // WinUI AnalysisSummary must deserialize diagnostics
  const auto analysisSummaryCs = ReadAllText(repoRoot / "dump_tool_winui" / "AnalysisSummary.cs");
  AssertContains(analysisSummaryCs, "Diagnostics",
    "WinUI AnalysisSummary must include Diagnostics property.");

  return 0;
}
