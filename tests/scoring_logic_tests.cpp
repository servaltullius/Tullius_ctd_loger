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
  assert(in && "Failed to open source file");
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static void TestCallstackFrameWeightConstants()
{
  const auto src = ReadFile("dump_tool/src/AnalyzerInternalsStackwalkScoring.cpp");
  assert(src.find("return 16") != std::string::npos);
  assert(src.find("return 12") != std::string::npos);
  assert(src.find("return 8") != std::string::npos);
}

static void TestStackwalkConfidenceThresholds()
{
  const auto src = ReadFile("dump_tool/src/AnalyzerInternalsStackwalkScoring.cpp");
  assert(src.find("firstDepth <= 2") != std::string::npos);
  assert(src.find("topScore >= 24u") != std::string::npos);
  assert(src.find("firstDepth <= 6") != std::string::npos);
  assert(src.find("topScore >= 12u") != std::string::npos);
}

static void TestStackwalkHookPromotionThreshold()
{
  const auto src = ReadFile("dump_tool/src/AnalyzerInternalsStackwalkScoring.cpp");
  assert(src.find("score + 4u") != std::string::npos);
}

static void TestStackScanConfidenceThresholds()
{
  const auto src = ReadFile("dump_tool/src/AnalyzerInternalsStackScan.cpp");
  assert(src.find("256u") != std::string::npos);
  assert(src.find("96u") != std::string::npos);
  assert(src.find("40u") != std::string::npos);
}

static void TestStackScanHookPromotionThreshold()
{
  const auto src = ReadFile("dump_tool/src/AnalyzerInternalsStackScan.cpp");
  assert(src.find("score + 8u") != std::string::npos);
}

static void TestConfidenceDowngradePresent()
{
  const auto stackwalk = ReadFile("dump_tool/src/AnalyzerInternalsStackwalkScoring.cpp");
  assert(stackwalk.find("kHigh") != std::string::npos);
  assert(stackwalk.find("kMedium") != std::string::npos);
  assert(stackwalk.find("is_known_hook_framework") != std::string::npos);

  const auto stackscan = ReadFile("dump_tool/src/AnalyzerInternalsStackScan.cpp");
  assert(stackscan.find("kHigh") != std::string::npos);
  assert(stackscan.find("kMedium") != std::string::npos);
  assert(stackscan.find("is_known_hook_framework") != std::string::npos);
}

int main()
{
  TestCallstackFrameWeightConstants();
  TestStackwalkConfidenceThresholds();
  TestStackwalkHookPromotionThreshold();
  TestStackScanConfidenceThresholds();
  TestStackScanHookPromotionThreshold();
  TestConfidenceDowngradePresent();
  return 0;
}
