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

}  // namespace skydiag::helper

