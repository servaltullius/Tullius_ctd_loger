#include "SkyrimDiagHelper/HangDetect.h"

#include "SkyrimDiagShared.h"

namespace skydiag::helper {

HangDecision EvaluateHang(
  std::uint64_t nowQpc,
  std::uint64_t lastHeartbeatQpc,
  std::uint64_t qpcFreq,
  std::uint32_t stateFlags,
  std::uint32_t thresholdInGameSec,
  std::uint32_t thresholdLoadingSec)
{
  HangDecision d{};

  if (qpcFreq == 0) {
    return d;
  }

  d.isLoading = (stateFlags & skydiag::kState_Loading) != 0u;
  d.thresholdSec = d.isLoading ? thresholdLoadingSec : thresholdInGameSec;

  if (nowQpc >= lastHeartbeatQpc) {
    const auto delta = nowQpc - lastHeartbeatQpc;
    d.secondsSinceHeartbeat = static_cast<double>(delta) / static_cast<double>(qpcFreq);
  }

  d.isHang = d.thresholdSec > 0 && d.secondsSinceHeartbeat >= static_cast<double>(d.thresholdSec);
  return d;
}

}  // namespace skydiag::helper

