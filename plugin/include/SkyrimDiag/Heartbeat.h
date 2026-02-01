#pragma once

#include <cstdint>

namespace skydiag::plugin {

struct HeartbeatConfig
{
  std::uint32_t intervalMs = 100;

  // Best-effort performance signal:
  // If the scheduled main-thread update runs late by >= threshold, record a "PerfHitch" event.
  bool enableHitchLog = true;
  std::uint32_t hitchThresholdMs = 250;
  std::uint32_t hitchCooldownMs = 3000;
};

bool StartHeartbeatScheduler(const HeartbeatConfig& cfg);
void HeartbeatOnInputLoaded() noexcept;

}  // namespace skydiag::plugin
