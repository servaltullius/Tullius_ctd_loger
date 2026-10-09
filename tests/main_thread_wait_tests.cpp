// What the frozen main thread was doing, with frame lists shaped like the
// user's real freeze dumps (2026-09-18 .. 09-28): four sleep-waits inside the
// engine's render pass and one wait inside the NVIDIA driver.

#include <cassert>
#include <string>
#include <vector>

#include "MainThreadWait.h"

using skydiag::dump_tool::ClassifyMainThreadWait;
using skydiag::dump_tool::IsBystanderWait;
using skydiag::dump_tool::ModalStackFrame;

namespace {

ModalStackFrame System(std::wstring module, std::wstring symbol, std::uint64_t displacement = 0x14)
{
  ModalStackFrame frame{};
  frame.has_module = true;
  frame.module_filename = std::move(module);
  frame.symbol = std::move(symbol);
  frame.displacement = displacement;
  frame.is_system = true;
  return frame;
}

ModalStackFrame Plugin(std::wstring module, std::wstring modName = L"")
{
  ModalStackFrame frame{};
  frame.has_module = true;
  frame.module_filename = std::move(module);
  frame.inferred_mod_name = std::move(modName);
  return frame;
}

ModalStackFrame GameExe()
{
  auto frame = Plugin(L"SkyrimSE.exe");
  frame.is_game_exe = true;
  return frame;
}

ModalStackFrame Skse()
{
  auto frame = Plugin(L"skse64_1_6_1170.dll");
  frame.is_skse_runtime = true;
  return frame;
}

void TestEngineSleepWaitInRenderPass()
{
  const std::vector<ModalStackFrame> frames = {
    System(L"ntdll.dll", L"NtDelayExecution"),
    System(L"ntdll.dll", L"RtlDelayExecution", 0x34),
    System(L"KERNELBASE.dll", L"SleepEx", 0x91),
    GameExe(),
    GameExe(),
    Plugin(L"EngineFixes.dll", L"Engine Fixes"),
    GameExe(),
    Plugin(L"CommunityShaders.dll"),
    GameExe(),
    Plugin(L"CommunityShaders.dll"),
    Skse(),
    Plugin(L"hdtsmp64.dll", L"FSMP"),
    GameExe(),
    System(L"kernel32.dll", L"BaseThreadInitThunk", 0x17),
  };
  const auto wait = ClassifyMainThreadWait(frames);
  assert(wait.kind == "engine_wait");
  assert(wait.wait_class == "sleep");
  assert(wait.wait_api == L"KERNELBASE.dll!SleepEx");
  assert(wait.waiting_module == L"SkyrimSE.exe");
  // Plugins further down, once each, without the SKSE runtime.
  assert((wait.path_modules == std::vector<std::wstring>{ L"EngineFixes.dll", L"CommunityShaders.dll", L"hdtsmp64.dll" }));
  assert(IsBystanderWait(wait));
}

void TestGraphicsDriverWaitWithShortStack()
{
  // The walk stopped inside the driver: no caller is known.
  auto driver = Plugin(L"nvwgf2umx.dll");
  driver.is_system = true;  // DriverStore paths can look system-like
  const auto wait = ClassifyMainThreadWait({
    System(L"ntdll.dll", L"NtWaitForSingleObject"),
    System(L"KERNELBASE.dll", L"WaitForSingleObjectEx", 0xaf),
    driver,
  });
  assert(wait.kind == "graphics_driver_wait");
  assert(wait.wait_class == "sync");
  assert(wait.waiting_module == L"nvwgf2umx.dll");
  assert(IsBystanderWait(wait));

  // A driver that is not marked system is still the graphics stack.
  const auto amd = ClassifyMainThreadWait({
    System(L"ntdll.dll", L"NtWaitForSingleObject"),
    Plugin(L"AMDXX64.DLL"),
    GameExe(),
  });
  assert(amd.kind == "graphics_driver_wait");
}

void TestPluginWaitAndRunning()
{
  const auto pluginWait = ClassifyMainThreadWait({
    System(L"ntdll.dll", L"NtWaitForSingleObject"),
    System(L"KERNELBASE.dll", L"WaitForSingleObjectEx", 0xaf),
    Plugin(L"SomeMod.dll", L"Some Mod"),
    GameExe(),
  });
  assert(pluginWait.kind == "plugin_wait");
  assert(pluginWait.waiting_module == L"SomeMod.dll");
  assert(pluginWait.waiting_mod_name == L"Some Mod");
  assert(!IsBystanderWait(pluginWait));

  const auto running = ClassifyMainThreadWait({ Plugin(L"BusyMod.dll"), GameExe() });
  assert(running.kind == "running");
  assert(running.waiting_module == L"BusyMod.dll");
  assert(!IsBystanderWait(running));

  // Inside a CRT copy called by a plugin: still running, not waiting.
  const auto inCrt = ClassifyMainThreadWait({ System(L"VCRUNTIME140.dll", L"memcpy", 0x120), Plugin(L"BusyMod.dll") });
  assert(inCrt.kind == "running");
  assert(inCrt.waiting_module == L"BusyMod.dll");
}

void TestUnknownWhenTheTopCannotBeRead()
{
  // An unnamed system frame on top: no basis to call it waiting or running.
  auto unnamed = System(L"ntdll.dll", L"");
  assert(ClassifyMainThreadWait({ unnamed, Plugin(L"SomeMod.dll") }).kind == "unknown");
  // A wait whose caller the walk never reached.
  ModalStackFrame lost{};
  assert(ClassifyMainThreadWait({ System(L"ntdll.dll", L"NtDelayExecution"), lost }).kind == "unknown");
  // A far-off export name is not a wait API.
  assert(ClassifyMainThreadWait({ System(L"ntdll.dll", L"NtDelayExecution", 0x9000), Plugin(L"X.dll") }).kind == "unknown");
  assert(ClassifyMainThreadWait({}).kind.empty());
}

}  // namespace

int main()
{
  TestEngineSleepWaitInRenderPass();
  TestGraphicsDriverWaitWithShortStack();
  TestPluginWaitAndRunning();
  TestUnknownWhenTheTopCannotBeRead();
  return 0;
}
