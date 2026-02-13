#pragma once

#include <cstdint>

namespace skydiag::helper {

struct HangSuppressionState {
  // When we suppress a hang while not-foreground, we remember the last heartbeat qpc that we saw.
  // If the game returns to foreground but the heartbeat has not advanced yet, we treat it as a likely
  // background pause (Alt-Tab) and keep suppressing for a short grace period.
  std::uint64_t suppressedHeartbeatQpc = 0;
  std::uint64_t foregroundResumeQpc = 0;
};

enum class HangSuppressionReason : std::uint8_t {
  kNone = 0,
  kNotForeground = 1,
  kForegroundGrace = 2,
  kForegroundResponsive = 3,
};

struct HangSuppressionResult {
  bool suppress = false;
  HangSuppressionReason reason = HangSuppressionReason::kNone;
};

inline HangSuppressionResult EvaluateHangSuppression(
  HangSuppressionState& state,
  bool isHang,
  bool isForeground,
  bool isLoading,
  bool isWindowResponsive,
  bool suppressHangWhenNotForeground,
  std::uint64_t nowQpc,
  std::uint64_t heartbeatQpc,
  std::uint64_t qpcFreq,
  std::uint32_t foregroundGraceSec)
{
  if (!isHang) {
    state = {};
    return {};
  }

  if (suppressHangWhenNotForeground && !isForeground) {
    // If the window is unresponsive while not foreground, treat as a real
    // freeze (user Alt-Tabbed away from a frozen game) and allow capture.
    if (!isWindowResponsive) {
      return {};
    }
    // Window is responsive but not foreground — likely normal Alt-Tab pause.
    state.suppressedHeartbeatQpc = heartbeatQpc;
    state.foregroundResumeQpc = 0;
    return { true, HangSuppressionReason::kNotForeground };
  }

  if (state.suppressedHeartbeatQpc == 0) {
    return {};
  }

  if (heartbeatQpc > state.suppressedHeartbeatQpc) {
    state = {};
    return {};
  }

  // Heartbeat has not advanced since the background suppression started.
  // If we're back in foreground, apply a grace period before treating it as an actionable hang.
  if (!isForeground) {
    return {};
  }

  if (foregroundGraceSec == 0 || qpcFreq == 0) {
    return {};
  }

  if (state.foregroundResumeQpc == 0) {
    state.foregroundResumeQpc = nowQpc;
  }

  const std::uint64_t deltaQpc = (nowQpc > state.foregroundResumeQpc) ? (nowQpc - state.foregroundResumeQpc) : 0;
  const double secondsSinceForeground = static_cast<double>(deltaQpc) / static_cast<double>(qpcFreq);
  if (secondsSinceForeground < static_cast<double>(foregroundGraceSec)) {
    return { true, HangSuppressionReason::kForegroundGrace };
  }

  // After the grace period, if the window is still responsive and we're not in a loading screen,
  // keep suppressing to avoid false "hang" dumps caused by background pauses (Alt-Tab).
  if (!isLoading && isWindowResponsive) {
    return { true, HangSuppressionReason::kForegroundResponsive };
  }

  return {};
}

}  // namespace skydiag::helper
