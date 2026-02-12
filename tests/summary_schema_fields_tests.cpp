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

  const std::filesystem::path outputWriterPath = repoRoot / "dump_tool" / "src" / "OutputWriter.cpp";
  assert(std::filesystem::exists(outputWriterPath) && "OutputWriter.cpp not found");
  const std::string outputWriter = ReadAllText(outputWriterPath);

  AssertContains(outputWriter, "summary[\"triage\"]", "Missing triage object in summary output");
  AssertContains(outputWriter, "summary[\"schema\"]", "Missing summary schema metadata object");
  AssertContains(outputWriter, "LoadExistingSummaryTriage", "Missing triage merge logic for existing summary");
  AssertContains(outputWriter, "summary[\"symbolization\"]", "Missing symbolization object in summary output");
  AssertContains(outputWriter, "symbolized_frames", "Missing symbolization.symbolized_frames field in summary output");
  AssertContains(outputWriter, "source_line_frames", "Missing symbolization.source_line_frames field in summary output");

  const std::filesystem::path scriptPath = repoRoot / "scripts" / "analyze_bucket_quality.py";
  assert(std::filesystem::exists(scriptPath) && "scripts/analyze_bucket_quality.py not found");
  const std::string script = ReadAllText(scriptPath);
  AssertContains(script, "crash_bucket_key", "Bucket-quality script must read crash_bucket_key");
  AssertContains(script, "ground_truth_mod", "Bucket-quality script must read triage ground truth fields");
  return 0;
}
