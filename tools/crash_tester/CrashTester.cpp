// Development-only SKSE plugin that causes crashes and freezes whose cause is
// known in advance, so the logger's analysis can be checked in the real game.
// It is never packaged. Hotkeys (Ctrl+Shift+...), each run on the game's main
// thread:
//   F1  access violation inside CrashTester.dll while RCX holds the reference
//       under the crosshair (the player when nothing is targeted)
//   F2  breakpoint (0x80000003), the way a mod's failed internal check stops
//   F3  unhandled C++ exception thrown from CrashTester.dll
//   F4  main thread held in Sleep inside CrashTester.dll for 45 s (a freeze);
//       may be pressed again after the game recovers
//   F6  access violation inside SkyrimSE.exe: CrashTester.dll passes a
//       corrupt reference to the engine's TESObjectREFR::GetDisplayFullName
// Built a second time as SkyrimCrashGuard.dll (CRASH_TESTER_GUARD_STUB),
// which only loads and does nothing, to check the problem-module warning.

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <spdlog/sinks/basic_file_sink.h>

using namespace std::literals;

#ifdef CRASH_TESTER_GUARD_STUB
SKSEPluginInfo(
  .Version = REL::Version{ 0, 0, 1, 0 },
  .Name = "SkyrimCrashGuardStub"sv,
  .Author = "Tullius CTD Logger tests"sv,
  .StructCompatibility = SKSE::StructCompatibility::Independent,
  .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
  SKSE::Init(skse);
  return true;
}
#else
SKSEPluginInfo(
  .Version = REL::Version{ 0, 0, 1, 0 },
  .Name = "CrashTester"sv,
  .Author = "Tullius CTD Logger tests"sv,
  .StructCompatibility = SKSE::StructCompatibility::Independent,
  .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

namespace {

std::atomic_bool g_crashQueued{ false };
std::atomic_bool g_freezeRunning{ false };

RE::TESObjectREFR* CrosshairOrPlayer()
{
  if (auto* pick = RE::CrosshairPickData::GetSingleton()) {
    if (auto ref = pick->target.get()) {
      return ref.get();
    }
  }
  return RE::PlayerCharacter::GetSingleton();
}

// Scenario 1. noinline keeps a CrashTester.dll frame at the fault; the
// reference stays in RCX (the first argument) when the store faults.
__declspec(noinline) void AccessViolationWith(RE::TESObjectREFR* ref)
{
  volatile std::uint32_t* nowhere = nullptr;
  *nowhere = ref->GetFormID();
}

// Scenario 2.
__declspec(noinline) void Breakpoint()
{
  __debugbreak();
}

// Scenario 3.
__declspec(noinline) void ThrowUnhandled()
{
  throw std::runtime_error("CrashTester: intentional unhandled C++ exception");
}

// Scenario 4.
__declspec(noinline) void HoldMainThread()
{
  for (int i = 0; i < 45; ++i) {
    Sleep(1000);
  }
}

// Scenario 5: bad data handed to engine code, the most common real pattern.
// The "reference" is a page that cannot be read, so the engine's first access
// to it faults inside SkyrimSE.exe. (Readable garbage made the engine wait
// forever on what looked like a held lock instead of crashing.)
__declspec(noinline) const char* EngineFaultWithCorruptReference()
{
  auto* fake = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS));
  if (!fake) {
    return nullptr;
  }
  const char* name = reinterpret_cast<RE::TESObjectREFR*>(fake)->GetDisplayFullName();
  // Used after the call, so it is a real call (not a tail jump) and this
  // frame stays on the stack under the engine's frames, as a mod's would.
  spdlog::warn("CrashTester: engine returned {}", name ? name : "(null)");
  VirtualFree(fake, 0, MEM_RELEASE);
  return name;
}

template <class F>
void RunOnMainThread(const char* what, F&& task)
{
  spdlog::warn("CrashTester: {}", what);
  spdlog::default_logger()->flush();
  if (auto* tasks = SKSE::GetTaskInterface()) {
    tasks->AddTask(std::forward<F>(task));
  }
}

void HotkeyLoop(const std::stop_token& stop)
{
  while (!stop.stop_requested()) {
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const auto pressed = [](int key) { return (GetAsyncKeyState(key) & 1) != 0; };
    // Read every key so stale "pressed since last poll" bits do not linger.
    const bool f1 = pressed(VK_F1);
    const bool f2 = pressed(VK_F2);
    const bool f3 = pressed(VK_F3);
    const bool f4 = pressed(VK_F4);
    const bool f6 = pressed(VK_F6);
    if (ctrl && shift) {
      if ((f1 || f2 || f3 || f6) && !g_crashQueued.exchange(true)) {
        if (f1) {
          RunOnMainThread("F1 access violation with the crosshair reference in RCX", [] {
            auto* ref = CrosshairOrPlayer();
            spdlog::warn("CrashTester: target formID={:08X}", ref ? ref->GetFormID() : 0u);
            spdlog::default_logger()->flush();
            AccessViolationWith(ref);
          });
        } else if (f2) {
          RunOnMainThread("F2 breakpoint", [] { Breakpoint(); });
        } else if (f3) {
          RunOnMainThread("F3 unhandled C++ exception", [] { ThrowUnhandled(); });
        } else {
          RunOnMainThread("F6 corrupt reference passed to SkyrimSE.exe", [] { EngineFaultWithCorruptReference(); });
        }
      }
      if (f4 && !g_freezeRunning.exchange(true)) {
        RunOnMainThread("F4 main thread held for 45 s", [] {
          HoldMainThread();
          g_freezeRunning.store(false);
          spdlog::warn("CrashTester: F4 freeze released");
          spdlog::default_logger()->flush();
        });
      }
    }
    Sleep(50);
  }
}

std::jthread* g_hotkeys = nullptr;

// ---- unattended runs (CrashTester.ini next to the DLL) ----
// [CrashTester]
// AutoScenario=1..4  (1: freeze twice, then F1 on the nearest NPC; 2: F2;
//                     3: F3; 4: F6). 0 or missing: hotkeys only.
// SaveName=<save file name without .ess>, loaded from the main menu
// StartDelaySec=20   seconds after the save has loaded

struct AutoConfig
{
  int scenario = 0;
  std::string saveName;
  int startDelaySec = 20;
};

AutoConfig g_auto{};

// The SKSE log folder first: it is a real folder, while a file added to an
// MO2 mod folder stays invisible until MO2 refreshes.
std::wstring IniPath()
{
  if (auto dir = SKSE::log::log_directory()) {
    const auto ini = *dir / "CrashTester.ini";
    if (GetFileAttributesW(ini.c_str()) != INVALID_FILE_ATTRIBUTES) {
      return ini.wstring();
    }
  }
  HMODULE self = nullptr;
  GetModuleHandleExW(
    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
    reinterpret_cast<LPCWSTR>(&IniPath), &self);
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(self, path, MAX_PATH);
  std::wstring ini(path);
  const auto slash = ini.find_last_of(L"\\/");
  return ini.substr(0, slash == std::wstring::npos ? 0 : slash + 1) + L"CrashTester.ini";
}

AutoConfig ReadAutoConfig()
{
  const auto ini = IniPath();
  AutoConfig cfg{};
  cfg.scenario = static_cast<int>(GetPrivateProfileIntW(L"CrashTester", L"AutoScenario", 0, ini.c_str()));
  cfg.startDelaySec = static_cast<int>(GetPrivateProfileIntW(L"CrashTester", L"StartDelaySec", 20, ini.c_str()));
  wchar_t save[260]{};
  GetPrivateProfileStringW(L"CrashTester", L"SaveName", L"", save, 260, ini.c_str());
  char saveA[520]{};
  WideCharToMultiByte(CP_ACP, 0, save, -1, saveA, sizeof(saveA), nullptr, nullptr);
  cfg.saveName = saveA;
  return cfg;
}

RE::Actor* NearestNpc()
{
  auto* player = RE::PlayerCharacter::GetSingleton();
  auto* lists = RE::ProcessLists::GetSingleton();
  if (!player || !lists) {
    return nullptr;
  }
  RE::Actor* best = nullptr;
  float bestDistance = 0.0f;
  for (auto& handle : lists->highActorHandles) {
    auto actor = handle.get();
    if (!actor || actor.get() == player || actor->IsDead()) {
      continue;
    }
    const float distance = actor->GetPosition().GetDistance(player->GetPosition());
    if (!best || distance < bestDistance) {
      best = actor.get();
      bestDistance = distance;
    }
  }
  return best;
}

void WaitForFreezeToEnd()
{
  while (g_freezeRunning.load()) {
    Sleep(500);
  }
}

void RunAutoScenario()
{
  spdlog::warn("CrashTester: auto scenario {} starts in {} s", g_auto.scenario, g_auto.startDelaySec);
  spdlog::default_logger()->flush();
  Sleep(static_cast<DWORD>(g_auto.startDelaySec) * 1000u);
  switch (g_auto.scenario) {
  case 1:
    for (int round = 1; round <= 2; ++round) {
      g_freezeRunning.store(true);
      RunOnMainThread(round == 1 ? "auto freeze 1 of 2 (45 s)" : "auto freeze 2 of 2 (45 s)", [] {
        HoldMainThread();
        g_freezeRunning.store(false);
        spdlog::warn("CrashTester: auto freeze released");
        spdlog::default_logger()->flush();
      });
      Sleep(2000);
      WaitForFreezeToEnd();
      Sleep(20000);
    }
    RunOnMainThread("auto F1 access violation with the nearest NPC in RCX", [] {
      RE::TESObjectREFR* ref = NearestNpc();
      if (!ref) {
        ref = RE::PlayerCharacter::GetSingleton();
      }
      spdlog::warn("CrashTester: target formID={:08X} name={}", ref ? ref->GetFormID() : 0u,
        ref ? ref->GetDisplayFullName() : "");
      spdlog::default_logger()->flush();
      AccessViolationWith(ref);
    });
    break;
  case 2:
    RunOnMainThread("auto F2 breakpoint", [] { Breakpoint(); });
    break;
  case 3:
    RunOnMainThread("auto F3 unhandled C++ exception", [] { ThrowUnhandled(); });
    break;
  case 4:
    RunOnMainThread("auto F6 corrupt reference passed to SkyrimSE.exe", [] { EngineFaultWithCorruptReference(); });
    break;
  default:
    break;
  }
}

class MainMenuWatcher final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
public:
  RE::BSEventNotifyControl ProcessEvent(
    const RE::MenuOpenCloseEvent* event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
  {
    if (event && event->opening && event->menuName == RE::MainMenu::MENU_NAME && !loadRequested_.exchange(true)) {
      std::thread([] {
        Sleep(3000);
        spdlog::warn("CrashTester: loading save {}", g_auto.saveName);
        spdlog::default_logger()->flush();
        if (auto* tasks = SKSE::GetTaskInterface()) {
          tasks->AddTask([] {
            if (auto* manager = RE::BGSSaveLoadManager::GetSingleton()) {
              manager->Load(g_auto.saveName.c_str());
            }
          });
        }
      }).detach();
    }
    return RE::BSEventNotifyControl::kContinue;
  }

private:
  std::atomic_bool loadRequested_{ false };
};

MainMenuWatcher g_mainMenuWatcher;
std::atomic_bool g_scenarioStarted{ false };

void OnSkseMessage(SKSE::MessagingInterface::Message* message)
{
  if (!message || g_auto.scenario <= 0) {
    return;
  }
  if (message->type == SKSE::MessagingInterface::kDataLoaded && !g_auto.saveName.empty()) {
    if (auto* ui = RE::UI::GetSingleton()) {
      ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_mainMenuWatcher);
    }
  } else if (message->type == SKSE::MessagingInterface::kPostLoadGame && !g_scenarioStarted.exchange(true)) {
    std::thread(RunAutoScenario).detach();
  }
}

}  // namespace

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
  SKSE::Init(skse);
  if (auto dir = SKSE::log::log_directory()) {
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((*dir / "CrashTester.log").string(), true);
    spdlog::set_default_logger(std::make_shared<spdlog::logger>("CrashTester", std::move(sink)));
  }
  g_auto = ReadAutoConfig();
  spdlog::warn("CrashTester loaded: Ctrl+Shift+F1 AV, F2 breakpoint, F3 C++ exception, F4 freeze 45 s, F6 engine fault; "
               "AutoScenario={} SaveName={}", g_auto.scenario, g_auto.saveName);
  spdlog::default_logger()->flush();
  if (auto* messaging = SKSE::GetMessagingInterface()) {
    messaging->RegisterListener(OnSkseMessage);
  }
  g_hotkeys = new std::jthread(HotkeyLoop);
  return true;
}
#endif
