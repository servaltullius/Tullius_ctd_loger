#pragma once

#include "SkyrimDiagHelper/Config.h"

namespace skydiag::helper {

// Policy helper to avoid duplicate analysis runs.
//
// - If a viewer window will open right now, prefer doing analysis in the viewer
//   (single analysis path, less confusion).
// - If analysis is required for internal workflows (e.g. crash recapture), run it
//   even if the viewer will open.
inline bool ShouldRunHeadlessDumpAnalysis(
  const HelperConfig& cfg,
  bool viewerWillOpenNow,
  bool analysisRequired)
{
  if (!cfg.autoAnalyzeDump) {
    return false;
  }
  if (analysisRequired) {
    return true;
  }
  return !viewerWillOpenNow;
}

// Under Wine/Proton the WinUI viewer fails during startup (Windows App Runtime
// needs WinRT pieces Wine does not provide) but its process usually lives past
// the launch check, so the helper would treat it as opened and skip headless
// analysis, leaving a dump without any report. Turn viewer auto-open off so
// every capture takes the headless path. Returns true when a setting changed.
inline bool ApplyWineViewerPolicy(HelperConfig* cfg, bool runningUnderWine)
{
  if (!cfg || !runningUnderWine || cfg->autoOpenViewerUnderWine) {
    return false;
  }
  const bool changed =
    cfg->autoOpenViewerOnCrash || cfg->autoOpenViewerOnHang || cfg->autoOpenViewerOnManualCapture;
  cfg->autoOpenViewerOnCrash = false;
  cfg->autoOpenViewerOnHang = false;
  cfg->autoOpenViewerOnManualCapture = false;
  return changed;
}

}  // namespace skydiag::helper

