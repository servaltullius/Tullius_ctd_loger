#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "SourceGuardTestUtils.h"

using skydiag::tests::source_guard::ReadAllText;
using skydiag::tests::source_guard::ReadSplitAwareText;

static void AssertContains(const std::string& haystack, const char* needle, const char* message)
{
  if (haystack.find(needle) == std::string::npos) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

int main()
{
  const std::filesystem::path repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  const auto stackwalkScoringPath = repoRoot / "dump_tool" / "src" / "AnalyzerInternalsStackwalkScoring.cpp";
  const auto stackwalkPath = repoRoot / "dump_tool" / "src" / "AnalyzerInternalsStackwalk.cpp";
  const auto stackScanPath = repoRoot / "dump_tool" / "src" / "AnalyzerInternalsStackScan.cpp";
  const auto minidumpUtilPath = repoRoot / "dump_tool" / "src" / "MinidumpUtil.cpp";
  const auto summaryPath = repoRoot / "dump_tool" / "src" / "EvidenceBuilderSummary.cpp";
  const auto recPath = repoRoot / "dump_tool" / "src" / "EvidenceBuilderRecommendations.cpp";
  const std::string stackwalkScoring = ReadAllText(stackwalkScoringPath);
  const std::string stackwalk = ReadAllText(stackwalkPath);
  const std::string stackScan = ReadAllText(stackScanPath);
  const std::string minidumpUtil = ReadAllText(minidumpUtilPath);
  const std::string summary = ReadAllText(summaryPath);
  const std::string rec = ReadAllText(recPath);
  const std::string evidence = ReadSplitAwareText(repoRoot / "dump_tool" / "src" / "EvidenceBuilderEvidence.cpp");

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
    "topIsMo2Vfs",
    "Stackwalk scoring must include MO2 VFS alias handling.");
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
    "topIsMo2Vfs",
    "Stack-scan scoring must include MO2 VFS alias handling.");
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
    minidumpUtil,
    "usvfs_x64.dll",
    "Minidump util fallback hook-framework list must include MO2 usvfs alias.");
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
    summary,
    "actionable candidate",
    "Summary builder must expose an actionable non-victim candidate when hook/system top suspect is likely a victim location.");
  AssertContains(
    summary,
    "topCandidateConf",
    "Summary builder must use actionable-candidate confidence instead of hardcoded confidence for EXE candidate wording.");
  AssertContains(
    rec,
    "preferStackCandidateOverFault",
    "Recommendations must prefer stack candidates over hook-framework fault modules.");
  AssertContains(
    rec,
    "actionable ",
    "Recommendations must label actionable stack candidates explicitly.");
  AssertContains(
    rec,
    "r.needs_bees && isCrashLike",
    "BEES recommendation must be gated to crash-like incidents to avoid snapshot false positives.");
  AssertContains(
    rec,
    "allowTopSuspectActionRecommendations",
    "Top-suspect action recommendations must be gated off for snapshot-like incidents.");
  AssertContains(
    rec,
    "status_id == \"cross_validated\"",
    "Recommendations must branch on cross-validated actionable candidates.");
  AssertContains(
    rec,
    "status_id == \"conflicting\"",
    "Recommendations must branch on conflicting actionable candidates.");
  AssertContains(
    rec,
    "DumpMode=2",
    "Conflict or weak-candidate recommendations must guide users toward a richer recapture when agreement is insufficient.");
  AssertContains(
    rec,
    "!isCrashLike &&",
    "Recommendation builder must guard crash-only BEES guidance for non-crash incidents.");
  AssertContains(
    rec,
    "rec.find(L\"[BEES]\")",
    "Recommendation builder must filter plugin-rule BEES strings on non-crash incidents.");
  AssertContains(
    evidence,
    "selectedTop",
    "Evidence builder must align displayed top suspect with actionable candidate selection when victim-like tops are present.");
  AssertContains(
    evidence,
    "r.needs_bees && ctx.isCrashLike",
    "Evidence builder must gate BEES evidence cards to crash-like incidents.");

  return 0;
}
