// What the frozen main thread was doing, with frame lists shaped like the
// user's real freeze dumps (2026-09-18 .. 09-28): four sleep-waits inside the
// engine's render pass and one wait inside the NVIDIA driver.

#include <cassert>
#include <cstdint>
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

// The 0x60 code bytes ending at SkyrimSE.exe+0xe46e21 in the user's freeze
// dumps (game 1.6.1170): End(query) via vtable +0xE0, then a loop of
// GetData via vtable +0xE8, test, and Sleep(1) through the import table.
const std::vector<std::uint8_t> kFieldQueryPoll = {
  0x83, 0xfb, 0x03, 0x41, 0x0f, 0x43, 0xdc, 0x46, 0x38, 0xa4, 0x2b, 0x78, 0x6a, 0x28, 0x03, 0x75,
  0x69, 0x4c, 0x89, 0x74, 0x24, 0x70, 0x4d, 0x8b, 0xb4, 0xdd, 0x60, 0x6a, 0x28, 0x03, 0x44, 0x89,
  0x64, 0x24, 0x78, 0xc7, 0x44, 0x24, 0x20, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x0d, 0xbd, 0x19,
  0x44, 0x02, 0x4c, 0x8d, 0x44, 0x24, 0x78, 0x41, 0xb9, 0x04, 0x00, 0x00, 0x00, 0x49, 0x8b, 0xd6,
  0x48, 0x8b, 0x01, 0xff, 0x90, 0xe8, 0x00, 0x00, 0x00, 0x85, 0xc0, 0x78, 0x08, 0x8b, 0x44, 0x24,
  0x78, 0x85, 0xc0, 0x75, 0x12, 0xb9, 0x01, 0x00, 0x00, 0x00, 0xff, 0x15, 0x9f, 0x86, 0x90, 0x00,
};

void TestGpuQueryPollPattern()
{
  using skydiag::dump_tool::LooksLikeGpuQueryPoll;
  assert(LooksLikeGpuQueryPoll(kFieldQueryPoll));

  auto otherSlot = kFieldQueryPoll;  // vtable +0xE0 (End) is not GetData
  otherSlot[69] = 0xe0;
  assert(!LooksLikeGpuQueryPoll(otherSlot));

  auto notACall = kFieldQueryPoll;  // the return address does not follow a call
  notACall[90] = 0x90;
  assert(!LooksLikeGpuQueryPoll(notACall));

  // call rel32 instead of the import-table call also counts.
  auto rel32 = kFieldQueryPoll;
  rel32[90] = 0x90;
  rel32[91] = 0xe8;
  assert(LooksLikeGpuQueryPoll(rel32));

  assert(!LooksLikeGpuQueryPoll({}));
  assert(!LooksLikeGpuQueryPoll({ 0xff, 0x15, 0x00, 0x00, 0x00, 0x00 }));
}

void TestEngineWaitNamesTheGpuQueryPoll()
{
  auto caller = GameExe();
  caller.code_before = kFieldQueryPoll;
  const std::vector<ModalStackFrame> frames = {
    System(L"ntdll.dll", L"NtDelayExecution"),
    System(L"KERNELBASE.dll", L"SleepEx", 0x91),
    caller,
    Plugin(L"EngineFixes.dll"),
  };
  const auto wait = ClassifyMainThreadWait(frames);
  assert(wait.kind == "engine_wait");
  assert(wait.engine_wait_detail == "gpu_query_poll");
  assert(skydiag::dump_tool::IsGpuWait(wait));

  // Without the code bytes (dump lacks them) it stays a plain engine wait.
  auto plain = frames;
  plain[2].code_before.clear();
  const auto unknownCode = ClassifyMainThreadWait(plain);
  assert(unknownCode.kind == "engine_wait" && unknownCode.engine_wait_detail.empty());
  assert(!skydiag::dump_tool::IsGpuWait(unknownCode));

  // A sync wait from the same code is not the Sleep poll.
  auto sync = frames;
  sync[0] = System(L"ntdll.dll", L"NtWaitForSingleObject");
  sync[1] = System(L"KERNELBASE.dll", L"WaitForSingleObjectEx", 0xaf);
  assert(ClassifyMainThreadWait(sync).engine_wait_detail.empty());
}

}  // namespace

int main()
{
  TestEngineSleepWaitInRenderPass();
  TestGraphicsDriverWaitWithShortStack();
  TestPluginWaitAndRunning();
  TestUnknownWhenTheTopCannotBeRead();
  TestGpuQueryPollPattern();
  TestEngineWaitNamesTheGpuQueryPoll();
  return 0;
}
