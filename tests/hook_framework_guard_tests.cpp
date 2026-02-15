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

  const auto stackwalkScoringPath = repoRoot / "dump_tool" / "src" / "AnalyzerInternalsStackwalkScoring.cpp";
  const auto stackwalkPath = repoRoot / "dump_tool" / "src" / "AnalyzerInternalsStackwalk.cpp";
  const auto stackScanPath = repoRoot / "dump_tool" / "src" / "AnalyzerInternalsStackScan.cpp";
  const auto minidumpUtilPath = repoRoot / "dump_tool" / "src" / "MinidumpUtil.cpp";
  const auto summaryPath = repoRoot / "dump_tool" / "src" / "EvidenceBuilderInternalsSummary.cpp";
  const auto recPath = repoRoot / "dump_tool" / "src" / "EvidenceBuilderInternalsRecommendations.cpp";

  const std::string stackwalkScoring = ReadAllText(stackwalkScoringPath);
  const std::string stackwalk = ReadAllText(stackwalkPath);
  const std::string stackScan = ReadAllText(stackScanPath);
  const std::string minidumpUtil = ReadAllText(minidumpUtilPath);
  const std::string summary = ReadAllText(summaryPath);
  const std::string rec = ReadAllText(recPath);

  AssertContains(
    stackwalkScoring,
    "topIsCrashLogger",
    "Stackwalk scoring must explicitly treat CrashLogger as a special hook-framework false-positive case.");
  AssertContains(
    stackwalkScoring,
    "crashlogger.dll",
    "Stackwalk scoring must include CrashLogger.dll alias handling.");
  AssertContains(
    stackwalkScoring,
    "topIsSkseRuntime",
    "Stackwalk scoring must include SKSE runtime alias handling.");
  AssertContains(
    stackwalkScoring,
    "IsSkseModule",
    "Stackwalk scoring must use shared SKSE module detection.");
  AssertContains(
    stackwalkScoring,
    "promotedHookTop",
    "Stackwalk scoring must keep non-hook promotion marker logic.");

  AssertContains(
    stackwalk,
    "topIsHookFramework",
    "Stackwalk confidence boost must detect hook-framework top suspects.");
  AssertContains(
    stackwalk,
    "&& !topIsHookFramework",
    "Stackwalk confidence boost must be gated off for hook-framework top suspects.");

  AssertContains(
    stackScan,
    "topIsCrashLogger",
    "Stack-scan scoring must explicitly treat CrashLogger as a special hook-framework false-positive case.");
  AssertContains(
    stackScan,
    "crashlogger.dll",
    "Stack-scan scoring must include CrashLogger.dll alias handling.");
  AssertContains(
    stackScan,
    "topIsSkseRuntime",
    "Stack-scan scoring must include SKSE runtime alias handling.");
  AssertContains(
    stackScan,
    "IsSkseModule",
    "Stack-scan scoring must use shared SKSE module detection.");
  AssertContains(
    minidumpUtil,
    "skse64_",
    "Minidump util must keep SKSE runtime-pattern detection.");
  AssertContains(
    minidumpUtil,
    "sl.interposer.dll",
    "Minidump util fallback hook-framework list must include Streamline interposer alias.");
  AssertContains(
    stackScan,
    "is_known_hook_framework",
    "Stack-scan scoring must keep hook-framework confidence downgrade logic.");

  AssertContains(
    summary,
    "hasNonHookSuspect",
    "Summary builder must distinguish non-hook stack candidates when fault module is a hook framework.");
  AssertContains(
    summary,
    "topSuspectIsHookFramework",
    "Summary builder must avoid over-blaming hook-framework top suspects for game/system module crashes.");
  AssertContains(
    rec,
    "preferStackCandidateOverFault",
    "Recommendations must prefer stack candidates over hook-framework fault modules.");

  return 0;
}
