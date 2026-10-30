#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "../dump_tool/src/AnalyzerScoringPolicy.h"

static std::string ReadFile(const char* relPath)
{
  const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
  assert(root && "SKYDIAG_PROJECT_ROOT must be set");
  const std::filesystem::path p = std::filesystem::path(root) / relPath;
  std::ifstream in(p, std::ios::in | std::ios::binary);
  assert(in && "Failed to open source file");
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The weights the stack walk gives a frame by its depth (called, not read
// from the source text: a text match passed with any constant changed).
static void TestCallstackFrameWeightConstants()
{
  using skydiag::dump_tool::internal::policy::CallstackFrameWeight;
  static_assert(CallstackFrameWeight(0) == 16);
  static_assert(CallstackFrameWeight(1) == 12);
  static_assert(CallstackFrameWeight(2) == 8);
  static_assert(CallstackFrameWeight(3) == 4 && CallstackFrameWeight(5) == 4);
  static_assert(CallstackFrameWeight(6) == 2 && CallstackFrameWeight(10) == 2);
  static_assert(CallstackFrameWeight(11) == 1 && CallstackFrameWeight(100) == 1);
  const auto src = ReadFile("dump_tool/src/AnalyzerInternalsStackwalkScoring.cpp");
  assert(src.find("CallstackFrameWeight(i)") != std::string::npos);
  assert(src.find("policy::TopSuspectCallstackConfidence(") != std::string::npos);
}

// High needs a suspect within the top three frames that leads the runner-up:
// by a strong score, or by a clear margin. A tie is Medium at most.
static void TestStackwalkConfidenceThresholds()
{
  using skydiag::dump_tool::internal::policy::TopSuspectCallstackConfidence;
  using Level = skydiag::dump_tool::i18n::ConfidenceLevel;
  static_assert(TopSuspectCallstackConfidence(28, 16, 0) == Level::kHigh);    // strong and ahead
  static_assert(TopSuspectCallstackConfidence(24, 24, 0) == Level::kMedium);  // strong but tied
  static_assert(TopSuspectCallstackConfidence(16, 4, 1) == Level::kHigh);     // a clear margin
  static_assert(TopSuspectCallstackConfidence(28, 16, 3) == Level::kMedium);  // too deep for High
  static_assert(TopSuspectCallstackConfidence(12, 12, 6) == Level::kMedium);
  static_assert(TopSuspectCallstackConfidence(8, 2, 6) == Level::kMedium);    // margin 6
  static_assert(TopSuspectCallstackConfidence(8, 4, 6) == Level::kLow);
  static_assert(TopSuspectCallstackConfidence(40, 0, 7) == Level::kLow);      // too deep for Medium
  static_assert(TopSuspectCallstackConfidence(0xFFFFFFFFu, 0xFFFFFFF0u, 0) == Level::kHigh);
}

static void TestStackwalkHookPromotionThreshold()
{
  const auto src = ReadFile("dump_tool/src/AnalyzerInternalsStackwalkScoring.cpp");
  assert(src.find("kHookFrameworkNearTieThreshold") != std::string::npos);
  assert(src.find("= 4") != std::string::npos);
  assert(src.find("kPassiveHookFallbackMinScore") != std::string::npos);
  assert(src.find("ShouldPromoteHookFallback") != std::string::npos);
}

static void TestStackScanConfidenceIsAlwaysLow()
{
  const auto src = ReadFile("dump_tool/src/AnalyzerInternalsStackScan.cpp");
  assert(src.find("si.confidence_level = i18n::ConfidenceLevel::kLow") != std::string::npos);
  assert(src.find("ConfidenceForTopSuspectLevel") == std::string::npos);
  assert(src.find("ConfidenceForSecondarySuspectLevel") == std::string::npos);
}

static void TestStackScanHookPromotionThreshold()
{
  const auto src = ReadFile("dump_tool/src/AnalyzerInternalsStackScan.cpp");
  assert(src.find("kHookFrameworkNearTieThreshold = 8u") != std::string::npos);
  assert(src.find("kPassiveHookFallbackMinScore") != std::string::npos);
  assert(src.find("ShouldPromoteHookFallback") != std::string::npos);
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
  assert(stackscan.find("si.confidence_level = i18n::ConfidenceLevel::kLow") != std::string::npos);
  assert(stackscan.find("ConfidenceForTopSuspectLevel") == std::string::npos);
}

static void TestCrashLoggerCorroborationRankingPresent()
{
  const auto analyzer = ReadFile("dump_tool/src/Analyzer.cpp");
  assert(analyzer.find("ApplyCrashLoggerCorroborationToSuspects") != std::string::npos);
  assert(analyzer.find("CrashLoggerRankBonus") != std::string::npos);
  assert(analyzer.find("Crash Logger frame promotion=+") != std::string::npos);
  assert(analyzer.find("ApplyCrashLoggerCorroborationToSuspects(&out, allModules)") != std::string::npos);
  assert(analyzer.find("out->suspects[0].confidence_level = i18n::ConfidenceLevel::kHigh") == std::string::npos);
  assert(analyzer.find("ConfidenceAfterCrashLoggerCorroboration") != std::string::npos);
}

static void TestCrashLoggerConfidencePolicyFlow()
{
  using skydiag::dump_tool::i18n::ConfidenceLevel;
  using skydiag::dump_tool::internal::policy::ConfidenceAfterCrashLoggerCorroboration;

  constexpr bool directFaultSupport = true;
  assert(ConfidenceAfterCrashLoggerCorroboration(
           ConfidenceLevel::kLow,
           directFaultSupport,
           /*suspectsFromStackwalk=*/false) == ConfidenceLevel::kLow);
  assert(ConfidenceAfterCrashLoggerCorroboration(
           ConfidenceLevel::kHigh,
           directFaultSupport,
           /*suspectsFromStackwalk=*/true) == ConfidenceLevel::kHigh);
  assert(ConfidenceAfterCrashLoggerCorroboration(
           ConfidenceLevel::kLow,
           directFaultSupport,
           /*suspectsFromStackwalk=*/true) == ConfidenceLevel::kMedium);
  assert(ConfidenceAfterCrashLoggerCorroboration(
           ConfidenceLevel::kUnknown,
           directFaultSupport,
           /*suspectsFromStackwalk=*/true) == ConfidenceLevel::kMedium);
  assert(ConfidenceAfterCrashLoggerCorroboration(
           ConfidenceLevel::kLow,
           /*hasCrashLoggerSupport=*/false,
           /*suspectsFromStackwalk=*/true) == ConfidenceLevel::kLow);
}

static void TestPointerScanConfidenceClampRunsAfterCorroboration()
{
  const auto analyzer = ReadFile("dump_tool/src/Analyzer.cpp");
  const auto applyPos = analyzer.find("ApplyCrashLoggerCorroborationToSuspects(&out, allModules);");
  assert(applyPos != std::string::npos);
  const auto clampPos = analyzer.find("CapPointerScanSuspectConfidence(out);", applyPos);
  assert(clampPos != std::string::npos);
  const auto clampFn = analyzer.find("void CapPointerScanSuspectConfidence(AnalysisResult& out)");
  assert(clampFn != std::string::npos);
  const auto walkPos = analyzer.find("if (out.suspects_from_stackwalk)", clampFn);
  assert(walkPos != std::string::npos);
  const auto lowPos = analyzer.find(
    "suspect.confidence_level = i18n::ConfidenceLevel::kLow",
    walkPos);
  assert(lowPos != std::string::npos);
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

static void TestCaptureQualityDoesNotBecomeCausalSupport()
{
  const auto candidates = ReadFile("dump_tool/src/EvidenceBuilderCandidates.cpp");
  const auto consensus = ReadFile("dump_tool/src/CandidateConsensus.cpp");

  assert(candidates.find("capture_quality_stack") == std::string::npos);
  assert(consensus.find("capture_quality_stack") != std::string::npos);
  assert(consensus.find("signal.family_id == kFamilyCaptureQualityStack") != std::string::npos);
}

static void TestSecondaryConfidenceIsEvidenceGated()
{
  const auto stackwalk = ReadFile("dump_tool/src/AnalyzerInternalsStackwalkScoring.cpp");
  const auto stackscan = ReadFile("dump_tool/src/AnalyzerInternalsStackScan.cpp");
  assert(stackwalk.find("ConfidenceForSecondaryCallstackLevel") != std::string::npos);
  assert(stackwalk.find("SecondaryCallstackCanBeMedium") != std::string::npos);
  assert(stackscan.find("ConfidenceForSecondarySuspectLevel") == std::string::npos);
  assert(stackscan.find("si.confidence_level = i18n::ConfidenceLevel::kLow") != std::string::npos);
}

static void TestLowStackwalkCannotBeReupgradedByConsensusWeight()
{
  using skydiag::dump_tool::internal::policy::ActionableStackSignalWeight;
  assert(ActionableStackSignalWeight(true, false, false, true) == 3u);
  assert(ActionableStackSignalWeight(true, false, false, true) < 5u);
  assert(ActionableStackSignalWeight(true, false, true, true) == 5u);
  assert(ActionableStackSignalWeight(true, true, true, true) == 3u);
  assert(ActionableStackSignalWeight(true, false, true, false) == 4u);
  assert(ActionableStackSignalWeight(false, false, true, true) == 2u);
}

int main()
{
  TestCallstackFrameWeightConstants();
  TestStackwalkConfidenceThresholds();
  TestStackwalkHookPromotionThreshold();
  TestStackScanConfidenceIsAlwaysLow();
  TestStackScanHookPromotionThreshold();
  TestResourceProviderScoreOnlyTuningPresent();
  TestConfidenceDowngradePresent();
  TestCrashLoggerCorroborationRankingPresent();
  TestCrashLoggerConfidencePolicyFlow();
  TestPointerScanConfidenceClampRunsAfterCorroboration();
  TestCrashLoggerPromotionUsesFrameSignals();
  TestCrashLoggerPromotionNoLongerRankOnly();
  TestCaptureQualityDoesNotBecomeCausalSupport();
  TestSecondaryConfidenceIsEvidenceGated();
  TestLowStackwalkCannotBeReupgradedByConsensusWeight();
  return 0;
}
