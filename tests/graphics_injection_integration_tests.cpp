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

void TestAnalysisResultHasGraphicsFields()
{
  const auto header = ReadFile("dump_tool/src/Analyzer.h");
  assert(header.find("GraphicsEnvironment") != std::string::npos);
  assert(header.find("GraphicsDiagResult") != std::string::npos);
  assert(header.find("graphics_env") != std::string::npos);
  assert(header.find("graphics_diag") != std::string::npos);
}

void TestAnalyzerCallsGraphicsDiag()
{
  const auto impl = ReadFile("dump_tool/src/Analyzer.cpp");
  assert(impl.find("GraphicsInjectionDiag") != std::string::npos);
  assert(impl.find("DetectEnvironment") != std::string::npos);
  assert(impl.find("graphics_injection_rules.json") != std::string::npos);
}

void TestEvidenceUsesGraphicsDiag()
{
  const auto impl = ReadFile("dump_tool/src/EvidenceBuilderInternalsEvidence.cpp");
  assert(impl.find("graphics_diag") != std::string::npos);
}

void TestRecommendationsUseGraphicsDiag()
{
  const auto impl = ReadFile("dump_tool/src/EvidenceBuilderInternalsRecommendations.cpp");
  assert(impl.find("graphics_diag") != std::string::npos);
}

void TestOutputWriterHasGraphicsFields()
{
  const auto impl = ReadFile("dump_tool/src/OutputWriter.cpp");
  assert(impl.find("\"graphics_environment\"") != std::string::npos);
  assert(impl.find("\"graphics_diagnosis\"") != std::string::npos);
}

}  // namespace

int main()
{
  TestAnalysisResultHasGraphicsFields();
  TestAnalyzerCallsGraphicsDiag();
  TestEvidenceUsesGraphicsDiag();
  TestRecommendationsUseGraphicsDiag();
  TestOutputWriterHasGraphicsFields();
  return 0;
}
