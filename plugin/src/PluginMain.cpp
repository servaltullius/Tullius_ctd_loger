#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <SKSE/Version.h>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/null_sink.h>

#include "SkyrimDiag/Blackbox.h"
#include "SkyrimDiag/CrashHandler.h"
#include "SkyrimDiag/EventSinks.h"
#include "SkyrimDiag/Heartbeat.h"
#include "SkyrimDiag/ResourceLog.h"
#include "SkyrimDiag/SharedMemory.h"
#include "SkyrimDiagProtocol.h"

namespace {

struct PluginConfig
{
  std::uint32_t heartbeatIntervalMs = 100;
  std::uint32_t crashHookMode = 1;
  bool enableUnsafeCrashHookMode2 = false;
  bool logMenus = true;
  bool autoStartHelper = true;
  std::wstring helperExe = L"SkyrimDiagHelper.exe";
  bool enableTestHotkeys = false;
  bool enableResourceLog = true;
  bool enableAdaptiveResourceLogThrottle = true;
  std::uint32_t resourceLogThrottleHighWatermarkPerSec = 1500;
  std::uint32_t resourceLogThrottleMaxSampleDivisor = 8;
  bool enablePerfHitchLog = true;
  std::uint32_t perfHitchThresholdMs = 250;
  std::uint32_t perfHitchCooldownMs = 3000;
};

PluginConfig g_cfg{};

std::wstring ReadIniString(const wchar_t* section, const wchar_t* key, const wchar_t* def, const wchar_t* path)
{
  std::size_t capacity = 256;
  if (def) {
    const std::size_t defLen = std::wcslen(def) + 1;
    if (defLen > capacity) {
      capacity = defLen;
    }
  }

  while (capacity <= 32768) {
    std::vector<wchar_t> buf(capacity, L'\0');
    const DWORD n = GetPrivateProfileStringW(
      section,
      key,
      def,
      buf.data(),
      static_cast<DWORD>(buf.size()),
      path);
    if (n < (buf.size() - 1)) {
      return std::wstring(buf.data(), n);
    }
    capacity *= 2;
  }

  return def ? std::wstring(def) : std::wstring{};
}

std::uint32_t ReadIniUint32Clamped(
  const wchar_t* section,
  const wchar_t* key,
  int def,
  const wchar_t* path,
  std::uint32_t minValue,
  std::uint32_t maxValue)
{
  const int raw = GetPrivateProfileIntW(section, key, def, path);
  const auto clamped = std::clamp<long long>(
    static_cast<long long>(raw),
    static_cast<long long>(minValue),
    static_cast<long long>(maxValue));
  return static_cast<std::uint32_t>(clamped);
}

PluginConfig LoadConfig()
{
  PluginConfig cfg{};

  // Relative to game root when installed in Data/SKSE/Plugins.
  const wchar_t* iniPath = L"Data\\SKSE\\Plugins\\SkyrimDiag.ini";

  cfg.heartbeatIntervalMs = ReadIniUint32Clamped(
    L"SkyrimDiag", L"HeartbeatIntervalMs", 100, iniPath, 10, 5000);

  {
    int mode = GetPrivateProfileIntW(L"SkyrimDiag", L"CrashHookMode", 1, iniPath);
    cfg.enableUnsafeCrashHookMode2 =
      GetPrivateProfileIntW(L"SkyrimDiag", L"EnableUnsafeCrashHookMode2", 0, iniPath) != 0;
    if (mode == 2 && !cfg.enableUnsafeCrashHookMode2) {
      mode = 1;
    }
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    cfg.crashHookMode = static_cast<std::uint32_t>(mode);
  }
  cfg.logMenus = GetPrivateProfileIntW(L"SkyrimDiag", L"LogMenus", 1, iniPath) != 0;
  cfg.autoStartHelper = GetPrivateProfileIntW(L"SkyrimDiag", L"AutoStartHelper", 1, iniPath) != 0;
  cfg.helperExe = ReadIniString(L"SkyrimDiag", L"HelperExe", L"SkyrimDiagHelper.exe", iniPath);
  cfg.enableTestHotkeys = GetPrivateProfileIntW(L"SkyrimDiag", L"EnableTestHotkeys", 0, iniPath) != 0;
  cfg.enableResourceLog = GetPrivateProfileIntW(L"SkyrimDiag", L"EnableResourceLog", 1, iniPath) != 0;
  cfg.enableAdaptiveResourceLogThrottle =
    GetPrivateProfileIntW(L"SkyrimDiag", L"EnableAdaptiveResourceLogThrottle", 1, iniPath) != 0;
  cfg.resourceLogThrottleHighWatermarkPerSec = ReadIniUint32Clamped(
    L"SkyrimDiag", L"ResourceLogThrottleHighWatermarkPerSec", 1500, iniPath, 1, 100000);
  cfg.resourceLogThrottleMaxSampleDivisor = ReadIniUint32Clamped(
    L"SkyrimDiag", L"ResourceLogThrottleMaxSampleDivisor", 8, iniPath, 1, 64);
  cfg.enablePerfHitchLog = GetPrivateProfileIntW(L"SkyrimDiag", L"EnablePerfHitchLog", 1, iniPath) != 0;
  cfg.perfHitchThresholdMs = ReadIniUint32Clamped(
    L"SkyrimDiag", L"PerfHitchThresholdMs", 250, iniPath, 1, 10000);
  cfg.perfHitchCooldownMs = ReadIniUint32Clamped(
    L"SkyrimDiag", L"PerfHitchCooldownMs", 3000, iniPath, 0, 60000);

  return cfg;
}

void InstallFallbackLogger() noexcept
{
  try {
    auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>("SkyrimDiag fallback", std::move(sink));
    spdlog::set_default_logger(std::move(logger));
    spdlog::set_level(spdlog::level::info);
  } catch (...) {
    // Plugin loading must not fail merely because even the no-op logger could
    // not be allocated. OutputDebugStringW is the last allocation-free notice.
    OutputDebugStringW(L"SkyrimDiag: failed to install fallback logger.\n");
  }
}

bool SetupLog() noexcept
{
  try {
    auto path = SKSE::log::log_directory();
    if (!path) {
      InstallFallbackLogger();
      return false;
    }

    *path /= "SkyrimDiag.log";
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
    auto logger = std::make_shared<spdlog::logger>("global log", std::move(sink));
    spdlog::set_default_logger(std::move(logger));
    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::info);
    return true;
  } catch (...) {
    InstallFallbackLogger();
    OutputDebugStringW(L"SkyrimDiag: file logger setup failed; using no-op fallback.\n");
    return false;
  }
}

void OnDataLoaded(const PluginConfig& cfg)
{
  // If we're no longer in the loading menu, clear the initial loading flag.
  if (auto* shm = skydiag::plugin::GetShared()) {
    auto* ui = RE::UI::GetSingleton();
    if (!ui || !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
      InterlockedAnd(
        reinterpret_cast<volatile LONG*>(&shm->header.state_flags),
        ~static_cast<LONG>(skydiag::kState_Loading));
    }
  }

  skydiag::plugin::RegisterEventSinks(cfg.logMenus);
  skydiag::plugin::StartHeartbeatScheduler(skydiag::plugin::HeartbeatConfig{
    cfg.heartbeatIntervalMs,
    cfg.enablePerfHitchLog,
    cfg.perfHitchThresholdMs,
    cfg.perfHitchCooldownMs,
  });

  spdlog::info("SkyrimDiag: data loaded; heartbeat={}ms crashHookMode={} logMenus={}",
               cfg.heartbeatIntervalMs,
               cfg.crashHookMode,
               cfg.logMenus);
}

void OnSkseMessage(SKSE::MessagingInterface::Message* message)
{
  if (!message) {
    return;
  }
  // CrashLogger is itself an SKSE plugin and may load after us, in which case
  // InstallCrashHandler saw no module image to cache. Re-check at each
  // lifecycle stage so nested-fault suppression covers late loads too; the
  // ranges publish at most once, so repeating this is cheap and idempotent.
  if (message->type == SKSE::MessagingInterface::kPostLoad ||
      message->type == SKSE::MessagingInterface::kPostPostLoad ||
      message->type == SKSE::MessagingInterface::kInputLoaded ||
      message->type == SKSE::MessagingInterface::kDataLoaded) {
    skydiag::plugin::RefreshCrashLoggerModuleRanges();
  }
  if (message->type == SKSE::MessagingInterface::kInputLoaded) {
    skydiag::plugin::HeartbeatOnInputLoaded();
  }
  if (message->type == SKSE::MessagingInterface::kDataLoaded) {
    OnDataLoaded(g_cfg);
  }
}

HMODULE GetThisModule() noexcept
{
  HMODULE mod{};
  GetModuleHandleExW(
    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
    reinterpret_cast<LPCWSTR>(&GetThisModule),
    &mod);
  return mod;
}

bool PinThisModuleForWorkerLifetime() noexcept
{
  HMODULE pinnedModule{};
  return GetModuleHandleExW(
           GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
             GET_MODULE_HANDLE_EX_FLAG_PIN,
           reinterpret_cast<LPCWSTR>(&PinThisModuleForWorkerLifetime),
           &pinnedModule) != FALSE;
}

std::filesystem::path GetThisModulePath()
{
  std::vector<wchar_t> buf(32768, L'\0');
  const HMODULE mod = GetThisModule();
  const DWORD n = GetModuleFileNameW(mod, buf.data(), static_cast<DWORD>(buf.size()));
  if (n == 0 || n >= buf.size()) {
    return {};
  }
  return std::filesystem::path(buf.data(), buf.data() + n);
}

using skydiag::protocol::MakeKernelName;

bool IsHelperSingletonPresent(std::uint32_t pid)
{
  const auto mutexName = MakeKernelName(pid, skydiag::protocol::kKernelObjectSuffix_HelperMutex);
  HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, mutexName.c_str());
  if (!h) {
    return false;
  }
  CloseHandle(h);
  return true;
}

std::filesystem::path ResolveHelperPath(const std::wstring& helperExe)
{
  const auto dllPath = GetThisModulePath();
  const auto dllDir = dllPath.parent_path();
  std::filesystem::path helperPath(helperExe);
  if (helperPath.is_relative()) {
    helperPath = dllDir / helperPath;
  }
  return helperPath;
}

bool StartHelperProcess(const std::filesystem::path& helperPath, std::uint32_t pid, DWORD* helperPidOut = nullptr)
{
  if (helperPidOut) {
    *helperPidOut = 0;
  }

  std::error_code pathEc;
  if (!std::filesystem::exists(helperPath, pathEc)) {
    spdlog::warn("SkyrimDiag: helper path does not exist: {}", helperPath.string());
    if (pathEc) {
      spdlog::warn("SkyrimDiag: helper path check error={} for {}", pathEc.value(), helperPath.string());
    }
    return false;
  }

  std::wstring cmd = L"\"";
  cmd += helperPath.wstring();
  cmd += L"\" --pid ";
  cmd += std::to_wstring(pid);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};

  const BOOL ok = CreateProcessW(
    helperPath.c_str(),
    cmd.data(),
    nullptr,
    nullptr,
    FALSE,
    CREATE_NO_WINDOW,
    nullptr,
    helperPath.parent_path().c_str(),
    &si,
    &pi);

  if (!ok) {
    const DWORD le = GetLastError();
    spdlog::warn("SkyrimDiag: failed to start helper (err={})", le);
    return false;
  }

  if (helperPidOut) {
    *helperPidOut = pi.dwProcessId;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

bool StartHelperIfConfigured(const PluginConfig& cfg)
{
  if (!cfg.autoStartHelper) {
    return true;
  }

  const auto pid = GetCurrentProcessId();
  if (IsHelperSingletonPresent(pid)) {
    spdlog::info("SkyrimDiag: helper already active (pid={})", pid);
    return true;
  }

  DWORD helperPid = 0;
  const auto helperPath = ResolveHelperPath(cfg.helperExe);
  if (!StartHelperProcess(helperPath, pid, &helperPid)) {
    return false;
  }

  spdlog::info("SkyrimDiag: helper started (helperPid={})", helperPid);
  return true;
}

// These workers are heap-owned so CRT detach never joins them while holding the
// loader lock. The module is pinned before any worker starts. The exported
// shutdown entry point joins and deletes them only when called from a normal
// thread outside DllMain.
static std::jthread* g_watchdogThread = nullptr;
static std::jthread* g_testHotkeysThread = nullptr;
static std::atomic_bool g_watchdogStarted{ false };
static std::atomic_bool g_testHotkeysStarted{ false };

void StartHelperWatchdogIfConfigured(const PluginConfig& cfg)
{
  if (!cfg.autoStartHelper) {
    return;
  }

  if (g_watchdogStarted.exchange(true)) {
    return;
  }

  try {
    g_watchdogThread = new std::jthread(
      [helperExe = cfg.helperExe](const std::stop_token &stopToken) noexcept {
        try {
          constexpr std::uint32_t kRetryMinMs = 1000;
          constexpr std::uint32_t kRetryMaxMs = 30000;
          constexpr std::uint32_t kLoopSleepMs = 1000;
          constexpr std::uint32_t kInitialGraceMs = 3000;
          constexpr int kHelperConfirmPollCount = 15;
          constexpr std::uint32_t kHelperConfirmPollSleepMs = 100;

          const auto pid = GetCurrentProcessId();
          std::uint32_t retryBackoffMs = kRetryMinMs;
          ULONGLONG nextAttemptTick = GetTickCount64() + kInitialGraceMs;

          while (!stopToken.stop_requested()) {
            try {
              if (IsHelperSingletonPresent(pid)) {
                retryBackoffMs = kRetryMinMs;
                nextAttemptTick = 0;
                Sleep(kLoopSleepMs);
                continue;
              }

              const ULONGLONG now = GetTickCount64();
              if (nextAttemptTick != 0 && now < nextAttemptTick) {
                Sleep(kLoopSleepMs);
                continue;
              }

              DWORD helperPid = 0;
              const auto helperPath = ResolveHelperPath(helperExe);
              if (StartHelperProcess(helperPath, pid, &helperPid)) {
                bool confirmed = false;
                for (int i = 0; i < kHelperConfirmPollCount; ++i) {
                  if (stopToken.stop_requested())
                    break;
                  if (IsHelperSingletonPresent(pid)) {
                    confirmed = true;
                    break;
                  }
                  Sleep(kHelperConfirmPollSleepMs);
                }
                if (confirmed) {
                  spdlog::info("SkyrimDiag: helper watchdog confirmed helper "
                               "running (helperPid={})",
                               helperPid);
                  retryBackoffMs = kRetryMinMs;
                  nextAttemptTick = 0;
                } else if (!stopToken.stop_requested()) {
                  nextAttemptTick = GetTickCount64() + retryBackoffMs;
                  spdlog::warn("SkyrimDiag: helper watchdog launch "
                               "unconfirmed; retry in {} ms",
                               retryBackoffMs);
                  retryBackoffMs = std::min(retryBackoffMs * 2, kRetryMaxMs);
                }
              } else {
                nextAttemptTick = GetTickCount64() + retryBackoffMs;
                spdlog::warn("SkyrimDiag: helper watchdog retry in {} ms",
                             retryBackoffMs);
                retryBackoffMs = std::min(retryBackoffMs * 2, kRetryMaxMs);
              }

              Sleep(kLoopSleepMs);
            } catch (...) {
              // A watchdog failure must not unwind across the thread entry
              // point or permanently disable helper recovery after one
              // transient exception.
              nextAttemptTick = GetTickCount64() + retryBackoffMs;
              retryBackoffMs = std::min(retryBackoffMs * 2, kRetryMaxMs);
              OutputDebugStringW(L"SkyrimDiag: helper watchdog iteration "
                                 L"failed; retry scheduled.\n");
              Sleep(kLoopSleepMs);
            }
          }
        } catch (...) {
          // Preserve the noexcept thread boundary even if an unexpected failure
          // occurs outside the per-iteration recovery scope.
          OutputDebugStringW(L"SkyrimDiag: helper watchdog stopped after an "
                             L"unexpected failure.\n");
        }
      });
  } catch (...) {
    g_watchdogStarted.store(false);
    throw;
  }
}

void StartTestHotkeysIfEnabled(const PluginConfig& cfg)
{
  if (!cfg.enableTestHotkeys) {
    return;
  }

  if (g_testHotkeysStarted.exchange(true)) {
    return;
  }

  try {
    g_testHotkeysThread = new std::jthread([](const std::stop_token& stopToken) {
      bool crashTriggered = false;
      bool hangTriggered = false;

      while (!stopToken.stop_requested()) {
        const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

        if (ctrl && shift && ((GetAsyncKeyState(VK_F10) & 1) != 0) && !crashTriggered) {
          crashTriggered = true;
          spdlog::warn("SkyrimDiag: test hotkey -> intentional crash");
          skydiag::plugin::Note(/*tag=*/0x544553545F435241ull);  // "TEST_CRA"
          if (auto* ti = SKSE::GetTaskInterface()) {
            try {
              ti->AddUITask([]() {
                *reinterpret_cast<volatile int*>(0) = 0;
              });
            } catch (...) {
              spdlog::warn("SkyrimDiag: failed to enqueue crash hotkey UI task");
            }
          }
        }

        if (ctrl && shift && ((GetAsyncKeyState(VK_F11) & 1) != 0) && !hangTriggered) {
          hangTriggered = true;
          spdlog::warn("SkyrimDiag: test hotkey -> intentional hang (main thread)");
          skydiag::plugin::Note(/*tag=*/0x544553545F48414Eull);  // "TEST_HAN"
          if (auto* ti = SKSE::GetTaskInterface()) {
            try {
              ti->AddUITask([]() {
                for (;;) {
                  Sleep(1000);
                }
              });
            } catch (...) {
              spdlog::warn("SkyrimDiag: failed to enqueue hang hotkey UI task");
            }
          }
        }

        Sleep(50);
      }
    });
  } catch (...) {
    g_testHotkeysStarted.store(false);
    throw;
  }
}

void StopOwnedWorker(std::jthread*& worker) noexcept
{
  auto* owned = worker;
  worker = nullptr;
  if (!owned) {
    return;
  }
  try {
    owned->request_stop();
    if (owned->joinable()) {
      if (owned->get_id() == std::this_thread::get_id()) {
        owned->detach();
      } else {
        owned->join();
      }
    }
  } catch (...) {
    OutputDebugStringW(
      L"SkyrimDiag: background worker shutdown failed; pinned module will retain it.\n");
  }
  if (!owned->joinable()) {
    delete owned;
  }
}

void StopPluginBackgroundWorkers() noexcept
{
  StopOwnedWorker(g_testHotkeysThread);
  StopOwnedWorker(g_watchdogThread);
  g_testHotkeysStarted.store(false);
  g_watchdogStarted.store(false);
}

} // namespace

extern "C" __declspec(dllexport) void SkyrimDiagShutdownWorkers() noexcept
{
  StopPluginBackgroundWorkers();
  skydiag::plugin::StopHeartbeatScheduler();
}

SKSEPluginLoad(const SKSE::LoadInterface *skse) {
  bool skseInitialized = false;
  try {
    SKSE::Init(skse);
    skseInitialized = true;
    if (!SetupLog()) {
      OutputDebugStringW(
          L"SkyrimDiag: continuing plugin load without a file logger.\n");
    }

    g_cfg = LoadConfig();

    if (!skydiag::plugin::InitSharedMemory()) {
      spdlog::warn("SkyrimDiag: shared memory init failed; plugin stays loaded "
                   "but diagnostics disabled");
      return true;
    }

    if (!PinThisModuleForWorkerLifetime()) {
      spdlog::error("SkyrimDiag: failed to pin plugin module before starting "
                    "background workers");
      return false;
    }

    skydiag::plugin::Note(
        /*tag=*/0x53455353494F4E31ull); // "SESSION1" (tag only)

    // Start as early as possible: the helper relies on main-thread heartbeat
    // updates.
    skydiag::plugin::StartHeartbeatScheduler(skydiag::plugin::HeartbeatConfig{
        g_cfg.heartbeatIntervalMs,
        g_cfg.enablePerfHitchLog,
        g_cfg.perfHitchThresholdMs,
        g_cfg.perfHitchCooldownMs,
    });

    StartHelperIfConfigured(g_cfg);
    StartHelperWatchdogIfConfigured(g_cfg);
    StartTestHotkeysIfEnabled(g_cfg);
    if (g_cfg.enableResourceLog) {
      skydiag::plugin::ConfigureResourceLogThrottle(
          g_cfg.enableAdaptiveResourceLogThrottle,
          g_cfg.resourceLogThrottleHighWatermarkPerSec,
          g_cfg.resourceLogThrottleMaxSampleDivisor);
      if (!skydiag::plugin::InstallResourceHooks()) {
        spdlog::warn("SkyrimDiag: resource hook install failed (resource "
                     "logging disabled)");
      } else {
        spdlog::info("SkyrimDiag: resource log throttle adaptive={} "
                     "highWatermarkPerSec={} maxDivisor={}",
                     g_cfg.enableAdaptiveResourceLogThrottle ? 1 : 0,
                     g_cfg.resourceLogThrottleHighWatermarkPerSec,
                     g_cfg.resourceLogThrottleMaxSampleDivisor);
      }
    }

    if (g_cfg.crashHookMode != 0) {
      if (!skydiag::plugin::InstallCrashHandler(g_cfg.crashHookMode)) {
        spdlog::warn("SkyrimDiag: crash handler install failed");
      }
    }

    if (auto *msg = SKSE::GetMessagingInterface()) {
      msg->RegisterListener(OnSkseMessage);
    }

    spdlog::info("SkyrimDiag: loaded");
    return true;
  } catch (...) {
    // Never unwind through the SKSE C ABI. Once SKSE initialization completed,
    // keep the DLL resident because hooks or worker threads may already exist.
    OutputDebugStringW(L"SkyrimDiag: unexpected plugin initialization failure; "
                       L"keeping any initialized components resident.\n");
    return skseInitialized;
  }
}
