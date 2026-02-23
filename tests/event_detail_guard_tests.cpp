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

static void TestEventRowHasDetailField()
{
  const auto src = ReadFile("dump_tool/src/Analyzer.h");
  assert(src.find("std::wstring detail") != std::string::npos);
}

static void TestFormatEventDetailDeclared()
{
  const auto src = ReadFile("dump_tool/src/AnalyzerInternals.h");
  assert(src.find("FormatEventDetail") != std::string::npos);
}

static void TestMenuHashLookupExists()
{
  const auto src = ReadFile("dump_tool/src/AnalyzerInternals.cpp");
  assert(src.find("kKnownMenuHashes") != std::string::npos);
}

static void TestPerfHitchFormatInDetail()
{
  const auto src = ReadFile("dump_tool/src/AnalyzerInternals.cpp");
  assert(src.find("hitch=") != std::string::npos);
}

static void TestStateFlagsDecoding()
{
  const auto src = ReadFile("dump_tool/src/AnalyzerInternals.cpp");
  assert(src.find("Loading") != std::string::npos);
  assert(src.find("InMenu") != std::string::npos);
}

static void TestOutputWriterEmitsDetail()
{
  const auto src = ReadFile("dump_tool/src/OutputWriter.cpp");
  assert(src.find("\"detail\"") != std::string::npos);
  assert(src.find("ev.detail") != std::string::npos);
}

static void TestAnalyzerPopulatesDetail()
{
  const auto src = ReadFile("dump_tool/src/Analyzer.cpp");
  assert(src.find("FormatEventDetail") != std::string::npos);
}

static void TestPreFreezeContextEvidence()
{
  const auto src = ReadFile("dump_tool/src/EvidenceBuilderInternalsEvidence.cpp");
  assert(src.find("pre-freeze") != std::string::npos
      || src.find("PreFreezeContext") != std::string::npos
      || src.find("context before") != std::string::npos
      || src.find("\xec\xa7\x81\xec\xa0\x84 \xec\x83\x81\xed\x99\xa9") != std::string::npos);
}

static void TestPluginStoresMenuNameInPayload()
{
  const auto src = ReadFile("plugin/src/EventSinks.cpp");
  assert(src.find("memcpy") != std::string::npos);
}

static void TestAnalyzerExtractsMenuNameFromPayload()
{
  const auto src = ReadFile("dump_tool/src/AnalyzerInternals.cpp");
  assert(src.find("menuBuf") != std::string::npos);
}

int main()
{
  TestEventRowHasDetailField();
  TestFormatEventDetailDeclared();
  TestMenuHashLookupExists();
  TestPerfHitchFormatInDetail();
  TestStateFlagsDecoding();
  TestOutputWriterEmitsDetail();
  TestAnalyzerPopulatesDetail();
  TestPreFreezeContextEvidence();
  TestPluginStoresMenuNameInPayload();
  TestAnalyzerExtractsMenuNameFromPayload();
  return 0;
}
