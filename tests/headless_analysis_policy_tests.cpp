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

static void Test_Wine_DisablesViewerAutoOpenSoCapturesGetReports()
{
  HelperConfig cfg{};
  cfg.autoAnalyzeDump = true;
  cfg.autoOpenViewerOnCrash = true;
  cfg.autoOpenViewerOnHang = true;
  cfg.autoOpenViewerOnManualCapture = true;
  assert(skydiag::helper::ApplyWineViewerPolicy(&cfg, /*runningUnderWine=*/true));
  assert(!cfg.autoOpenViewerOnCrash);
  assert(!cfg.autoOpenViewerOnHang);
  assert(!cfg.autoOpenViewerOnManualCapture);
  // With no viewer opening, every capture takes the headless report path.
  assert(ShouldRunHeadlessDumpAnalysis(cfg, /*viewerWillOpenNow=*/false, /*analysisRequired=*/false));

  // Already off: nothing to change.
  assert(!skydiag::helper::ApplyWineViewerPolicy(&cfg, /*runningUnderWine=*/true));
}

static void Test_Windows_And_WineOverride_KeepViewerSettings()
{
  HelperConfig windows{};
  windows.autoOpenViewerOnCrash = true;
  windows.autoOpenViewerOnHang = true;
  assert(!skydiag::helper::ApplyWineViewerPolicy(&windows, /*runningUnderWine=*/false));
  assert(windows.autoOpenViewerOnCrash && windows.autoOpenViewerOnHang);

  HelperConfig overridden{};
  overridden.autoOpenViewerOnCrash = true;
  overridden.autoOpenViewerUnderWine = true;
  assert(!skydiag::helper::ApplyWineViewerPolicy(&overridden, /*runningUnderWine=*/true));
  assert(overridden.autoOpenViewerOnCrash);

  assert(!skydiag::helper::ApplyWineViewerPolicy(nullptr, /*runningUnderWine=*/true));
}

int main()
{
  Test_Wine_DisablesViewerAutoOpenSoCapturesGetReports();
  Test_Windows_And_WineOverride_KeepViewerSettings();
  Test_AutoAnalyzeDisabled_NeverRuns();
  Test_ViewerWillOpen_SkipsHeadlessByDefault();
  Test_NoViewer_RunsHeadless();
  Test_AnalysisRequired_OverridesViewerSkip();
  return 0;
}

