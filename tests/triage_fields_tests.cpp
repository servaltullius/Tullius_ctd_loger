#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

static std::string ReadFile(const char* relPath)
{
  const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
  assert(root && "SKYDIAG_PROJECT_ROOT must be set");
  const std::filesystem::path p = std::filesystem::path(root) / relPath;
  std::ifstream in(p, std::ios::in | std::ios::binary);
  assert(in && "Failed to open file");
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static void TestOutputWriterHasTriageFields()
{
  const auto outputWriter = ReadFile("dump_tool/src/OutputWriter.cpp");
  assert(outputWriter.find("\"triage\"") != std::string::npos);
  assert(outputWriter.find("LoadExistingSummaryTriage") != std::string::npos);
  assert(outputWriter.find("\"signature_matched\"") != std::string::npos);

  const auto internals = ReadFile("dump_tool/src/OutputWriterInternals.cpp");
  assert(internals.find("\"review_status\"") != std::string::npos);
  assert(internals.find("\"verdict\"") != std::string::npos);
  assert(internals.find("\"actual_cause\"") != std::string::npos);
  assert(internals.find("\"ground_truth_mod\"") != std::string::npos);
}

static void TestBucketQualityScriptExists()
{
  const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
  assert(root);
  const std::filesystem::path p = std::filesystem::path(root) / "scripts" / "analyze_bucket_quality.py";
  assert(std::filesystem::exists(p));
}

int main()
{
  TestOutputWriterHasTriageFields();
  TestBucketQualityScriptExists();
  return 0;
}
