// Plugins that only wrap a game thread's entry are not suspects for a fault
// deep in game code above them (ThreadEntryHooks.h). The Crash Logger call
// stack below is the one from the user's 2026-10-13 CTD: the game threw
// std::invalid_argument reading ContentCatalog.txt, and NPCWaterAIFix.dll and
// Trails.dll were blamed only because they hook InitTESThread.

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <string>
#include <vector>

#include "CrashLoggerParseCore.h"
#include "ThreadEntryHooks.h"

using skydiag::dump_tool::MarkThreadEntryHookFrames;
using skydiag::dump_tool::StackFrameKind;
namespace core = skydiag::dump_tool::crashlogger_core;

namespace {

constexpr auto G = StackFrameKind::kGame;
constexpr auto P = StackFrameKind::kPlugin;
constexpr auto S = StackFrameKind::kSystem;
constexpr auto U = StackFrameKind::kUnknown;

const char* kContentCatalogCrash =
  "CRASH TIME: 2026-10-13 11:34:03\n"
  "Skyrim SSE v1.6.1170\n"
  "CrashLoggerSSE v1-25-0-0 Aug 21 2026 09:19:20\n"
  "Unhandled exception \"C++ Exception\" at 0x7FFB541B483A KERNELBASE.dll+00C483A\tnop [rax+rax*1], eax\n"
  "C++ EXCEPTION:\n"
  "\tType: (std::invalid_argument*)\n"
  "\tInfo: invalid stoull argument\n"
  "\tThrow Location: MSVCP140.dll+0044E92\n"
  "POSSIBLE RELEVANT OBJECTS:\n"
  "\tRSP+7A0: (TESObjectSTAT*) [0x000F077B] (\"Skyrim.esm\")\n"
  "\tRSP+7D0: (PathingCell*)\n"
  "\tRSP+858: (InstalledContent*)\n"
  "\tRSP+898: (PlayerCharacter*) \"Prisoner\" [0x00000014] (\"Skyrim.esm\")\n"
  "PROCESS INFO:\n"
  "\tProcess ID: 40940\n"
  "CALL STACK ([P]robable / [S]tack scan):\n"
  "\t[ 0][P] 0x7FFB541B483A    KERNELBASE.dll+00C483A\n"
  "\t[ 1][P] 0x7FFB378755A9  VCRUNTIME140.dll+00055A9\n"
  "\t[ 2][P] 0x7FFB377F4E92      MSVCP140.dll+0044E92\n"
  "\t[ 3][P] 0x7FF6ABA95AE9      SkyrimSE.exe+1235AE9 -> 443464+0x119\tint3\n"
  "\t[ 4][P] 0x7FF6ABA958E0      SkyrimSE.exe+12358E0 -> 443459+0x1A0\tmov rdi, rax\n"
  "\t[ 5][P] 0x7FF6ABA95696      SkyrimSE.exe+1235696 -> 443456+0x56\tnop\n"
  "\t[ 6][P] 0x7FF6AB2B5B35      SkyrimSE.exe+0A55B35 -> 442824+0x6A5\tnop\n"
  "\t[ 7][P] 0x7FF6AB2B53DE      SkyrimSE.exe+0A553DE -> 442823+0x13E\tmovzx r12d, r12b\n"
  "\t[ 8][P] 0x7FF6AB2B311B      SkyrimSE.exe+0A5311B -> 442803+0x8B\ttest al, al\n"
  "\t[ 9][P] 0x7FF6AB2B1979      SkyrimSE.exe+0A51979 -> 442782+0xC9\tmov rbx, rax\n"
  "\t[10][P] 0x7FF6AAE5588A      SkyrimSE.exe+05F588A -> 442448+0x1A\tmov rcx, [rbx+0x20]\n"
  "\t[11][P] 0x7FF6AAEA2F22      SkyrimSE.exe+0642F22 -> 36553+0x352\tcall 0x00007FF6AAA68750\n"
  "\t[12][P] 0x7FF6AAEAC5BC      SkyrimSE.exe+064C5BC -> 36644+0x1C\txor eax, eax\n"
  "\t[13][P] 0x7FF9BE614153 NPCWaterAIFix.dll+0024153\n"
  "\t[14][P] 0x7FF99793FA07        Trails.dll+004FA07\tmov rcx, rbx\n"
  "\t[15][P] 0x7FF6AB530DBD      SkyrimSE.exe+0CD0DBD -> 68445+0x3D\tmov rcx, [0x00007FF6ADA2C318]\n"
  "\t[16][P] 0x7FFB55EACDF7      KERNEL32.DLL+002CDF7\n"
  "\t[17][P] 0x7FFB56F3EE4C         ntdll.dll+007EE4C\n"
  "\t[18][S] 0x7FFB546950D9      ucrtbase.dll+00050D9\n"
  "\t[19][S] 0x7FFAD3BD05D6   EngineFixes.dll+00F05D6\tmov r8d, [rdi+0x68]\n"
  "\n"
  "REGISTERS:\n"
  "\tRAX 0x0                (size_t) [0]\n";

std::vector<bool> Mark(const std::vector<StackFrameKind>& frames)
{
  return MarkThreadEntryHookFrames(frames);
}

void TestContentCatalogCrashShape()
{
  // system x3, game x10, two plugin hooks, the game's thread start, system x2
  const std::vector<StackFrameKind> frames = { S, S, S, G, G, G, G, G, G, G, G, G, G, P, P, G, S, S };
  const auto marked = Mark(frames);
  for (std::size_t i = 0; i < frames.size(); ++i) {
    assert(marked[i] == (i == 13 || i == 14));
  }
}

void TestPluginsThatAreNotThreadEntryHooks()
{
  // A plugin's own thread: nothing of the game below it.
  assert(!Mark({ G, G, G, P, S, S })[3]);
  // The main thread: WinMain's loop and the CRT start are two game frames.
  assert(!Mark({ G, G, G, P, G, G, S, S })[3]);
  // A plugin called from deep in a game thread (an SKSE task, the crash
  // test's engine fault): many game frames below it.
  assert(!Mark({ G, G, G, P, P, G, G, G, G, S, S })[3]);
  // Too few game frames above: the plugin is close to the fault.
  assert(!Mark({ S, G, G, P, G, S, S })[3]);
  // The walk did not reach the thread start (truncated or broken).
  assert(!Mark({ G, G, G, P, G })[3]);
  assert(!Mark({ G, G, G, P, G, U, S })[3]);
  assert(Mark({}).empty());
}

void TestCrashLoggerFrameSignalsSkipThreadEntryHooks()
{
  const auto signals = core::ParseCrashLoggerFrameSignalsAscii(kContentCatalogCrash);
  // Only [P] rows: the stack-scan rows (ucrtbase, EngineFixes) are not frames.
  assert(signals.probable_modules_in_order.size() == 18u);
  assert(signals.first_actionable_probable_module.empty());
  assert((signals.thread_entry_hook_modules == std::vector<std::string>{ "NPCWaterAIFix.dll", "Trails.dll" }));
  assert(core::ParseCrashLoggerFrameTopModulesAsciiLower(kContentCatalogCrash).empty());
  assert(core::ParseCrashLoggerTopModulesAsciiLower(kContentCatalogCrash).empty());
}

void TestCrashLoggerObjectTypes()
{
  const auto types = core::ParseCrashLoggerObjectTypesAscii(kContentCatalogCrash);
  const auto has = [&](const char* type) {
    for (const auto& t : types) {
      if (t == type) {
        return true;
      }
    }
    return false;
  };
  // InstalledContent has no plugin or name, so the object refs drop it.
  assert(has("InstalledContent"));
  assert(has("TESObjectSTAT"));
  assert(has("PlayerCharacter"));
  assert(has("PathingCell"));
  // The thrown type is named only under C++ EXCEPTION (CPP_BAD_ALLOC needs
  // std::bad_alloc from there); nothing comes from the CALL STACK.
  assert(has("std::invalid_argument"));
  assert(!has("NPCWaterAIFix.dll") && !has("Info:") && !has("MSVCP140.dll+0044E92"));
  const auto older = core::ParseCrashLoggerObjectTypesAscii(
    "C++ EXCEPTION:\n\tType: class std::bad_alloc\n\tInfo: bad allocation\n"
    "REGISTERS:\n\tRAX 0x0                (size_t) [0]\n\tRCX 0x1B5535AF2E0      (NiTriShape*)\n");
  // REGISTERS gives pointer types only, not the register names.
  assert((older == std::vector<std::string>{ "std::bad_alloc", "NiTriShape" }));
}

void TestPluginOnTheCrashingPathIsNoHook()
{
  // Hooks.dll wraps the thread entry and also sits on the crashing path: it
  // stays the first actionable module and is not reported as a hook.
  const std::string log =
    "CALL STACK ([P]robable / [S]tack scan):\n"
    "\t[ 0][P] 0x7FF6AB000001      SkyrimSE.exe+0000001\n"
    "\t[ 1][P] 0x7FF6AB000002      SkyrimSE.exe+0000002\n"
    "\t[ 2][P] 0x7FF6AB000003      SkyrimSE.exe+0000003\n"
    "\t[ 3][P] 0x7FF9BE000001         Hooks.dll+0000001\n"
    "\t[ 4][P] 0x7FF6AB000004      SkyrimSE.exe+0000004\n"
    "\t[ 5][P] 0x7FF6AB000005      SkyrimSE.exe+0000005\n"
    "\t[ 6][P] 0x7FF6AB000006      SkyrimSE.exe+0000006\n"
    "\t[ 7][P] 0x7FF9BE000002         Hooks.dll+0000002\n"
    "\t[ 8][P] 0x7FF9BE000003         Hooks.dll+0000003\n"
    "\t[ 9][P] 0x7FF6AB000007      SkyrimSE.exe+0000007\n"
    "\t[10][P] 0x7FFB55EACDF7      KERNEL32.DLL+002CDF7\n"
    "\t[11][P] 0x7FFB56F3EE4C         ntdll.dll+007EE4C\n"
    "\n";
  const auto signals = core::ParseCrashLoggerFrameSignalsAscii(log);
  assert(signals.probable_frame_is_thread_entry_hook[7] && signals.probable_frame_is_thread_entry_hook[8]);
  assert(!signals.probable_frame_is_thread_entry_hook[3]);
  assert(signals.thread_entry_hook_modules.empty());
  assert(signals.first_actionable_probable_module == "Hooks.dll");

  using skydiag::dump_tool::ThreadEntryHookOnlyModules;
  const std::vector<std::string> modules = { "A.dll", "B.dll", "A.dll" };
  assert((ThreadEntryHookOnlyModules(modules, { false, true, true }) == std::vector<std::string>{ "B.dll" }));
  assert((ThreadEntryHookOnlyModules(modules, { true, true, true }) == std::vector<std::string>{ "A.dll", "B.dll" }));
}

void TestThreadEntryHookIsNoStreak()
{
  // Four frames of one hook directly below three game frames: the longest
  // run in the stack, but it only wraps the thread entry.
  const std::string log =
    "CALL STACK ([P]robable / [S]tack scan):\n"
    "\t[ 0][P] 0x7FFB541B483A    KERNELBASE.dll+00C483A\n"
    "\t[ 1][P] 0x7FF6AB000001      SkyrimSE.exe+0000001\n"
    "\t[ 2][P] 0x7FF6AB000002      SkyrimSE.exe+0000002\n"
    "\t[ 3][P] 0x7FF6AB000003      SkyrimSE.exe+0000003\n"
    "\t[ 4][P] 0x7FF9BE000001         Hooks.dll+0000001\n"
    "\t[ 5][P] 0x7FF9BE000002         Hooks.dll+0000002\n"
    "\t[ 6][P] 0x7FF9BE000003         Hooks.dll+0000003\n"
    "\t[ 7][P] 0x7FF9BE000004         Hooks.dll+0000004\n"
    "\t[ 8][P] 0x7FF6AB000004      SkyrimSE.exe+0000004\n"
    "\t[ 9][P] 0x7FFB55EACDF7      KERNEL32.DLL+002CDF7\n"
    "\t[10][P] 0x7FFB56F3EE4C         ntdll.dll+007EE4C\n"
    "\n";
  const auto signals = core::ParseCrashLoggerFrameSignalsAscii(log);
  assert((signals.thread_entry_hook_modules == std::vector<std::string>{ "Hooks.dll" }));
  assert(signals.probable_streak_module == "SkyrimSE.exe" && signals.probable_streak_length == 3u);
}

void TestCrashLoggerStackMustReachThreadStart()
{
  // The hook shape, but the stack ends in ucrtbase.dll, not at the thread
  // start in ntdll.dll: what lies below is unknown, so nothing is excluded.
  const std::string log =
    "CALL STACK ([P]robable / [S]tack scan):\n"
    "\t[ 0][P] 0x7FF6AB000001      SkyrimSE.exe+0000001\n"
    "\t[ 1][P] 0x7FF6AB000002      SkyrimSE.exe+0000002\n"
    "\t[ 2][P] 0x7FF6AB000003      SkyrimSE.exe+0000003\n"
    "\t[ 3][P] 0x7FF9BE000001        BadHook.dll+0000001\n"
    "\t[ 4][P] 0x7FF6AB000004      SkyrimSE.exe+0000004\n"
    "\t[ 5][P] 0x7FFB546950D9      ucrtbase.dll+00050D9\n"
    "\n";
  const auto signals = core::ParseCrashLoggerFrameSignalsAscii(log);
  assert(!signals.probable_frame_is_thread_entry_hook[3]);
  assert(signals.thread_entry_hook_modules.empty());
  assert(signals.first_actionable_probable_module == "BadHook.dll");
}

void TestFrameWithoutModuleKeepsItsPlace()
{
  // Crash Logger prints a [P] row with no module for code outside every
  // module. Dropping it would make A.dll look like it sits on the game's
  // thread start routine.
  const std::string log =
    "CALL STACK ([P]robable / [S]tack scan):\n"
    "\t[ 0][P] 0x7FF6AB000001      SkyrimSE.exe+0000001\n"
    "\t[ 1][P] 0x7FF6AB000002      SkyrimSE.exe+0000002\n"
    "\t[ 2][P] 0x7FF6AB000003      SkyrimSE.exe+0000003\n"
    "\t[ 3][P] 0x7FF9BE000001             A.dll+0000001\n"
    "\t[ 4][P] 0x7FF6AB000004      SkyrimSE.exe+0000004\n"
    "\t[ 5][P] 0x12345678               \n"
    "\t[ 6][S] 0x7FF9BE000002             B.dll+0000002\n"
    "\t[ 7][P] 0x7FFB55EACDF7      KERNEL32.DLL+002CDF7\n"
    "\t[ 8][P] 0x7FFB56F3EE4C         ntdll.dll+007EE4C\n"
    "\n";
  const auto signals = core::ParseCrashLoggerFrameSignalsAscii(log);
  assert(signals.probable_modules_in_order.size() == 7u);
  assert(signals.probable_frame_is_thread_entry_hook.size() == 7u);
  assert(!signals.probable_frame_is_thread_entry_hook[3]);
  assert(signals.thread_entry_hook_modules.empty());
  assert(signals.first_actionable_probable_module == "A.dll");
}

}  // namespace

int main()
{
  TestContentCatalogCrashShape();
  TestPluginsThatAreNotThreadEntryHooks();
  TestCrashLoggerFrameSignalsSkipThreadEntryHooks();
  TestCrashLoggerObjectTypes();
  TestPluginOnTheCrashingPathIsNoHook();
  TestThreadEntryHookIsNoStreak();
  TestCrashLoggerStackMustReachThreadStart();
  TestFrameWithoutModuleKeepsItsPlace();
  return 0;
}
