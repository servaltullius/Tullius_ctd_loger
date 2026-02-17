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

void TestHeaderApiExists()
{
  const auto header = ReadFile("dump_tool/src/GraphicsInjectionDiag.h");
  assert(header.find("GraphicsInjectionDiag") != std::string::npos);
  assert(header.find("GraphicsEnvironment") != std::string::npos);
  assert(header.find("GraphicsDiagResult") != std::string::npos);
  assert(header.find("LoadRules") != std::string::npos);
  assert(header.find("DetectEnvironment") != std::string::npos);
  assert(header.find("Diagnose") != std::string::npos);
}

void TestImplExists()
{
  const auto impl = ReadFile("dump_tool/src/GraphicsInjectionDiag.cpp");
  assert(impl.find("LoadRules") != std::string::npos);
  assert(impl.find("DetectEnvironment") != std::string::npos);
  assert(impl.find("Diagnose") != std::string::npos);
  assert(impl.find("nlohmann") != std::string::npos);
}

void TestImplUsesLowerCaseComparison()
{
  const auto impl = ReadFile("dump_tool/src/GraphicsInjectionDiag.cpp");
  assert(
    (impl.find("WideLower") != std::string::npos || impl.find("towlower") != std::string::npos) &&
    "Must use case-insensitive module comparison");
}

}  // namespace

int main()
{
  TestHeaderApiExists();
  TestImplExists();
  TestImplUsesLowerCaseComparison();
  return 0;
}
