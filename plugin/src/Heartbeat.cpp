#include "SkyrimDiag/Heartbeat.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <stop_token>
#include <thread>

#include <SKSE/SKSE.h>

#include "SkyrimDiag/Blackbox.h"
#include "SkyrimDiag/SharedMemory.h"

namespace skydiag::plugin {
namespace {

std::atomic_bool g_running{ false };
std::atomic_uint32_t g_intervalMs{ 100 };

std::atomic_bool g_hitchEnabled{ false };
std::atomic_uint32_t g_hitchThresholdMs{ 250 };
std::atomic_uint32_t g_hitchCooldownMs{ 3000 };

std::atomic_bool g_taskPending{ false };
std::atomic_uint64_t g_taskEnqueueQpc{ 0 };

std::atomic_bool g_schedulerStarted{ false };
std::jthread g_scheduler;

inline void HeartbeatTaskOnMainThread() noexcept
{
  auto* shm = GetShared();
  if (!shm) {
    g_taskPending.store(false);
    return;
  }

  LARGE_INTEGER li{};
  QueryPerformanceCounter(&li);
  const std::uint64_t now = static_cast<std::uint64_t>(li.QuadPart);
  shm->header.last_heartbeat_qpc = now;

  static std::uint64_t lastLoggedQpc = 0;

  const bool enabled = g_hitchEnabled.load();
  const std::uint32_t thresholdMs = g_hitchThresholdMs.load();
  const std::uint32_t cooldownMs = g_hitchCooldownMs.load();

  const std::uint64_t enq = g_taskEnqueueQpc.load();
  if (enabled && thresholdMs > 0 && enq != 0 && shm->header.qpc_freq != 0 && now >= enq) {
    const std::uint64_t deltaQpc = now - enq;
    const double deltaMs = 1000.0 * (static_cast<double>(deltaQpc) / static_cast<double>(shm->header.qpc_freq));
    if (deltaMs >= static_cast<double>(thresholdMs)) {
      bool cooldownOk = true;
      if (cooldownMs > 0 && lastLoggedQpc != 0) {
        const std::uint64_t needQpc = (static_cast<std::uint64_t>(cooldownMs) * shm->header.qpc_freq) / 1000ull;
        cooldownOk = (now >= lastLoggedQpc + needQpc);
      }

      if (cooldownOk) {
        lastLoggedQpc = now;
        skydiag::EventPayload p{};
        p.a = static_cast<std::uint64_t>(deltaMs + 0.5);  // ms (rounded)
        p.b = shm->header.state_flags;
        p.c = static_cast<std::uint64_t>(g_intervalMs.load());
        PushEvent(skydiag::EventType::kPerfHitch, p, sizeof(p));
      }
    }
  }

  g_taskPending.store(false);
}

void QueueHeartbeatTask() noexcept
{
  if (!g_running.load()) {
    return;
  }

  bool expected = false;
  if (!g_taskPending.compare_exchange_strong(expected, true)) {
    return;  // already queued
  }

  LARGE_INTEGER li{};
  QueryPerformanceCounter(&li);
  g_taskEnqueueQpc.store(static_cast<std::uint64_t>(li.QuadPart));

  if (auto* ti = SKSE::GetTaskInterface()) {
    // Must run on the game/UI thread so a frozen main thread stops heartbeats.
    try {
      ti->AddUITask([]() { HeartbeatTaskOnMainThread(); });
    } catch (...) {
      // Never leave the scheduler stuck in pending=true if task enqueue throws.
      g_taskPending.store(false);
    }
  } else {
    g_taskPending.store(false);
  }
}

void SchedulerLoop(std::stop_token st)
{
  while (!st.stop_requested()) {
    const std::uint32_t intervalMs = g_intervalMs.load();
    if (!g_running.load() || intervalMs == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      continue;
    }

    QueueHeartbeatTask();
    std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
  }
}

}  // namespace

static void ApplyHeartbeatConfig(const HeartbeatConfig& cfg) noexcept
{
  g_intervalMs.store(cfg.intervalMs);
  g_hitchEnabled.store(cfg.enableHitchLog);
  g_hitchThresholdMs.store(cfg.hitchThresholdMs);
  g_hitchCooldownMs.store(cfg.hitchCooldownMs);
}

bool StartHeartbeatScheduler(const HeartbeatConfig& cfg)
{
  ApplyHeartbeatConfig(cfg);
  g_running.store(true);
  bool expected = false;
  if (g_schedulerStarted.compare_exchange_strong(expected, true)) {
    g_scheduler = std::jthread(SchedulerLoop);
  }
  QueueHeartbeatTask();  // kick once immediately
  return true;
}

void HeartbeatOnInputLoaded() noexcept
{
  // Heartbeat is scheduled via SKSE tasks; nothing to do.
}

}  // namespace skydiag::plugin
