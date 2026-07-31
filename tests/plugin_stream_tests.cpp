#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "SourceGuardTestUtils.h"

namespace {

using skydiag::tests::source_guard::AssertOrdered;
using skydiag::tests::source_guard::ExtractFunctionBody;
using skydiag::tests::source_guard::ReadSplitAwareText;

std::string ReadFile(const char* relPath)
{
  const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
  assert(root);
  std::filesystem::path p = std::filesystem::path(root) / relPath;
  return ReadSplitAwareText(p);
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
  assert(writeDumpBody.find("skydiag::SharedHeader committedHeader{}") != std::string::npos);
  assert(writeDumpBody.find("std::memcpy(&committedHeader, blackboxBytes.data()") != std::string::npos);
  assert(writeDumpBody.find("committedHeader.crash.exception_record") != std::string::npos);
  assert(writeDumpBody.find("reinterpret_cast<const skydiag::SharedLayout*>(blackboxBytes.data())") == std::string::npos);
  assert(
    writeDumpBody.find("shmSnapshot->header.crash.exception_record") == std::string::npos &&
    "Exception stream must be built from the same immutable copy as the blackbox user stream");
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

void TestCrashSeqlockProtocolVersionAndDumpCompatibility()
{
  const auto shared = ReadFile("shared/SkyrimDiagShared.h");
  const auto pluginSharedMemory = ReadFile("plugin/src/SharedMemory.cpp");
  const auto helperMain = ReadFile("helper/src/main.cpp");
  const auto processAttach = ReadFile("helper/src/ProcessAttach.cpp");
  const auto analyzerCapture = ReadFile("dump_tool/src/Analyzer.CaptureInputs.cpp");
  assert(
    shared.find("kVersion = 4") != std::string::npos &&
    "Incident ownership/ACK semantics require a new live helper/plugin protocol version");
  assert(
    pluginSharedMemory.find("g_shared->header.version = skydiag::kVersion") != std::string::npos &&
    "The live plugin mapping must advertise the current protocol version");
  assert(
    helperMain.find("proc.shm->header.version != skydiag::kVersion") != std::string::npos &&
    "The live helper must reject every non-current mapping before processing it");
  assert(
    processAttach.find("FILE_MAP_READ | FILE_MAP_WRITE") != std::string::npos &&
    "Protocol v4 helper attach must be writable so recovered incidents can be ACKed/rearmed");
  assert(
    analyzerCapture.find("ver != 2u") != std::string::npos &&
    analyzerCapture.find("ver != 3u") != std::string::npos &&
    analyzerCapture.find("ver != skydiag::kVersion") != std::string::npos &&
    "Offline analyzer must continue accepting v2 and v3 blackbox streams from existing dumps");
}

void TestAnalyzerHasPluginSidecarFallback()
{
  const auto impl = ReadFile("dump_tool/src/Analyzer.cpp");
  assert(impl.find("_PluginScan.json") != std::string::npos);
  assert(impl.find("ReadTextFileUtf8") != std::string::npos);
}

}  // namespace

int main()
{
  TestDumpWriterIncludesPluginStream();
  TestDumpWriterPopulatesExpectedStreams();
  TestDumpWriterHeaderHasPluginParam();
  TestCrashPathIsDumpFirst();
  TestCrashPathWritesPluginScanSidecar();
  TestCrashSeqlockProtocolVersionAndDumpCompatibility();
  TestAnalyzerHasPluginSidecarFallback();
  return 0;
}
