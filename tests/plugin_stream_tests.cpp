#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

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

void TestDumpWriterReservesThreeStreams()
{
  const auto impl = ReadFile("helper/src/DumpWriter.cpp");
  assert(impl.find("reserve(3)") != std::string::npos);
}

void TestDumpWriterHeaderHasPluginParam()
{
  const auto header = ReadFile("helper/include/SkyrimDiagHelper/DumpWriter.h");
  assert(header.find("pluginScanJson") != std::string::npos);
}

void TestCrashPathIsDumpFirst()
{
  const auto impl = ReadFile("helper/src/CrashCapture.cpp");
  const auto writePos = impl.find("WriteDumpWithStreams(");
  auto scanPos = impl.find("CollectPluginScanJson(");
  if (scanPos == std::string::npos) {
    scanPos = impl.find("ScanPlugins(");
  }
  assert(writePos != std::string::npos);
  assert(scanPos != std::string::npos);
  assert(writePos < scanPos && "Crash capture must write dump before plugin scanning");
}

void TestCrashPathWritesPluginScanSidecar()
{
  const auto impl = ReadFile("helper/src/CrashCapture.cpp");
  assert(impl.find("_PluginScan.json") != std::string::npos);
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
  TestDumpWriterReservesThreeStreams();
  TestDumpWriterHeaderHasPluginParam();
  TestCrashPathIsDumpFirst();
  TestCrashPathWritesPluginScanSidecar();
  TestAnalyzerHasPluginSidecarFallback();
  return 0;
}
