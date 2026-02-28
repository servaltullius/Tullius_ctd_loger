#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

static std::string ReadAllText(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::in | std::ios::binary);
  assert(in && "Failed to open file");
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static void AssertContains(const std::string& haystack, const char* needle, const char* message)
{
  assert(haystack.find(needle) != std::string::npos && message);
}

int main()
{
  const std::filesystem::path repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  const auto minidumpUtilPath = repoRoot / "dump_tool" / "src" / "MinidumpUtil.cpp";
  const auto analyzerPath = repoRoot / "dump_tool" / "src" / "Analyzer.cpp";
  const auto summaryPath = repoRoot / "dump_tool" / "src" / "EvidenceBuilderSummary.cpp";
  const auto recPath = repoRoot / "dump_tool" / "src" / "EvidenceBuilderRecommendations.cpp";
  const auto crashLoggerPath = repoRoot / "dump_tool" / "src" / "CrashLogger.cpp";
  const auto crashLoggerParseCorePath = repoRoot / "dump_tool" / "src" / "CrashLoggerParseCore.h";

  const std::string minidumpUtil = ReadAllText(minidumpUtilPath);
  const std::string analyzer = ReadAllText(analyzerPath);
  const std::string summary = ReadAllText(summaryPath);
  const std::string rec = ReadAllText(recPath);
  const std::string crashLogger = ReadAllText(crashLoggerPath);
  const std::string crashLoggerParseCore = ReadAllText(crashLoggerParseCorePath);

  AssertContains(
    minidumpUtil,
    "win32u.dll",
    "System module list must include win32u.dll.");
  AssertContains(
    minidumpUtil,
    "IsLikelyWindowsSystemModulePathLower",
    "Minidump util must keep Windows system-path detection.");
  AssertContains(
    minidumpUtil,
    "IsLikelyWindowsSystemModulePath(",
    "Minidump util must expose Windows system-path helper.");

  AssertContains(
    analyzer,
    "IsLikelyWindowsSystemModulePath(out.fault_module_path)",
    "Analyzer must use module path when classifying Windows system modules.");
  AssertContains(
    analyzer,
    "faultIsSystem",
    "Analyzer must gate inferred mod names for system modules.");
  AssertContains(
    analyzer,
    "out.inferred_mod_name.clear()",
    "Analyzer must clear bogus inferred mod names in guarded cases.");

  AssertContains(
    summary,
    "topSuspectIsSystem",
    "Summary builder must detect top system-DLL suspects.");
  AssertContains(
    summary,
    "waiting/victim location",
    "Summary builder must avoid over-blaming system DLLs for hang captures.");

  AssertContains(
    rec,
    "topStackCandidateIsSystem",
    "Recommendations must detect top system-DLL stack candidates.");
  AssertContains(
    rec,
    "Windows system DLL",
    "Recommendations must include system-DLL caution guidance.");

  AssertContains(
    crashLogger,
    "IsSystemishModule",
    "CrashLogger module filter must delegate to IsSystemishModule for system module detection.");
  AssertContains(
    crashLoggerParseCore,
    "win32u.dll",
    "CrashLogger parse core filter must include win32u.dll as system module.");

  return 0;
}
