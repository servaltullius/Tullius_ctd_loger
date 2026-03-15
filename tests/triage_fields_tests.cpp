#include <cassert>
#include <filesystem>
#include <iterator>
#include <string>

#include "SourceGuardTestUtils.h"

using skydiag::tests::source_guard::ProjectRoot;
using skydiag::tests::source_guard::ReadAllText;
using skydiag::tests::source_guard::ReadProjectText;

static void TestOutputWriterHasTriageFields()
{
  const auto outputWriter = ReadProjectText("dump_tool/src/OutputWriter.cpp");
  assert(outputWriter.find("\"triage\"") != std::string::npos);
  assert(outputWriter.find("LoadExistingSummaryTriage") != std::string::npos);
  assert(outputWriter.find("\"signature_matched\"") != std::string::npos);

  const auto internals = ReadProjectText("dump_tool/src/OutputWriterInternals.cpp");
  assert(internals.find("\"review_status\"") != std::string::npos);
  assert(internals.find("\"verdict\"") != std::string::npos);
  assert(internals.find("\"actual_cause\"") != std::string::npos);
  assert(internals.find("\"ground_truth_mod\"") != std::string::npos);
}

static void TestBucketQualityScriptExists()
{
  const std::filesystem::path p = ProjectRoot() / "scripts" / "analyze_bucket_quality.py";
  assert(std::filesystem::exists(p));
}

int main()
{
  TestOutputWriterHasTriageFields();
  TestBucketQualityScriptExists();
  return 0;
}
