#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::string ReadFile(const char* relPath)
{
  const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
  assert(root && "SKYDIAG_PROJECT_ROOT must be set");
  std::filesystem::path p = std::filesystem::path(root) / relPath;
  std::ifstream f(p);
  assert(f.is_open());
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void TestJsonFileExists()
{
  const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
  assert(root);
  std::filesystem::path p = std::filesystem::path(root) / "dump_tool" / "data" / "graphics_injection_rules.json";
  assert(std::filesystem::exists(p) && "graphics_injection_rules.json must exist");
}

void TestHasRequiredStructure()
{
  const auto content = ReadFile("dump_tool/data/graphics_injection_rules.json");
  assert(content.find("\"version\"") != std::string::npos);
  assert(content.find("\"detection_modules\"") != std::string::npos);
  assert(content.find("\"rules\"") != std::string::npos);
}

void TestHasEnbAndReshadeDetection()
{
  const auto content = ReadFile("dump_tool/data/graphics_injection_rules.json");
  assert(content.find("\"enb\"") != std::string::npos);
  assert(content.find("\"reshade\"") != std::string::npos);
  assert(content.find("d3d11.dll") != std::string::npos);
  assert(content.find("d3dcompiler_46e.dll") != std::string::npos);
}

void TestRulesHaveBilingualFields()
{
  const auto content = ReadFile("dump_tool/data/graphics_injection_rules.json");
  assert(content.find("\"cause_ko\"") != std::string::npos);
  assert(content.find("\"cause_en\"") != std::string::npos);
  assert(content.find("\"recommendations_ko\"") != std::string::npos);
  assert(content.find("\"recommendations_en\"") != std::string::npos);
}

void TestRulesRequireFaultModule()
{
  const auto content = ReadFile("dump_tool/data/graphics_injection_rules.json");
  assert(content.find("\"fault_module_any\"") != std::string::npos);
}

}  // namespace

int main()
{
  TestJsonFileExists();
  TestHasRequiredStructure();
  TestHasEnbAndReshadeDetection();
  TestRulesHaveBilingualFields();
  TestRulesRequireFaultModule();
  return 0;
}
