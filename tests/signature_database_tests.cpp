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

static void TestSignatureJsonExists()
{
  const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
  assert(root);
  const std::filesystem::path p = std::filesystem::path(root) / "dump_tool" / "data" / "crash_signatures.json";
  assert(std::filesystem::exists(p));

  const auto content = ReadFile("dump_tool/data/crash_signatures.json");
  assert(content.find("\"version\"") != std::string::npos);
  assert(content.find("\"signatures\"") != std::string::npos);
}

static void TestSignatureDatabaseApiExists()
{
  const auto header = ReadFile("dump_tool/src/SignatureDatabase.h");
  assert(header.find("SignatureDatabase") != std::string::npos);
  assert(header.find("LoadFromJson") != std::string::npos);
  assert(header.find("Match") != std::string::npos);
  assert(header.find("SignatureMatch") != std::string::npos);
}

static void TestKnownSignaturePresent()
{
  const auto content = ReadFile("dump_tool/data/crash_signatures.json");
  assert(content.find("D6DDDA_VRAM") != std::string::npos);
}

static void TestBilingualFieldsPresent()
{
  const auto content = ReadFile("dump_tool/data/crash_signatures.json");
  assert(content.find("cause_ko") != std::string::npos);
  assert(content.find("cause_en") != std::string::npos);
  assert(content.find("recommendations_ko") != std::string::npos);
  assert(content.find("recommendations_en") != std::string::npos);
}

static void TestAnalyzerUsesRealCallstackForSignatureInput()
{
  const auto analyzer = ReadFile("dump_tool/src/Analyzer.cpp");
  assert(analyzer.find("out.stackwalk_primary_frames") != std::string::npos);
  assert(analyzer.find("input.callstack_modules.push_back(frame)") != std::string::npos);
}

int main()
{
  TestSignatureJsonExists();
  TestSignatureDatabaseApiExists();
  TestKnownSignaturePresent();
  TestBilingualFieldsPresent();
  TestAnalyzerUsesRealCallstackForSignatureInput();
  return 0;
}
