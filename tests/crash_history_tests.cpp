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

static void TestCrashHistoryApiExists()
{
  const auto header = ReadFile("dump_tool/src/CrashHistory.h");
  assert(header.find("CrashHistory") != std::string::npos);
  assert(header.find("AddEntry") != std::string::npos);
  assert(header.find("LoadFromFile") != std::string::npos);
  assert(header.find("SaveToFile") != std::string::npos);
  assert(header.find("GetModuleStats") != std::string::npos);
}

static void TestAnalyzerHasHistoryCorrelationField()
{
  const auto header = ReadFile("dump_tool/src/Analyzer.h");
  assert(header.find("BucketCorrelation") != std::string::npos);
  assert(header.find("history_correlation") != std::string::npos);
}

static void TestEvidenceHasCorrelationDisplay()
{
  const auto src = ReadFile("dump_tool/src/EvidenceBuilderInternalsEvidence.cpp");
  assert(src.find("history_correlation") != std::string::npos);
}

static void TestOutputWriterHasHistoryCorrelation()
{
  const auto src = ReadFile("dump_tool/src/OutputWriter.cpp");
  assert(src.find("history_correlation") != std::string::npos);
}

int main()
{
  TestCrashHistoryApiExists();
  TestAnalyzerHasHistoryCorrelationField();
  TestEvidenceHasCorrelationDisplay();
  TestOutputWriterHasHistoryCorrelation();
  return 0;
}
