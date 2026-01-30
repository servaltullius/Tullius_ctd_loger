#include <Windows.h>

#include <filesystem>
#include <string>
#include <thread>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <SKSE/Version.h>

#include <spdlog/sinks/basic_file_sink.h>

#include "SkyrimDiag/Blackbox.h"
#include "SkyrimDiag/CrashHandler.h"
#include "SkyrimDiag/EventSinks.h"
#include "SkyrimDiag/Heartbeat.h"
#include "SkyrimDiag/ResourceLog.h"
#include "SkyrimDiag/SharedMemory.h"

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
  bool enablePerfHitchLog = true;
  std::uint32_t perfHitchThresholdMs = 250;
  std::uint32_t perfHitchCooldownMs = 3000;
};

PluginConfig g_cfg{};

std::wstring ReadIniString(const wchar_t* section, const wchar_t* key, const wchar_t* def, const wchar_t* path)
{
  wchar_t buf[MAX_PATH]{};
  GetPrivateProfileStringW(section, key, def, buf, MAX_PATH, path);
  return buf;
}

PluginConfig LoadConfig()
{
  PluginConfig cfg{};

  // Relative to game root when installed in Data/SKSE/Plugins.
  const wchar_t* iniPath = L"Data\\SKSE\\Plugins\\SkyrimDiag.ini";

  cfg.heartbeatIntervalMs =
    static_cast<std::uint32_t>(GetPrivateProfileIntW(L"SkyrimDiag", L"HeartbeatIntervalMs", 100, iniPath));

  {
    int mode = GetPrivateProfileIntW(L"SkyrimDiag", L"CrashHookMode", 1, iniPath);
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    cfg.crashHookMode = static_cast<std::uint32_t>(mode);
  }
  cfg.logMenus = GetPrivateProfileIntW(L"SkyrimDiag", L"LogMenus", 1, iniPath) != 0;
  cfg.autoStartHelper = GetPrivateProfileIntW(L"SkyrimDiag", L"AutoStartHelper", 1, iniPath) != 0;
  cfg.helperExe = ReadIniString(L"SkyrimDiag", L"HelperExe", L"SkyrimDiagHelper.exe", iniPath);
  cfg.enableTestHotkeys = GetPrivateProfileIntW(L"SkyrimDiag", L"EnableTestHotkeys", 0, iniPath) != 0;
  cfg.enableResourceLog = GetPrivateProfileIntW(L"SkyrimDiag", L"EnableResourceLog", 1, iniPath) != 0;
  cfg.enablePerfHitchLog = GetPrivateProfileIntW(L"SkyrimDiag", L"EnablePerfHitchLog", 1, iniPath) != 0;
  cfg.perfHitchThresholdMs = static_cast<std::uint32_t>(GetPrivateProfileIntW(L"SkyrimDiag", L"PerfHitchThresholdMs", 250, iniPath));
  cfg.perfHitchCooldownMs = static_cast<std::uint32_t>(GetPrivateProfileIntW(L"SkyrimDiag", L"PerfHitchCooldownMs", 3000, iniPath));

  return cfg;
}

void SetupLog()
{
  auto path = SKSE::log::log_directory();
  if (!path) {
    return;
  }

  *path /= "SkyrimDiag.log";
  auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
  auto logger = std::make_shared<spdlog::logger>("global log", std::move(sink));
  spdlog::set_default_logger(std::move(logger));
  spdlog::set_level(spdlog::level::info);
  spdlog::flush_on(spdlog::level::info);
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

std::filesystem::path GetThisModulePath()
{
  wchar_t buf[MAX_PATH]{};
  const HMODULE mod = GetThisModule();
  const DWORD n = GetModuleFileNameW(mod, buf, MAX_PATH);
  return std::filesystem::path(buf, buf + n);
}

bool StartHelperIfConfigured(const PluginConfig& cfg)
{
  if (!cfg.autoStartHelper) {
    return true;
  }

  const auto pid = GetCurrentProcessId();
  const auto dllPath = GetThisModulePath();
  const auto dllDir = dllPath.parent_path();

  std::filesystem::path helperPath(cfg.helperExe);
  if (helperPath.is_relative()) {
    helperPath = dllDir / helperPath;
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

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  spdlog::info("SkyrimDiag: helper started (helperPid={})", pi.dwProcessId);
  return true;
}

void StartTestHotkeysIfEnabled(const PluginConfig& cfg)
{
  if (!cfg.enableTestHotkeys) {
    return;
  }

  std::thread([]() {
    bool crashTriggered = false;
    bool hangTriggered = false;

    while (true) {
      const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
      const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

      if (ctrl && shift && ((GetAsyncKeyState(VK_F10) & 1) != 0) && !crashTriggered) {
        crashTriggered = true;
        spdlog::warn("SkyrimDiag: test hotkey -> intentional crash");
        skydiag::plugin::Note(/*tag=*/0x544553545F435241ull);  // "TEST_CRA"
        if (auto* ti = SKSE::GetTaskInterface()) {
          ti->AddTask([]() {
            *reinterpret_cast<volatile int*>(0) = 0;
          });
        }
      }

      if (ctrl && shift && ((GetAsyncKeyState(VK_F11) & 1) != 0) && !hangTriggered) {
        hangTriggered = true;
        spdlog::warn("SkyrimDiag: test hotkey -> intentional hang (main thread)");
        skydiag::plugin::Note(/*tag=*/0x544553545F48414Eull);  // "TEST_HAN"
        if (auto* ti = SKSE::GetTaskInterface()) {
          ti->AddTask([]() {
            for (;;) {
              Sleep(1000);
            }
          });
        }
      }

      Sleep(50);
    }
  }).detach();
}

}  // namespace

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
  SKSE::Init(skse);
  SetupLog();

  g_cfg = LoadConfig();

  if (!skydiag::plugin::InitSharedMemory()) {
    spdlog::warn("SkyrimDiag: shared memory init failed; plugin stays loaded but diagnostics disabled");
    return true;
  }

  skydiag::plugin::Note(/*tag=*/0x53455353494F4E31ull);  // "SESSION1" (tag only)

  // Start as early as possible: the helper relies on main-thread heartbeat updates.
  skydiag::plugin::StartHeartbeatScheduler(skydiag::plugin::HeartbeatConfig{
    g_cfg.heartbeatIntervalMs,
    g_cfg.enablePerfHitchLog,
    g_cfg.perfHitchThresholdMs,
    g_cfg.perfHitchCooldownMs,
  });

  StartHelperIfConfigured(g_cfg);
  StartTestHotkeysIfEnabled(g_cfg);
  if (g_cfg.enableResourceLog) {
    if (!skydiag::plugin::InstallResourceHooks()) {
      spdlog::warn("SkyrimDiag: resource hook install failed (resource logging disabled)");
    }
  }

  if (g_cfg.crashHookMode != 0) {
    if (!skydiag::plugin::InstallCrashHandler(g_cfg.crashHookMode)) {
      spdlog::warn("SkyrimDiag: crash handler install failed");
    }
  }

  if (auto* msg = SKSE::GetMessagingInterface()) {
    msg->RegisterListener(OnSkseMessage);
  }

  spdlog::info("SkyrimDiag: loaded");
  return true;
}
