#include <cassert>
#include <iterator>
#include <string>

#include "SourceGuardTestUtils.h"

using skydiag::tests::source_guard::ReadProjectText;

static void TestEventRowHasDetailField()
{
  const auto src = ReadProjectText("dump_tool/src/Analyzer.h");
  assert(src.find("std::wstring detail") != std::string::npos);
}

static void TestFormatEventDetailDeclared()
{
  const auto src = ReadProjectText("dump_tool/src/AnalyzerInternals.h");
  assert(src.find("FormatEventDetail") != std::string::npos);
}

static void TestMenuHashLookupExists()
{
  const auto src = ReadProjectText("dump_tool/src/AnalyzerInternals.cpp");
  assert(src.find("kKnownMenuHashes") != std::string::npos);
}

static void TestPerfHitchFormatInDetail()
{
  const auto src = ReadProjectText("dump_tool/src/AnalyzerInternals.cpp");
  assert(src.find("hitch=") != std::string::npos);
}

static void TestStateFlagsDecoding()
{
  const auto src = ReadProjectText("dump_tool/src/AnalyzerInternals.cpp");
  assert(src.find("Loading") != std::string::npos);
  assert(src.find("InMenu") != std::string::npos);
}

static void TestOutputWriterEmitsDetail()
{
  const auto src = ReadProjectText("dump_tool/src/OutputWriter.cpp");
  assert(src.find("\"detail\"") != std::string::npos);
  assert(src.find("ev.detail") != std::string::npos);
}

static void TestAnalyzerPopulatesDetail()
{
  const auto src = ReadProjectText("dump_tool/src/Analyzer.cpp");
  assert(src.find("FormatEventDetail") != std::string::npos);
}

static void TestPreFreezeContextEvidence()
{
  const auto src = ReadProjectText("dump_tool/src/EvidenceBuilderEvidence.cpp");
  assert(src.find("pre-freeze") != std::string::npos
      || src.find("PreFreezeContext") != std::string::npos
      || src.find("context before") != std::string::npos
      || src.find("\xec\xa7\x81\xec\xa0\x84 \xec\x83\x81\xed\x99\xa9") != std::string::npos);
}

static void TestPluginStoresMenuNameInPayload()
{
  const auto src = ReadProjectText("plugin/src/EventSinks.cpp");
  assert(src.find("memcpy") != std::string::npos);
}

static void TestAnalyzerExtractsMenuNameFromPayload()
{
  const auto src = ReadProjectText("dump_tool/src/AnalyzerInternals.cpp");
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
