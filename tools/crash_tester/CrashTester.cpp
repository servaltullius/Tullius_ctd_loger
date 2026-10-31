// Development-only SKSE plugin that causes crashes and freezes whose cause is
// known in advance, so the logger's analysis can be checked in the real game.
// It is never packaged. Hotkeys (Ctrl+Shift+...), each run on the game's main
// thread:
//   F1  access violation inside CrashTester.dll while RCX holds the reference
//       under the crosshair (the player when nothing is targeted)
//   F2  breakpoint (0x80000003), the way a mod's failed internal check stops
//   F3  unhandled C++ exception thrown from CrashTester.dll, while another
//       thread keeps throwing and catching C++ exceptions
//   F4  main thread held in Sleep inside CrashTester.dll for 45 s (a freeze);
//       may be pressed again after the game recovers
//   F5  an access violation CrashTester.dll catches itself, then 2 s later
//       a real one on a thread of its own
//   F6  access violation inside SkyrimSE.exe: CrashTester.dll passes a
//       corrupt reference to the engine's TESObjectREFR::GetDisplayFullName
//   F7  access violation in CrashTester.dll's menu event sink, called by the
//       game when the Journal Menu opens (a mod's callback crashing)
//   F8  access violation on a thread CrashTester.dll started itself
//   F9  stack overflow from recursion inside CrashTester.dll
//   F10 game job thread spinning in CrashTester.dll for 45 s (a busy freeze)
//   F11 menu event sink holding the game for 45 s when the Journal Menu opens
// Built a second time as SkyrimCrashGuard.dll (CRASH_TESTER_GUARD_STUB),
// which only loads and does nothing, to check the problem-module warning.

// CommonLibSSE-NG must come before any Windows API header (REX/W32/BASE.h).
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>

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

// Another thread throwing and catching C++ exceptions while the unhandled one
// is being reported. With one kept-exception slot these replaced it and the
// crash was ignored (review #5).
void StartCaughtThrowNoise()
{
  std::thread([] {
    for (int i = 0; i < 20000; ++i) {
      try {
        throw std::runtime_error("CrashTester: caught noise");
      } catch (const std::exception&) {
      }
      Sleep(1);
    }
  }).detach();
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

// Scenario 7 (F7): the game calls a mod's event sink, which crashes. The
// sink's frame is at the fault with the game's event dispatch below it.
__declspec(noinline) void CrashInEventSink(const RE::MenuOpenCloseEvent* event)
{
  volatile std::uint32_t* nowhere = nullptr;
  *nowhere = event->opening ? 1u : 0u;
}

// Scenario 8 (F8): a thread the mod started itself; no game frame below it.
__declspec(noinline) void CrashOnOwnThread()
{
  volatile std::uint64_t* nowhere = nullptr;
  *nowhere = 0x4F574E54ull;
}

// Scenario 9 (F9): unbounded recursion. The local buffer and the use of the
// result after the call keep every level a real frame.
__declspec(noinline) std::uint64_t Recurse(std::uint64_t depth)
{
  volatile std::uint8_t pad[256];
  pad[depth % sizeof(pad)] = static_cast<std::uint8_t>(depth);
  return Recurse(depth + 1) + pad[0];
}

// Scenario 10 (F10): a freeze that burns CPU in the mod instead of waiting.
__declspec(noinline) void SpinInPlugin()
{
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(45);
  volatile std::uint64_t spins = 0;
  while (std::chrono::steady_clock::now() < until) {
    spins = spins + 1;
    YieldProcessor();
  }
}

// Scenario 11 (F11): the game calls a mod's event sink, which blocks.
__declspec(noinline) void HoldInEventSink()
{
  for (int i = 0; i < 45; ++i) {
    Sleep(1000);
  }
}

// What the menu sink does the next time the Journal Menu opens.
enum class SinkAction : int
{
  kNone = 0,
  kCrash = 1,
  kHold = 2,
};
std::atomic<int> g_sinkAction{ static_cast<int>(SinkAction::kNone) };

class JournalSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
public:
  RE::BSEventNotifyControl ProcessEvent(
    const RE::MenuOpenCloseEvent* event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
  {
    if (event && event->opening && event->menuName == RE::JournalMenu::MENU_NAME) {
      switch (static_cast<SinkAction>(g_sinkAction.exchange(static_cast<int>(SinkAction::kNone)))) {
      case SinkAction::kCrash:
        CrashInEventSink(event);
        break;
      case SinkAction::kHold:
        HoldInEventSink();
        spdlog::warn("CrashTester: F11 event sink released");
        spdlog::default_logger()->flush();
        break;
      default:
        break;
      }
    }
    return RE::BSEventNotifyControl::kContinue;
  }
};

JournalSink g_journalSink;

template <class F>
void RunOnMainThread(const char* what, F&& task)
{
  spdlog::warn("CrashTester: {}", what);
  spdlog::default_logger()->flush();
  if (auto* tasks = SKSE::GetTaskInterface()) {
    tasks->AddTask(std::forward<F>(task));
  }
}

// Arms the menu sink and opens the Journal Menu so the game calls it.
void OpenJournalWith(SinkAction action, const char* what)
{
  g_sinkAction.store(static_cast<int>(action));
  RunOnMainThread(what, [] {
    if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
      queue->AddMessage(RE::JournalMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
    }
  });
}

// Scenario F5: a fatal-code exception the mod handles itself (the helper
// records it and starts checking it), then a real CTD while it is still
// checked. The report must be about the second, naming the first (ADR-0017).
__declspec(noinline) int CaughtAccessViolation()
{
  __try {
    volatile std::uint32_t* nowhere = nullptr;
    return static_cast<int>(*nowhere);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

void CaughtThenRealCrash(const char* what)
{
  RunOnMainThread(what, [] {
    spdlog::warn("CrashTester: F5 caught access violation returned {}", CaughtAccessViolation());
    spdlog::default_logger()->flush();
  });
  std::thread([] {
    Sleep(2000);
    CrashOnOwnThread();
  }).detach();
}

void CrashOnNewThread()
{
  spdlog::warn("CrashTester: F8 access violation on a thread of its own");
  spdlog::default_logger()->flush();
  std::thread([] { CrashOnOwnThread(); }).detach();
}

void SpinJobThread()
{
  RunOnMainThread("F10 job thread spinning for 45 s", [] {
    SpinInPlugin();
    g_freezeRunning.store(false);
    spdlog::warn("CrashTester: F10 spin released");
    spdlog::default_logger()->flush();
  });
}

void HotkeyLoop(const std::stop_token& stop)
{
  // A key counts as pressed when it is down now and was up at the previous
  // poll. GetAsyncKeyState's "pressed since the last call" bit is shared with
  // every other caller, and SkyrimDiag samples F4 every 25 ms for its Alt+F4
  // log (ADR-0022), so that bit was mostly gone by the time this loop looked
  // and Ctrl+Shift+F4 often did nothing (2026-10-31).
  std::array<bool, 12> wasDown{};
  while (!stop.stop_requested()) {
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const auto pressed = [&](int key) {
      const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
      bool& before = wasDown[static_cast<std::size_t>(key - VK_F1)];
      const bool edge = down && !before;
      before = down;
      return edge;
    };
    // Read every key so a key held from before the chord does not fire later.
    const bool f1 = pressed(VK_F1);
    const bool f2 = pressed(VK_F2);
    const bool f3 = pressed(VK_F3);
    const bool f4 = pressed(VK_F4);
    const bool f5 = pressed(VK_F5);
    const bool f6 = pressed(VK_F6);
    const bool f7 = pressed(VK_F7);
    const bool f8 = pressed(VK_F8);
    const bool f9 = pressed(VK_F9);
    const bool f10 = pressed(VK_F10);
    const bool f11 = pressed(VK_F11);
    if (ctrl && shift) {
      if ((f7 || f8 || f9) && !g_crashQueued.exchange(true)) {
        if (f7) {
          OpenJournalWith(SinkAction::kCrash, "F7 access violation in the Journal Menu event sink");
        } else if (f8) {
          CrashOnNewThread();
        } else {
          RunOnMainThread("F9 stack overflow", [] { spdlog::warn("CrashTester: depth {}", Recurse(0)); });
        }
      }
      if (f5 && !g_crashQueued.exchange(true)) {
        CaughtThenRealCrash("F5 caught access violation, a real one 2 s later");
      }
      if (f10 && !g_freezeRunning.exchange(true)) {
        SpinJobThread();
      }
      if (f11) {
        OpenJournalWith(SinkAction::kHold, "F11 Journal Menu event sink held for 45 s");
      }
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
          StartCaughtThrowNoise();
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
// AutoScenario=1..10 (1: freeze twice, then F1 on the nearest NPC; 2: F2;
//                     3: F3; 4: F6; 5: F7; 6: F8; 7: F9; 8: F10; 9: F11;
//                     10: F5).
//                     0 or missing: hotkeys only.
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
    StartCaughtThrowNoise();
    RunOnMainThread("auto F3 unhandled C++ exception", [] { ThrowUnhandled(); });
    break;
  case 4:
    RunOnMainThread("auto F6 corrupt reference passed to SkyrimSE.exe", [] { EngineFaultWithCorruptReference(); });
    break;
  case 5:
    OpenJournalWith(SinkAction::kCrash, "auto F7 access violation in the Journal Menu event sink");
    break;
  case 6:
    CrashOnNewThread();
    break;
  case 7:
    RunOnMainThread("auto F9 stack overflow", [] { spdlog::warn("CrashTester: depth {}", Recurse(0)); });
    break;
  case 8:
    g_freezeRunning.store(true);
    SpinJobThread();
    break;
  case 9:
    OpenJournalWith(SinkAction::kHold, "auto F11 Journal Menu event sink held for 45 s");
    break;
  case 10:
    CaughtThenRealCrash("auto F5 caught access violation, a real one 2 s later");
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
  if (message && message->type == SKSE::MessagingInterface::kDataLoaded) {
    if (auto* ui = RE::UI::GetSingleton()) {
      ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_journalSink);
    }
  }
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
  spdlog::warn("CrashTester loaded: Ctrl+Shift+F1 AV, F2 breakpoint, F3 C++ exception, F4 freeze 45 s, F6 engine fault, "
               "F7 event sink AV, F8 own-thread AV, F9 stack overflow, F10 spin freeze, F11 event sink freeze; "
               "AutoScenario={} SaveName={}", g_auto.scenario, g_auto.saveName);
  spdlog::default_logger()->flush();
  if (auto* messaging = SKSE::GetMessagingInterface()) {
    messaging->RegisterListener(OnSkseMessage);
  }
  g_hotkeys = new std::jthread(HotkeyLoop);
  return true;
}
#endif
