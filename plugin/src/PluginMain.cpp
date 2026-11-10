// CommonLibSSE-NG must come before any Windows API header (REX/W32/BASE.h).
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <SKSE/Version.h>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/null_sink.h>

#include "SkyrimDiag/Blackbox.h"
#include "SkyrimDiag/CloseRequestHooks.h"
#include "SkyrimDiag/CrashHandler.h"
#include "SkyrimDiag/CrashHookConfig.h"
#include "SkyrimDiag/EventSinks.h"
#include "SkyrimDiag/Heartbeat.h"
#include "SkyrimDiag/HelperWatchdog.h"
#include "SkyrimDiag/ResourceLog.h"
#include "SkyrimDiag/SharedMemory.h"
#include "SkyrimDiagProtocol.h"
#include "SkyrimDiagSessionLogs.h"

namespace {

struct PluginConfig
{
  std::uint32_t heartbeatIntervalMs = 100;
  std::uint32_t crashHookMode = 1;
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
  bool enableCloseRequestLog = true;
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
    // Read as text: GetPrivateProfileIntW reads "abc" as 0 (the crash hook
    // off) and 3 or 99 used to become mode 2 without its consent setting.
    const auto modeText = ReadIniString(L"SkyrimDiag", L"CrashHookMode", L"", iniPath);
    const auto consentText = ReadIniString(L"SkyrimDiag", L"EnableUnsafeCrashHookMode2", L"", iniPath);
    const auto setting = skydiag::plugin::ResolveCrashHookMode(modeText, consentText);
    cfg.crashHookMode = setting.mode;
    if (setting.invalid_value) {
      spdlog::warn("SkyrimDiag: CrashHookMode is not 0, 1 or 2; using 1 (fatal exceptions)");
    } else if (setting.mode2_refused) {
      spdlog::warn("SkyrimDiag: CrashHookMode=2 needs EnableUnsafeCrashHookMode2=1; using 1 (fatal exceptions)");
    }
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
  cfg.enableCloseRequestLog = GetPrivateProfileIntW(L"SkyrimDiag", L"EnableCloseRequestLog", 1, iniPath) != 0;

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

// path::string() converts to the ANSI code page and throws for a character
// outside it (a Korean user name on an English Windows). Log text is UTF-8.
std::string PathForLog(const std::filesystem::path& path)
{
  try {
    const auto u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
  } catch (...) {
    return "(path)";
  }
}

std::optional<std::string> WideToAnsiExact(const std::wstring& wide)
{
  const UINT codePage = GetACP();
  BOOL lossy = FALSE;
  // CP_UTF8 takes neither flag nor the lossy check.
  const DWORD flags = codePage == CP_UTF8 ? 0u : WC_NO_BEST_FIT_CHARS;
  BOOL* lossyOut = codePage == CP_UTF8 ? nullptr : &lossy;
  const int n = WideCharToMultiByte(codePage, flags, wide.c_str(), -1, nullptr, 0, nullptr, lossyOut);
  if (n <= 0 || lossy) {
    return std::nullopt;
  }
  std::string out(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(codePage, flags, wide.c_str(), -1, out.data(), n, nullptr, nullptr);
  out.resize(static_cast<std::size_t>(n - 1));
  return out;
}

// spdlog opens its file by an ANSI name. A folder outside the ANSI code page
// has none; its short 8.3 name, where the volume keeps them, does.
std::optional<std::string> AnsiFileName(const std::filesystem::path& path)
{
  if (auto name = WideToAnsiExact(path.wstring())) {
    return name;
  }
  wchar_t shortDir[MAX_PATH * 2]{};
  const DWORD n = GetShortPathNameW(path.parent_path().c_str(), shortDir, static_cast<DWORD>(std::size(shortDir)));
  if (n == 0 || n >= std::size(shortDir)) {
    return std::nullopt;
  }
  return WideToAnsiExact((std::filesystem::path(shortDir) / path.filename()).wstring());
}

// Earlier sessions' SkyrimDiag.log kept as SkyrimDiag.previous.log,
// SkyrimDiag.previous.2.log, ..., as many as the helper keeps by default: a
// test such as a quit after a freeze is often followed by another launch
// before anyone reads the log, which then started over (2026-10-31).
constexpr std::uint32_t kPluginSessionLogsToKeep = 5;

bool SetupLog() noexcept
{
  try {
    auto path = SKSE::log::log_directory();
    if (!path) {
      InstallFallbackLogger();
      return false;
    }

    *path /= "SkyrimDiag.log";
    // A log that cannot be moved is started over below, as before.
    skydiag::KeepSessionLog(*path, kPluginSessionLogsToKeep);
    const auto fileName = AnsiFileName(*path);
    if (!fileName) {
      InstallFallbackLogger();
      OutputDebugStringW(L"SkyrimDiag: the log folder has no ANSI name; using no-op fallback.\n");
      return false;
    }
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(*fileName, true);
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

  // ADR-0022: the game window exists by now. This runs on the loader thread,
  // so the window hooks wait for the heartbeat task on the window's thread.
  if (cfg.enableCloseRequestLog) {
    if (skydiag::plugin::StartCloseRequestLog()) {
      spdlog::info("SkyrimDiag: close request log on (key sampler, quit flag; window hooks from the game window's thread)");
    } else {
      spdlog::warn("SkyrimDiag: close request log off (game window not found, err={})", GetLastError());
    }
  }

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
  // ADR-0025: which save a load started with, so a crash before it finished
  // can be matched with earlier crashes loading the same save. SKSE passes the
  // save's file path (data) and its length (dataLen); PostLoadGame's data is
  // the result. A new game leaves no save in play.
  if (message->type == SKSE::MessagingInterface::kPreLoadGame) {
    std::string_view path;
    if (const auto* text = static_cast<const char*>(message->data)) {
      const std::size_t maxLen = std::min<std::size_t>(message->dataLen, 512u);
      std::size_t len = 0;
      while (len < maxLen && text[len] != '\0') {
        ++len;
      }
      path = std::string_view(text, len);
    }
    skydiag::plugin::PushSaveLoadStartEvent(path);
    // The name only: a folder would carry the Windows user name into the log.
    spdlog::info("SkyrimDiag: loading save {}", skydiag::SaveNameForEvent(path));
  }
  if (message->type == SKSE::MessagingInterface::kPostLoadGame) {
    const bool loaded = message->data != nullptr;
    skydiag::plugin::PushSaveLoadEndEvent(loaded ? skydiag::SaveLoadResult::kLoaded : skydiag::SaveLoadResult::kFailed);
    spdlog::info("SkyrimDiag: save load {}", loaded ? "finished" : "failed");
  }
  if (message->type == SKSE::MessagingInterface::kNewGame) {
    skydiag::plugin::PushSaveLoadEndEvent(skydiag::SaveLoadResult::kNewGame);
    spdlog::info("SkyrimDiag: new game started");
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
    spdlog::warn("SkyrimDiag: helper path does not exist: {}", PathForLog(helperPath));
    if (pathEc) {
      spdlog::warn("SkyrimDiag: helper path check error={} for {}", pathEc.value(), PathForLog(helperPath));
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
          constexpr std::uint32_t kLoopSleepMs = 1000;
          constexpr int kHelperConfirmPollCount = 15;
          constexpr std::uint32_t kHelperConfirmPollSleepMs = 100;

          using skydiag::plugin::HelperWatchdogPolicy;
          const auto pid = GetCurrentProcessId();
          // When to start the helper again, and when to stop trying
          // (HelperWatchdog.h).
          HelperWatchdogPolicy policy(GetTickCount64());
          const auto logPolicyEvent = [&policy]() {
            switch (policy.LastEvent()) {
              case HelperWatchdogPolicy::Event::kHelperExitedEarly:
                spdlog::warn(
                  "SkyrimDiag: helper exited {} ms after it started; retry in {} ms (failed start {} of {})",
                  policy.LastRunMs(), policy.RetryInMs(), policy.FailedStarts(),
                  HelperWatchdogPolicy::kMaxFailedStarts);
                break;
              case HelperWatchdogPolicy::Event::kStartFailed:
                spdlog::warn(
                  "SkyrimDiag: helper watchdog launch unconfirmed; retry in {} ms (failed start {} of {})",
                  policy.RetryInMs(), policy.FailedStarts(), HelperWatchdogPolicy::kMaxFailedStarts);
                break;
              case HelperWatchdogPolicy::Event::kGaveUp:
                spdlog::error(
                  "SkyrimDiag: the helper did not stay running after {} starts in a row; not starting it again "
                  "this session. Crashes and freezes are not captured until the game is restarted. "
                  "SkyrimDiagHelper.log in the output folder may say why it exits; security software that ends "
                  "SkyrimDiagHelper.exe does this too.",
                  policy.FailedStarts());
                break;
              default:
                break;
            }
          };

          while (!stopToken.stop_requested()) {
            try {
              const auto action = policy.Tick(GetTickCount64(), IsHelperSingletonPresent(pid));
              logPolicyEvent();
              if (action == HelperWatchdogPolicy::Action::kLaunch) {
                DWORD helperPid = 0;
                bool confirmed = false;
                if (StartHelperProcess(ResolveHelperPath(helperExe), pid, &helperPid)) {
                  for (int i = 0; i < kHelperConfirmPollCount && !stopToken.stop_requested(); ++i) {
                    if (IsHelperSingletonPresent(pid)) {
                      confirmed = true;
                      break;
                    }
                    Sleep(kHelperConfirmPollSleepMs);
                  }
                }
                if (stopToken.stop_requested()) {
                  break;  // the game is closing: not a failed start
                }
                policy.NoteLaunch(GetTickCount64(), confirmed);
                if (confirmed) {
                  spdlog::info("SkyrimDiag: helper watchdog confirmed helper "
                               "running (helperPid={})",
                               helperPid);
                }
                logPolicyEvent();
              }

              Sleep(kLoopSleepMs);
            } catch (...) {
              // A watchdog failure must not unwind across the thread entry
              // point or permanently disable helper recovery after one
              // transient exception. It counts as a failed start, so a
              // failure that repeats ends in the policy's stop, not a loop.
              policy.NoteLaunch(GetTickCount64(), /*confirmed=*/false);
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
    // Without CommonLib's own log: it opens SkyrimDiag.log truncated before
    // SetupLog can keep the last session's (2026-10-31: no previous log in
    // game, as KeepSessionLog leaves an empty log alone).
    SKSE::Init(skse, false);
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

    spdlog::info("SkyrimDiag: loaded (v{})", SKSE::GetPluginVersion().string("."));
    return true;
  } catch (...) {
    // Never unwind through the SKSE C ABI. Once SKSE initialization completed,
    // keep the DLL resident because hooks or worker threads may already exist.
    OutputDebugStringW(L"SkyrimDiag: unexpected plugin initialization failure; "
                       L"keeping any initialized components resident.\n");
    return skseInitialized;
  }
}
