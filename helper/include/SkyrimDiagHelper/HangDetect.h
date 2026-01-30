#pragma once

#include <cstdint>

namespace skydiag::helper {

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

}  // namespace skydiag::helper

