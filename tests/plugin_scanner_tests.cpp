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

void TestPluginScannerHeaderExists()
{
  const auto header = ReadFile("helper/include/SkyrimDiagHelper/PluginScanner.h");
  assert(header.find("PluginMeta") != std::string::npos);
  assert(header.find("PluginScanResult") != std::string::npos);
  assert(header.find("ScanPlugins") != std::string::npos);
  assert(header.find("ParseTes4Header") != std::string::npos);
  assert(header.find("ParsePluginsTxt") != std::string::npos);
}

void TestPluginScannerImplExists()
{
  const auto impl = ReadFile("helper/src/PluginScanner.cpp");
  assert(impl.find("ParseTes4Header") != std::string::npos);
  assert(impl.find("TES4") != std::string::npos);
  assert(impl.find("MAST") != std::string::npos);
  assert(impl.find("0x0200") != std::string::npos);
}

void TestMo2SelectedProfileByteArraySupportExists()
{
  const auto impl = ReadFile("helper/src/PluginScanner.cpp");
  assert(impl.find("@ByteArray(") != std::string::npos);
  assert(impl.find("ParseSelectedProfileValue") != std::string::npos);
}

void TestPluginsTxtBomHandlingExists()
{
  const auto impl = ReadFile("helper/src/PluginScanner.cpp");
  assert(impl.find("StripUtf8BomInPlace") != std::string::npos);
  assert(impl.find("ParsePluginsTxt") != std::string::npos);
}

void TestTestDataExists()
{
  const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
  assert(root);
  const std::filesystem::path p = std::filesystem::path(root) / "tests" / "data" / "test_plugin_esl.bin";
  assert(std::filesystem::exists(p) && "Test ESP binary must exist");
  assert(std::filesystem::file_size(p) >= 24);
}

}  // namespace

int main()
{
  TestPluginScannerHeaderExists();
  TestPluginScannerImplExists();
  TestMo2SelectedProfileByteArraySupportExists();
  TestPluginsTxtBomHandlingExists();
  TestTestDataExists();
  return 0;
}
