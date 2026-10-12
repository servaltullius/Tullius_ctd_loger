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
// The fake reference is heap memory full of 0x41 bytes, so the engine reads
// garbage pointers and faults inside SkyrimSE.exe.
__declspec(noinline) const char* EngineFaultWithCorruptReference()
{
  auto* fake = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!fake) {
    return nullptr;
  }
  std::memset(fake, 0x41, 0x1000);
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

}  // namespace

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
  SKSE::Init(skse);
  if (auto dir = SKSE::log::log_directory()) {
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((*dir / "CrashTester.log").string(), true);
    spdlog::set_default_logger(std::make_shared<spdlog::logger>("CrashTester", std::move(sink)));
  }
  spdlog::warn("CrashTester loaded: Ctrl+Shift+F1 AV, F2 breakpoint, F3 C++ exception, F4 freeze 45 s, F6 engine fault");
  spdlog::default_logger()->flush();
  g_hotkeys = new std::jthread(HotkeyLoop);
  return true;
}
#endif
