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

  // A vtable +0xE8 call whose result is not checked is some other object.
  auto unchecked = kFieldQueryPoll;
  unchecked[73] = 0x90;  // test eax,eax -> nop nop
  unchecked[74] = 0x90;
  assert(!LooksLikeGpuQueryPoll(unchecked));

  // cmp eax,1 (S_FALSE) also checks it.
  std::vector<std::uint8_t> cmpS_False(kFieldQueryPoll.begin(), kFieldQueryPoll.begin() + 73);
  cmpS_False.insert(cmpS_False.end(), { 0x83, 0xf8, 0x01, 0x74, 0x02 });
  cmpS_False.insert(cmpS_False.end(), { 0xb9, 0x01, 0x00, 0x00, 0x00, 0xff, 0x15, 0x9f, 0x86, 0x90, 0x00 });
  assert(LooksLikeGpuQueryPoll(cmpS_False));

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

// The main thread waiting for game threads, as in the 2026-10-12 crash test
// (tools/crash_tester): the window/render thread sits in the engine's
// WaitForSingleObjectEx while a BSJobs job thread runs SKSE tasks.
skydiag::dump_tool::MainThreadWaitInfo EngineSyncWait()
{
  const std::vector<ModalStackFrame> frames = {
    System(L"ntdll.dll", L"NtWaitForSingleObject"),
    System(L"KERNELBASE.dll", L"WaitForSingleObjectEx", 0xaf),
    GameExe(),
    GameExe(),
    Plugin(L"hdtsmp64.dll", L"FSMP"),
    GameExe(),
  };
  return ClassifyMainThreadWait(frames);
}

skydiag::dump_tool::ThreadTopFrames Thread(std::uint32_t tid, std::vector<ModalStackFrame> frames)
{
  return skydiag::dump_tool::ThreadTopFrames{ tid, std::move(frames) };
}

// Hitch Hunter's hooked game thread waiting for work: thread start, the
// game's thread function, the plugin, a sync wait. Present in every capture.
skydiag::dump_tool::ThreadTopFrames IdleHookedThread(std::uint32_t tid)
{
  return Thread(tid, {
    System(L"ntdll.dll", L"NtWaitForSingleObject"),
    System(L"KERNELBASE.dll", L"WaitForSingleObjectEx", 0xaf),
    Plugin(L"HitchHunter.dll", L"Hitch Hunter And Fixes"),
    GameExe(),
    GameExe(),
    System(L"kernel32.dll", L"BaseThreadInitThunk", 0x17),
    System(L"ntdll.dll", L"RtlUserThreadStart", 0x2c),
  });
}

void TestBlockedGameThreadIsFound()
{
  using skydiag::dump_tool::FindBlockedGameThread;
  using skydiag::dump_tool::HasBlockedGameThread;

  auto wait = EngineSyncWait();
  assert(wait.kind == "engine_wait" && wait.wait_class == "sync");
  // Freeze 1: a job thread sleeping inside CrashTester.dll's SKSE task.
  FindBlockedGameThread(wait, {
    IdleHookedThread(9644),
    Thread(40400, {
      System(L"ntdll.dll", L"ZwDelayExecution"),
      System(L"ntdll.dll", L"RtlDelayExecution", 0x34),
      System(L"KERNELBASE.dll", L"SleepEx", 0x91),
      Plugin(L"CrashTester.dll", L"CrashTester (dev test)"),
      Plugin(L"CrashTester.dll", L"CrashTester (dev test)"),
      Skse(),
    }),
  });
  assert(HasBlockedGameThread(wait));
  assert(wait.blocked_thread_tid == 40400u && wait.blocked_module == L"CrashTester.dll");
  assert(wait.blocked_mod_name == L"CrashTester (dev test)" && wait.blocked_state == "waiting_in_plugin");
  assert(wait.blocked_top == L"KERNELBASE.dll!SleepEx" && wait.blocked_thread_count == 1u);

  // Freeze 2: engine code the plugin called, spinning on a lock.
  auto spin = EngineSyncWait();
  FindBlockedGameThread(spin, {
    IdleHookedThread(37948),
    Thread(9532, { GameExe(), GameExe(), GameExe(), GameExe(), Plugin(L"CrashTester.dll"), Skse() }),
  });
  assert(spin.blocked_thread_tid == 9532u && spin.blocked_state == "engine_call_from_plugin");
  assert(spin.blocked_top == L"SkyrimSE.exe");

  // Waiting inside a plugin outranks a plugin merely running; a shallower
  // plugin frame wins a tie.
  auto ranked = EngineSyncWait();
  FindBlockedGameThread(ranked, {
    Thread(1, { Plugin(L"Busy.dll"), GameExe() }),
    Thread(2, { System(L"KERNELBASE.dll", L"SleepEx"), GameExe(), Plugin(L"Deep.dll") }),
    Thread(3, { System(L"KERNELBASE.dll", L"SleepEx"), Plugin(L"Shallow.dll") }),
  });
  assert(ranked.blocked_module == L"Shallow.dll" && ranked.blocked_thread_count == 3u);

  // Crash test 2026-10-13 (F10): a plugin spinning reads the clock in a loop;
  // a named non-wait call above it is the plugin running, not waiting.
  auto busy = EngineSyncWait();
  FindBlockedGameThread(busy, {
    Thread(28596, {
      System(L"msvcp140.dll", L"Query_perf_counter"),
      Plugin(L"CrashTester.dll", L"CrashTester (dev test)"),
      Skse(),
    }),
  });
  assert(busy.blocked_module == L"CrashTester.dll" && busy.blocked_state == "running_in_plugin");
  assert(busy.blocked_top == L"msvcp140.dll!Query_perf_counter");
  // An unnamed system frame may be a wait, so it is not called running.
  auto unnamed = EngineSyncWait();
  FindBlockedGameThread(unnamed, {
    Thread(7, { System(L"ntdll.dll", L""), Plugin(L"CrashTester.dll") }),
  });
  assert(unnamed.blocked_state == "waiting_in_plugin");
}

void TestBlockedGameThreadIsNotInvented()
{
  using skydiag::dump_tool::FindBlockedGameThread;
  using skydiag::dump_tool::HasBlockedGameThread;

  // A plugin-owned idle loop alone is not a lead.
  auto idle = EngineSyncWait();
  FindBlockedGameThread(idle, { IdleHookedThread(9644) });
  assert(!HasBlockedGameThread(idle));

  // The SKSE runtime, hook frameworks, the graphics stack and SkyrimDiag
  // itself are not plugin code here.
  auto framework = EngineSyncWait();
  auto hook = Plugin(L"EngineFixes.dll");
  hook.is_hook_framework = true;
  FindBlockedGameThread(framework, {
    Thread(1, { System(L"KERNELBASE.dll", L"SleepEx"), Skse(), GameExe() }),
    Thread(2, { System(L"KERNELBASE.dll", L"SleepEx"), hook, GameExe() }),
    Thread(3, { System(L"KERNELBASE.dll", L"SleepEx"), Plugin(L"nvwgf2umx.dll"), GameExe() }),
    Thread(4, { System(L"KERNELBASE.dll", L"SleepEx"), Plugin(L"SkyrimDiag.dll"), GameExe() }),
  });
  assert(!HasBlockedGameThread(framework));

  // Only when the main thread waits on a sync object in the engine: not for
  // the GPU poll (a Sleep) or a plugin's own wait.
  const ModalStackFrame sleepFrames[] = {
    System(L"ntdll.dll", L"NtDelayExecution"), System(L"KERNELBASE.dll", L"SleepEx"), GameExe(),
  };
  auto sleeping = ClassifyMainThreadWait({ std::begin(sleepFrames), std::end(sleepFrames) });
  const std::vector<skydiag::dump_tool::ThreadTopFrames> crashTester = {
    Thread(40400, { System(L"KERNELBASE.dll", L"SleepEx"), Plugin(L"CrashTester.dll") }),
  };
  FindBlockedGameThread(sleeping, crashTester);
  assert(!HasBlockedGameThread(sleeping));
  auto pluginWait = ClassifyMainThreadWait({
    System(L"ntdll.dll", L"NtWaitForSingleObject"), System(L"KERNELBASE.dll", L"WaitForSingleObjectEx"),
    Plugin(L"Other.dll"), GameExe() });
  assert(pluginWait.kind == "plugin_wait");
  FindBlockedGameThread(pluginWait, crashTester);
  assert(!HasBlockedGameThread(pluginWait));
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
  TestBlockedGameThreadIsFound();
  TestBlockedGameThreadIsNotInvented();
  return 0;
}
