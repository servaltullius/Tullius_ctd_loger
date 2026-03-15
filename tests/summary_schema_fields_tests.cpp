#include <cassert>
#include <string>

#include "SourceGuardTestUtils.h"

using skydiag::tests::source_guard::AssertContains;
using skydiag::tests::source_guard::ProjectRoot;
using skydiag::tests::source_guard::ReadAllText;
using skydiag::tests::source_guard::ReadProjectText;

int main()
{
  const auto outputWriter = ReadProjectText("dump_tool/src/OutputWriter.cpp");

  AssertContains(outputWriter, "summary[\"triage\"]", "Missing triage object in summary output");
  AssertContains(outputWriter, "summary[\"schema\"]", "Missing summary schema metadata object");
  AssertContains(outputWriter, "LoadExistingSummaryTriage", "Missing triage merge logic for existing summary");
  AssertContains(outputWriter, "summary[\"symbolization\"]", "Missing symbolization object in summary output");
  AssertContains(outputWriter, "symbolized_frames", "Missing symbolization.symbolized_frames field in summary output");
  AssertContains(outputWriter, "source_line_frames", "Missing symbolization.source_line_frames field in summary output");

  const auto scriptPath = ProjectRoot() / "scripts" / "analyze_bucket_quality.py";
  assert(std::filesystem::exists(scriptPath) && "scripts/analyze_bucket_quality.py not found");
  const std::string script = ReadAllText(scriptPath);
  AssertContains(script, "crash_bucket_key", "Bucket-quality script must read crash_bucket_key");
  AssertContains(script, "ground_truth_mod", "Bucket-quality script must read triage ground truth fields");
  return 0;
}
