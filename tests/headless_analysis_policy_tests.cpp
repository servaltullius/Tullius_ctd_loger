#include "SkyrimDiagHelper/HeadlessAnalysisPolicy.h"

#include "SkyrimDiagHelper/Config.h"

#include <cassert>

using skydiag::helper::HelperConfig;
using skydiag::helper::ShouldRunHeadlessDumpAnalysis;

static void Test_AutoAnalyzeDisabled_NeverRuns()
{
  HelperConfig cfg{};
  cfg.autoAnalyzeDump = false;
  assert(!ShouldRunHeadlessDumpAnalysis(cfg, /*viewerWillOpenNow=*/false, /*analysisRequired=*/false));
  assert(!ShouldRunHeadlessDumpAnalysis(cfg, /*viewerWillOpenNow=*/true, /*analysisRequired=*/true));
}

static void Test_ViewerWillOpen_SkipsHeadlessByDefault()
{
  HelperConfig cfg{};
  cfg.autoAnalyzeDump = true;
  assert(!ShouldRunHeadlessDumpAnalysis(cfg, /*viewerWillOpenNow=*/true, /*analysisRequired=*/false));
}

static void Test_NoViewer_RunsHeadless()
{
  HelperConfig cfg{};
  cfg.autoAnalyzeDump = true;
  assert(ShouldRunHeadlessDumpAnalysis(cfg, /*viewerWillOpenNow=*/false, /*analysisRequired=*/false));
}

static void Test_AnalysisRequired_OverridesViewerSkip()
{
  HelperConfig cfg{};
  cfg.autoAnalyzeDump = true;
  assert(ShouldRunHeadlessDumpAnalysis(cfg, /*viewerWillOpenNow=*/true, /*analysisRequired=*/true));
}

int main()
{
  Test_AutoAnalyzeDisabled_NeverRuns();
  Test_ViewerWillOpen_SkipsHeadlessByDefault();
  Test_NoViewer_RunsHeadless();
  Test_AnalysisRequired_OverridesViewerSkip();
  return 0;
}

