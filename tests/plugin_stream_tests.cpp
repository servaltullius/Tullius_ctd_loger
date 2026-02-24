#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "SourceGuardTestUtils.h"

namespace {

using skydiag::tests::source_guard::AssertOrdered;
using skydiag::tests::source_guard::ExtractFunctionBody;

std::string ReadFile(const char* relPath)
{
  const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
  assert(root);
  std::filesystem::path p = std::filesystem::path(root) / relPath;
  std::ifstream f(p);
  assert(f.is_open());
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void TestDumpWriterIncludesPluginStream()
{
  const auto impl = ReadFile("helper/src/DumpWriter.cpp");
  assert(impl.find("kMinidumpUserStream_PluginInfo") != std::string::npos);
}

void TestDumpWriterPopulatesExpectedStreams()
{
  const auto dumpWriter = ReadFile("helper/src/DumpWriter.cpp");
  const auto writeDumpBody = ExtractFunctionBody(dumpWriter, "bool WriteDumpWithStreams(");
  assert(writeDumpBody.find("kMinidumpUserStream_Blackbox") != std::string::npos);
  assert(writeDumpBody.find("kMinidumpUserStream_WctJson") != std::string::npos);
  assert(writeDumpBody.find("kMinidumpUserStream_PluginInfo") != std::string::npos);
  assert(writeDumpBody.find("if (!pluginScanJson.empty())") != std::string::npos);
}

void TestDumpWriterHeaderHasPluginParam()
{
  const auto header = ReadFile("helper/include/SkyrimDiagHelper/DumpWriter.h");
  assert(header.find("pluginScanJson") != std::string::npos);
}

void TestCrashPathIsDumpFirst()
{
  const auto impl = ReadFile("helper/src/CrashCapture.cpp");
  const auto crashTickBody = ExtractFunctionBody(impl, "bool HandleCrashEventTick(");
  const auto writePos = crashTickBody.find("WriteDumpWithStreams(");
  const auto processPos = crashTickBody.find("ProcessValidCrashDump(");
  assert(writePos != std::string::npos);
  assert(processPos != std::string::npos);
  assert(writePos < processPos && "Crash capture must write dump before post-processing");
}

void TestCrashPathWritesPluginScanSidecar()
{
  const auto impl = ReadFile("helper/src/CrashCapture.cpp");
  const auto processValidBody = ExtractFunctionBody(impl, "void ProcessValidCrashDump(");
  assert(processValidBody.find("_PluginScan.json") != std::string::npos);
  AssertOrdered(
    processValidBody,
    "CollectPluginScanJson(",
    "WriteTextFileUtf8(pluginScanPath, pluginScanJson)",
    "Crash path must write plugin scan sidecar when collected.");
}

void TestAnalyzerHasPluginSidecarFallback()
{
  const auto impl = ReadFile("dump_tool/src/Analyzer.cpp");
  assert(impl.find("_PluginScan.json") != std::string::npos);
  assert(impl.find("TryReadTextFileUtf8") != std::string::npos);
}

}  // namespace

int main()
{
  TestDumpWriterIncludesPluginStream();
  TestDumpWriterPopulatesExpectedStreams();
  TestDumpWriterHeaderHasPluginParam();
  TestCrashPathIsDumpFirst();
  TestCrashPathWritesPluginScanSidecar();
  TestAnalyzerHasPluginSidecarFallback();
  return 0;
}
