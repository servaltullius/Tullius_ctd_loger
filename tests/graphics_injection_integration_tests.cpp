#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "SourceGuardTestUtils.h"

namespace {
using skydiag::tests::source_guard::ReadProjectText;

void TestAnalysisResultHasGraphicsFields()
{
  const auto header = ReadProjectText("dump_tool/src/Analyzer.h");
  assert(header.find("GraphicsEnvironment") != std::string::npos);
  assert(header.find("GraphicsDiagResult") != std::string::npos);
  assert(header.find("graphics_env") != std::string::npos);
  assert(header.find("graphics_diag") != std::string::npos);
}

void TestAnalyzerCallsGraphicsDiag()
{
  const auto impl = ReadProjectText("dump_tool/src/Analyzer.cpp");
  assert(impl.find("GraphicsInjectionDiag") != std::string::npos);
  assert(impl.find("DetectEnvironment") != std::string::npos);
  assert(impl.find("graphics_injection_rules.json") != std::string::npos);
  // ENB/ReShade/DXVK DLLs share their names with Windows DLLs the game always
  // loads; only copies from outside System32 count (every report claimed ENB
  // and ReShade before, 2026-10-14).
  assert(impl.find("!IsLikelyWindowsSystemModulePath(m.path)") != std::string::npos);
  assert(impl.find("faultInWindows ? std::wstring{} : out.fault_module_filename") != std::string::npos);
}

void TestEvidenceUsesGraphicsDiag()
{
  const auto impl = ReadProjectText("dump_tool/src/EvidenceBuilderEvidence.cpp");
  assert(impl.find("graphics_diag") != std::string::npos);
}

void TestRecommendationsUseGraphicsDiag()
{
  const auto impl = ReadProjectText("dump_tool/src/EvidenceBuilderRecommendations.cpp");
  assert(impl.find("graphics_diag") != std::string::npos);
}

void TestOutputWriterHasGraphicsFields()
{
  const auto impl = ReadProjectText("dump_tool/src/OutputWriter.cpp");
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
