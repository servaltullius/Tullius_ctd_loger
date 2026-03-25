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
  // Named constants: kWeightDepth0=16, kWeightDepth1=12, kWeightDepth2=8
  assert(src.find("kWeightDepth0") != std::string::npos);
  assert(src.find("kWeightDepth1") != std::string::npos);
  assert(src.find("kWeightDepth2") != std::string::npos);
  assert(src.find("= 16") != std::string::npos);
  assert(src.find("= 12") != std::string::npos);
  assert(src.find("= 8") != std::string::npos);
}

static void TestStackwalkConfidenceThresholds()
{
  const auto src = ReadFile("dump_tool/src/AnalyzerInternalsStackwalkScoring.cpp");
  // Named constants for confidence thresholds
  assert(src.find("kHighConfMaxDepth") != std::string::npos);
  assert(src.find("kHighConfMinScore") != std::string::npos);
  assert(src.find("kMedConfMaxDepth") != std::string::npos);
  assert(src.find("kMedConfMinScore") != std::string::npos);
  // Values
  assert(src.find("= 2") != std::string::npos);   // kHighConfMaxDepth
  assert(src.find("= 24") != std::string::npos);   // kHighConfMinScore
  assert(src.find("= 6") != std::string::npos);    // kMedConfMaxDepth or kMedConfMinMargin
  assert(src.find("= 12") != std::string::npos);   // kMedConfMinScore or kHighConfMinMargin
}

static void TestStackwalkHookPromotionThreshold()
{
  const auto src = ReadFile("dump_tool/src/AnalyzerInternalsStackwalkScoring.cpp");
  assert(src.find("kHookFrameworkNearTieThreshold") != std::string::npos);
  assert(src.find("= 4") != std::string::npos);
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

static void TestResourceProviderScoreOnlyTuningPresent()
{
  const auto src = ReadFile("dump_tool/src/EvidenceBuilderCandidates.cpp");
  assert(src.find("signal.weight = (row.hitCount >= 2u && row.bestDistanceMs <= 150.0) ? 5u") != std::string::npos);
  assert(src.find("((row.bestDistanceMs <= 300.0) || row.hitCount >= 2u) ? 4u : 3u") != std::string::npos);
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

static void TestCrashLoggerCorroborationRankingPresent()
{
  const auto analyzer = ReadFile("dump_tool/src/Analyzer.cpp");
  assert(analyzer.find("ApplyCrashLoggerCorroborationToSuspects") != std::string::npos);
  assert(analyzer.find("CrashLoggerRankBonus") != std::string::npos);
  assert(analyzer.find("Crash Logger frame promotion=+") != std::string::npos);
  assert(analyzer.find("ApplyCrashLoggerCorroborationToSuspects(&out)") != std::string::npos);
}

static void TestCrashLoggerPromotionUsesFrameSignals()
{
  const auto header = ReadFile("dump_tool/src/Analyzer.h");
  const auto impl = ReadFile("dump_tool/src/Analyzer.cpp");

  assert(header.find("crash_logger_direct_fault_module") != std::string::npos);
  assert(header.find("crash_logger_first_actionable_probable_module") != std::string::npos);
  assert(header.find("crash_logger_probable_streak_module") != std::string::npos);
  assert(header.find("crash_logger_probable_streak_length") != std::string::npos);
  assert(header.find("crash_logger_frame_signal_strength") != std::string::npos);

  assert(impl.find("bool CanPromoteCrashLoggerFrameModule(std::wstring_view module)") != std::string::npos);
  assert(impl.find("struct CrashLoggerFramePromotion") != std::string::npos);
  assert(impl.find("promotion.directFaultPromotion = directFaultMatches ? kCrashLoggerDirectFaultPromotion : 0u;") != std::string::npos);
  assert(impl.find("promotion.firstActionablePromotion = firstActionableMatches ? kCrashLoggerFirstActionablePromotion : 0u;") != std::string::npos);
  assert(impl.find("promotion.probableStreakPromotion = probableStreakMatches ? kCrashLoggerProbableStreakPromotion : 0u;") != std::string::npos);
  assert(impl.find("promotion.cppExceptionSupport = cppModuleMatches ? kCrashLoggerCppExceptionSupport : 0u;") != std::string::npos);
}

static void TestCrashLoggerPromotionNoLongerRankOnly()
{
  const auto analyzer = ReadFile("dump_tool/src/Analyzer.cpp");
  assert(analyzer.find("promotion.topModuleRankBonus = matchedRank ? CrashLoggerRankBonus(*matchedRank) : 0u;") != std::string::npos);
  assert(analyzer.find("promotion.frameSignalStrength = promotion.directFaultPromotion +") != std::string::npos);
  assert(analyzer.find("promotion.totalPromotion = promotion.frameSignalStrength + promotion.cppExceptionSupport + promotion.topModuleRankBonus;") != std::string::npos);
  assert(analyzer.find("row.score + row.promotion.totalPromotion") != std::string::npos);
  assert(analyzer.find("out->crash_logger_frame_signal_strength") != std::string::npos);
}

static void TestCaptureQualityStackSupportPresent()
{
  const auto candidates = ReadFile("dump_tool/src/EvidenceBuilderCandidates.cpp");
  const auto consensus = ReadFile("dump_tool/src/CandidateConsensus.cpp");

  assert(candidates.find("capture_quality_stack") != std::string::npos);
  assert(candidates.find("incident_capture_profile_process_thread_data") != std::string::npos);
  assert(candidates.find("incident_capture_profile_full_memory_info") != std::string::npos);
  assert(candidates.find("incident_capture_profile_module_headers") != std::string::npos);
  assert(candidates.find("incident_capture_profile_indirect_memory") != std::string::npos);
  assert(consensus.find("capture_quality_stack") != std::string::npos);
}

int main()
{
  TestCallstackFrameWeightConstants();
  TestStackwalkConfidenceThresholds();
  TestStackwalkHookPromotionThreshold();
  TestStackScanConfidenceThresholds();
  TestStackScanHookPromotionThreshold();
  TestResourceProviderScoreOnlyTuningPresent();
  TestConfidenceDowngradePresent();
  TestCrashLoggerCorroborationRankingPresent();
  TestCrashLoggerPromotionUsesFrameSignals();
  TestCrashLoggerPromotionNoLongerRankOnly();
  TestCaptureQualityStackSupportPresent();
  return 0;
}
