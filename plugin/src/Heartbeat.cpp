#include "SkyrimDiag/Heartbeat.h"

#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <SKSE/SKSE.h>

#include "SkyrimDiag/Blackbox.h"
#include "SkyrimDiag/Hash.h"
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
// Intentionally heap-owned. SKSE plugins have no loader-lock-safe unload
// callback where a joining jthread destructor could run. The module is pinned
// before this worker starts; explicit shutdown joins and deletes it outside
// DllMain, while process termination lets Windows reclaim it after other
// threads have already stopped.
std::jthread* g_scheduler = nullptr;
bool g_lifecycleBaselineReady = false;
std::uint64_t g_lifecycleActiveUntilQpc = 0;
std::uint64_t g_nextLifecyclePollQpc = 0;
std::unordered_map<std::uint64_t, std::string> g_lastModuleNames;
std::unordered_set<std::uint32_t> g_lastThreadIds;

constexpr std::uint64_t kLifecyclePollIntervalMs = 1000;

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
        const double roundedDeltaMs = std::round(deltaMs);
        p.a = roundedDeltaMs >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())
                ? std::numeric_limits<std::uint64_t>::max()
                : static_cast<std::uint64_t>(roundedDeltaMs);
        p.b = shm->header.state_flags;
        p.c = static_cast<std::uint64_t>(g_intervalMs.load());
        PushEvent(skydiag::EventType::kPerfHitch, p, sizeof(p));
      }
    }
  }

  g_taskPending.store(false);
}

std::uint64_t QpcNow() noexcept
{
  LARGE_INTEGER li{};
  QueryPerformanceCounter(&li);
  return static_cast<std::uint64_t>(li.QuadPart);
}

std::string WideToUtf8BestEffort(std::wstring_view text)
{
  if (text.empty()) {
    return {};
  }

  const int needed = WideCharToMultiByte(
    CP_UTF8,
    0,
    text.data(),
    static_cast<int>(text.size()),
    nullptr,
    0,
    nullptr,
    nullptr);
  if (needed <= 0) {
    return {};
  }

  std::string out(static_cast<std::size_t>(needed), '\0');
  const int written = WideCharToMultiByte(
    CP_UTF8,
    0,
    text.data(),
    static_cast<int>(text.size()),
    out.data(),
    needed,
    nullptr,
    nullptr);
  if (written <= 0) {
    return {};
  }
  out.resize(static_cast<std::size_t>(written));
  return out;
}

constexpr char ToLowerAscii(char ch) noexcept
{
  if (ch >= 'A' && ch <= 'Z') {
    return static_cast<char>(ch + ('a' - 'A'));
  }
  return ch;
}

std::uint64_t HashModuleName(std::string_view moduleNameUtf8)
{
  char lower[128]{};
  const std::size_t copyLen = std::min<std::size_t>(moduleNameUtf8.size(), sizeof(lower) - 1);
  for (std::size_t i = 0; i < copyLen; ++i) {
    lower[i] = ToLowerAscii(moduleNameUtf8[i]);
  }
  return skydiag::hash::Fnv1a64(std::string_view(lower, copyLen));
}

bool CaptureLoadedModules(std::unordered_map<std::uint64_t, std::string>& out)
{
  out.clear();

  DWORD neededBytes = 0;
  std::vector<HMODULE> modules(256);
  if (!EnumProcessModules(
        GetCurrentProcess(),
        modules.data(),
        static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
        &neededBytes)) {
    return false;
  }

  if (neededBytes > modules.size() * sizeof(HMODULE)) {
    modules.resize((neededBytes / sizeof(HMODULE)) + 8);
    if (!EnumProcessModules(
          GetCurrentProcess(),
          modules.data(),
          static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
          &neededBytes)) {
      return false;
    }
  }

  const std::size_t moduleCount = neededBytes / sizeof(HMODULE);
  std::vector<wchar_t> pathBuf(32768, L'\0');
  for (std::size_t i = 0; i < moduleCount; ++i) {
    const DWORD len = GetModuleFileNameW(modules[i], pathBuf.data(), static_cast<DWORD>(pathBuf.size()));
    if (len == 0 || len >= pathBuf.size()) {
      continue;
    }

    const std::filesystem::path modulePath(std::wstring(pathBuf.data(), len));
    const auto moduleName = modulePath.filename().wstring();
    if (moduleName.empty()) {
      continue;
    }

    const auto moduleNameUtf8 = WideToUtf8BestEffort(moduleName);
    if (moduleNameUtf8.empty()) {
      continue;
    }

    out.emplace(HashModuleName(moduleNameUtf8), moduleNameUtf8);
  }

  return true;
}

bool CaptureThreadIds(std::unordered_set<std::uint32_t>& out)
{
  out.clear();

  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return false;
  }

  const auto closeSnapshot = [&snapshot]() noexcept {
    if (snapshot != INVALID_HANDLE_VALUE) {
      CloseHandle(snapshot);
      snapshot = INVALID_HANDLE_VALUE;
    }
  };

  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  if (!Thread32First(snapshot, &entry)) {
    closeSnapshot();
    return false;
  }

  const DWORD currentPid = GetCurrentProcessId();
  do {
    if (entry.th32OwnerProcessID == currentPid && entry.th32ThreadID != 0u) {
      out.insert(entry.th32ThreadID);
    }
    entry.dwSize = sizeof(entry);
  } while (Thread32Next(snapshot, &entry));

  closeSnapshot();
  return true;
}

void EmitLifecycleDiffs(
  const std::unordered_map<std::uint64_t, std::string>& currentModules,
  const std::unordered_set<std::uint32_t>& currentThreadIds)
{
  constexpr std::size_t kMaxModuleEventsPerTick = 12;
  constexpr std::size_t kMaxThreadEventsPerTick = 16;

  std::size_t emittedModules = 0;
  for (const auto& [hash, name] : currentModules) {
    if (g_lastModuleNames.find(hash) == g_lastModuleNames.end()) {
      PushModuleLifecycleEvent(skydiag::EventType::kModuleLoad, name);
      if (++emittedModules >= kMaxModuleEventsPerTick) {
        break;
      }
    }
  }
  if (emittedModules < kMaxModuleEventsPerTick) {
    for (const auto& [hash, name] : g_lastModuleNames) {
      if (currentModules.find(hash) == currentModules.end()) {
        PushModuleLifecycleEvent(skydiag::EventType::kModuleUnload, name);
        if (++emittedModules >= kMaxModuleEventsPerTick) {
          break;
        }
      }
    }
  }

  const std::uint32_t activeThreadCount = static_cast<std::uint32_t>(currentThreadIds.size());
  std::size_t emittedThreads = 0;
  for (const auto tid : currentThreadIds) {
    if (g_lastThreadIds.find(tid) == g_lastThreadIds.end()) {
      PushThreadLifecycleEvent(skydiag::EventType::kThreadCreate, tid, activeThreadCount);
      if (++emittedThreads >= kMaxThreadEventsPerTick) {
        break;
      }
    }
  }
  if (emittedThreads < kMaxThreadEventsPerTick) {
    for (const auto tid : g_lastThreadIds) {
      if (currentThreadIds.find(tid) == currentThreadIds.end()) {
        PushThreadLifecycleEvent(skydiag::EventType::kThreadExit, tid, activeThreadCount);
        if (++emittedThreads >= kMaxThreadEventsPerTick) {
          break;
        }
      }
    }
  }
}

void PollLifecycleSignals()
{
  auto* shm = GetShared();
  if (!shm) {
    g_lifecycleBaselineReady = false;
    g_lifecycleActiveUntilQpc = 0;
    g_nextLifecyclePollQpc = 0;
    g_lastModuleNames.clear();
    g_lastThreadIds.clear();
    return;
  }

  const std::uint64_t nowQpc = QpcNow();
  if ((shm->header.state_flags & skydiag::kState_Loading) != 0u && shm->header.qpc_freq != 0u) {
    g_lifecycleActiveUntilQpc = nowQpc + (shm->header.qpc_freq * 5ull);
  }
  if (g_lifecycleActiveUntilQpc == 0u || nowQpc > g_lifecycleActiveUntilQpc) {
    g_lifecycleBaselineReady = false;
    g_nextLifecyclePollQpc = 0;
    g_lastModuleNames.clear();
    g_lastThreadIds.clear();
    return;
  }

  if (g_nextLifecyclePollQpc != 0u && nowQpc < g_nextLifecyclePollQpc) {
    return;
  }
  const std::uint64_t intervalQpc =
    std::max<std::uint64_t>(1ull, (shm->header.qpc_freq * kLifecyclePollIntervalMs) / 1000ull);
  g_nextLifecyclePollQpc = nowQpc + intervalQpc;

  std::unordered_map<std::uint64_t, std::string> currentModules;
  std::unordered_set<std::uint32_t> currentThreadIds;
  if (!CaptureLoadedModules(currentModules) || !CaptureThreadIds(currentThreadIds)) {
    return;
  }

  if (!g_lifecycleBaselineReady) {
    g_lastModuleNames = std::move(currentModules);
    g_lastThreadIds = std::move(currentThreadIds);
    g_lifecycleBaselineReady = true;
    return;
  }

  EmitLifecycleDiffs(currentModules, currentThreadIds);
  g_lastModuleNames = std::move(currentModules);
  g_lastThreadIds = std::move(currentThreadIds);
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

void SchedulerLoop(const std::stop_token& st)
{
  while (!st.stop_requested()) {
    const std::uint32_t intervalMs = g_intervalMs.load();
    if (!g_running.load() || intervalMs == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      continue;
    }

    QueueHeartbeatTask();
    try {
      PollLifecycleSignals();
    } catch (...) {
      // Lifecycle telemetry is optional; keep heartbeat scheduling alive if a
      // temporary allocation or enumeration failure escapes the poller.
      g_lifecycleBaselineReady = false;
      g_nextLifecyclePollQpc = 0;
      g_lastModuleNames.clear();
      g_lastThreadIds.clear();
    }
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
    try {
      g_scheduler = new std::jthread(SchedulerLoop);
    } catch (...) {
      g_running.store(false);
      g_schedulerStarted.store(false);
      throw;
    }
  }
  QueueHeartbeatTask();  // kick once immediately
  return true;
}

void StopHeartbeatScheduler() noexcept
{
  g_running.store(false);
  auto* scheduler = g_scheduler;
  g_scheduler = nullptr;
  if (scheduler) {
    try {
      scheduler->request_stop();
      if (scheduler->joinable()) {
        if (scheduler->get_id() == std::this_thread::get_id()) {
          scheduler->detach();
        } else {
          scheduler->join();
        }
      }
    } catch (...) {
      OutputDebugStringW(
        L"SkyrimDiag: heartbeat worker shutdown failed; pinned module will retain it.\n");
    }
    if (!scheduler->joinable()) {
      delete scheduler;
    }
  }
  g_schedulerStarted.store(false);
  g_taskPending.store(false);
}

void HeartbeatOnInputLoaded() noexcept
{
  // Heartbeat is scheduled via SKSE tasks; nothing to do.
}

}  // namespace skydiag::plugin
