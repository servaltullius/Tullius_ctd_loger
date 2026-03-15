#include <cassert>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <string>

#include "SourceGuardTestUtils.h"

using skydiag::tests::source_guard::ReadProjectText;

static void RequireContains(const std::string& haystack, const char* needle, const char* message)
{
  if (haystack.find(needle) == std::string::npos) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

static void TestCrashHistoryApiExists()
{
  const auto header = ReadProjectText("dump_tool/src/CrashHistory.h");
  assert(header.find("CrashHistory") != std::string::npos);
  assert(header.find("AddEntry") != std::string::npos);
  assert(header.find("LoadFromFile") != std::string::npos);
  assert(header.find("SaveToFile") != std::string::npos);
  assert(header.find("GetModuleStats") != std::string::npos);
}

static void TestAnalyzerHasHistoryCorrelationField()
{
  const auto header = ReadProjectText("dump_tool/src/Analyzer.h");
  assert(header.find("BucketCorrelation") != std::string::npos);
  assert(header.find("history_correlation") != std::string::npos);
}

static void TestEvidenceHasCorrelationDisplay()
{
  const auto src = ReadProjectText("dump_tool/src/EvidenceBuilderEvidence.cpp");
  assert(src.find("history_correlation") != std::string::npos);
}

static void TestOutputWriterHasHistoryCorrelation()
{
  const auto src = ReadProjectText("dump_tool/src/OutputWriter.cpp");
  assert(src.find("history_correlation") != std::string::npos);
}

static void TestActionableCandidatesDoNotUseGlobalHistoryStats()
{
  const auto src = ReadProjectText("dump_tool/src/EvidenceBuilderCandidates.cpp");
  assert(src.find("history_stats") == std::string::npos &&
         "Actionable candidate scoring must not read global history_stats; use bucket-scoped repeats instead.");
}

static void TestActionableCandidatesUseBucketScopedHistoryRepeats()
{
  const auto header = ReadProjectText("dump_tool/src/CrashHistory.h");
  RequireContains(header, "GetBucketCandidateStats",
                  "CrashHistory must expose bucket-scoped candidate repeat stats.");

  const auto src = ReadProjectText("dump_tool/src/EvidenceBuilderCandidates.cpp");
  RequireContains(src, "bucket_candidate_repeats",
                  "Actionable candidate scoring must consume bucket-scoped candidate repeat signals.");
  RequireContains(src, "\"history_repeat\"",
                  "Actionable candidate scoring must emit history_repeat family when bucket repeats support a candidate.");
}

static void TestRecommendationsHasTroubleshootingGuide()
{
  const auto rec = ReadProjectText("dump_tool/src/EvidenceBuilderRecommendations.cpp");
  assert(rec.find("troubleshooting") != std::string::npos || rec.find("Troubleshooting") != std::string::npos);

  const auto header = ReadProjectText("dump_tool/src/Analyzer.h");
  assert(header.find("troubleshooting_steps") != std::string::npos);

  const auto writer = ReadProjectText("dump_tool/src/OutputWriter.cpp");
  assert(writer.find("troubleshooting_steps") != std::string::npos);
}

int main()
{
  TestCrashHistoryApiExists();
  TestAnalyzerHasHistoryCorrelationField();
  TestEvidenceHasCorrelationDisplay();
  TestOutputWriterHasHistoryCorrelation();
  TestActionableCandidatesDoNotUseGlobalHistoryStats();
  TestActionableCandidatesUseBucketScopedHistoryRepeats();
  TestRecommendationsHasTroubleshootingGuide();
  return 0;
}
