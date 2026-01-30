#include "SkyrimDiag/Heartbeat.h"

#include <Windows.h>

#include <atomic>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "SkyrimDiag/Blackbox.h"
#include "SkyrimDiag/SharedMemory.h"

namespace skydiag::plugin {
namespace {

std::atomic_bool g_running{ false };
std::atomic_bool g_hitchEnabled{ false };
std::atomic_uint32_t g_hitchThresholdMs{ 250 };
std::atomic_uint32_t g_hitchCooldownMs{ 3000 };
std::atomic_bool g_inputSinkRegistered{ false };

inline void UpdateHeartbeatOnMainThread() noexcept
{
  auto* shm = GetShared();
  if (!shm) {
    return;
  }

  LARGE_INTEGER li{};
  QueryPerformanceCounter(&li);
  const std::uint64_t now = static_cast<std::uint64_t>(li.QuadPart);
  shm->header.last_heartbeat_qpc = now;

  static std::uint64_t lastQpc = 0;
  static std::uint64_t lastLoggedQpc = 0;

  const bool enabled = g_hitchEnabled.load();
  const std::uint32_t thresholdMs = g_hitchThresholdMs.load();
  const std::uint32_t cooldownMs = g_hitchCooldownMs.load();

  if (enabled && thresholdMs > 0 && lastQpc != 0 && shm->header.qpc_freq != 0) {
    if (now >= lastQpc) {
      const std::uint64_t deltaQpc = now - lastQpc;
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
          PushEvent(skydiag::EventType::kPerfHitch, p, sizeof(p));
        }
      }
    }
  }

  lastQpc = now;
}

class HeartbeatInputSink final : public RE::BSTEventSink<RE::InputEvent*>
{
public:
  RE::BSEventNotifyControl ProcessEvent(
    RE::InputEvent* const*,
    RE::BSTEventSource<RE::InputEvent*>*) override
  {
    if (!g_running.load()) {
      return RE::BSEventNotifyControl::kContinue;
    }
    UpdateHeartbeatOnMainThread();
    return RE::BSEventNotifyControl::kContinue;
  }
};

HeartbeatInputSink g_inputSink;

}  // namespace

static void ApplyHeartbeatConfig(const HeartbeatConfig& cfg) noexcept
{
  g_hitchEnabled.store(cfg.enableHitchLog);
  g_hitchThresholdMs.store(cfg.hitchThresholdMs);
  g_hitchCooldownMs.store(cfg.hitchCooldownMs);
}

bool StartHeartbeatScheduler(const HeartbeatConfig& cfg)
{
  ApplyHeartbeatConfig(cfg);
  g_running.store(true);
  HeartbeatOnInputLoaded();
  return true;
}

void HeartbeatOnInputLoaded() noexcept
{
  bool expected = false;
  if (!g_inputSinkRegistered.compare_exchange_strong(expected, true)) {
    return;  // already registered
  }

  auto* mgr = RE::BSInputDeviceManager::GetSingleton();
  if (!mgr) {
    g_inputSinkRegistered.store(false);
    return;
  }

  mgr->AddEventSink(&g_inputSink);
}

}  // namespace skydiag::plugin
