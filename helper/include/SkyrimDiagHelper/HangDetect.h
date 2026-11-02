#pragma once

#include <cstdint>

namespace skydiag::helper {

struct HelperConfig;

struct HangDecision {
  bool isHang = false;
  double secondsSinceHeartbeat = 0.0;
  std::uint32_t thresholdSec = 0;
  bool isLoading = false;
};

HangDecision EvaluateHang(
  std::uint64_t nowQpc,
  std::uint64_t lastHeartbeatQpc,
  std::uint64_t qpcFreq,
  std::uint32_t stateFlags,
  std::uint32_t thresholdInGameSec,
  std::uint32_t thresholdLoadingSec);

struct HangThresholds {
  std::uint32_t inGameSec = 0;
  std::uint32_t loadingSec = 0;
};

// The thresholds EvaluateHang takes for these state flags: an open menu raises
// the in-game one to the menu threshold, and the loading one is the adaptive
// threshold when that is enabled and the helper has loading samples.
HangThresholds ComputeHangThresholds(
  const HelperConfig& cfg,
  std::uint32_t stateFlags,
  bool haveLoadingSamples,
  std::uint32_t adaptiveLoadingThresholdSec);

}  // namespace skydiag::helper

