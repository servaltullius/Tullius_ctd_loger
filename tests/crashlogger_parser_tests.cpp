#include "CrashLoggerParseCore.h"
#include "SourceGuardTestUtils.h"

#include <cassert>
#include <filesystem>
#include <string>
#include <vector>

using skydiag::dump_tool::LooksLikeCrashLoggerLogTextCore;
using skydiag::dump_tool::crashlogger_core::ParseCrashLoggerCppExceptionDetailsAscii;
using skydiag::dump_tool::crashlogger_core::ParseCrashLoggerIniCrashlogDirectoryAscii;
using skydiag::dump_tool::crashlogger_core::ParseCrashLoggerVersionAscii;
using skydiag::dump_tool::crashlogger_core::ParseCrashLoggerFrameSignalsAscii;
using skydiag::dump_tool::crashlogger_core::ParseCrashLoggerFrameTopModulesAsciiLower;
using skydiag::dump_tool::ParseCrashLoggerTopModulesAsciiLower;
using skydiag::dump_tool::crashlogger_core::TryExtractModulePlusOffsetTokenAscii;
using skydiag::dump_tool::crashlogger_core::IsSystemishModuleAsciiLower;
using skydiag::dump_tool::crashlogger_core::IsGameExeModuleAsciiLower;
using skydiag::dump_tool::crashlogger_core::TryExtractCompactTimestampFromStem;
using skydiag::dump_tool::crashlogger_core::TryExtractDashedTimestampFromStem;
using skydiag::dump_tool::crashlogger_core::ParsedTimestamp;
using skydiag::dump_tool::crashlogger_core::CrashLoggerFrameSignals;
using skydiag::dump_tool::crashlogger_core::CrashLoggerArtifactKind;
using skydiag::dump_tool::crashlogger_core::ClassifyCrashLoggerArtifactNameAscii;
using skydiag::dump_tool::crashlogger_core::IsBetterCrashLoggerPairCandidate;
using skydiag::dump_tool::crashlogger_core::IsCrashLoggerPairingDiffAllowed;
using skydiag::dump_tool::crashlogger_core::IsCrashLoggerPairingAmbiguous;
using skydiag::dump_tool::crashlogger_core::kCrashLoggerFileTimeTicksPerSecond;
using skydiag::tests::source_guard::AssertContains;
using skydiag::tests::source_guard::ProjectRoot;
using skydiag::tests::source_guard::ReadAllText;
using skydiag::tests::source_guard::ReadProjectText;

static std::string ReadCrashLoggerFrameFixture(const char* filename)
{
  return ReadAllText(ProjectRoot() / "tests" / "data" / "crashlogger_frame_cases" / filename);
}

static void Test_LooksLikeCrashLogger_CrashLog()
{
  const std::string s =
    "CrashLoggerSSE v1.17.0\n"
    "CRASH TIME: 2026-01-31 12:34:56\n"
    "PROBABLE CALL STACK:\n"
    "  SkyrimSE.exe+0x123\n";
  assert(LooksLikeCrashLoggerLogTextCore(s));
}

static void Test_LooksLikeCrashLogger_ThreadDump()
{
  const std::string s =
    "CrashLoggerSSE v1.17.0\n"
    "========================================\n"
    "THREAD DUMP (Manual Trigger)\n"
    "========================================\n"
    "TIME: 2026-01-31 12:34:56\n"
    "\tCALLSTACK:\n"
    "\t\tPrismaUI.dll+0x1234\n";
  assert(LooksLikeCrashLoggerLogTextCore(s));
}

static void Test_LooksLikeCrashLogger_RuntimeLogHotkeyLineIsNotThreadDump()
{
  const std::string s =
    "[12:07:32.476] [41376] [I] CrashLogger v1-24-0-0\n"
    "[12:07:32.476] [41376] [I] installed crash handlers\n"
    "[12:07:32.476] [41376] [I] Thread dump hotkey monitoring started (Ctrl+Shift+F12)\n"
    "[12:07:32.476] [41376] [I] CrashLogger has finished loading.\n";

  assert(!LooksLikeCrashLoggerLogTextCore(s));
}

static void Test_ClassifyCrashLoggerArtifactName()
{
  assert(ClassifyCrashLoggerArtifactNameAscii("crash-2026-07-13-12-34-56.log") ==
         CrashLoggerArtifactKind::kCrash);
  assert(ClassifyCrashLoggerArtifactNameAscii("C:/logs/CRASH_20260713_123456.txt") ==
         CrashLoggerArtifactKind::kCrash);
  assert(ClassifyCrashLoggerArtifactNameAscii("threaddump-2026-07-13-12-34-56.log") ==
         CrashLoggerArtifactKind::kThreadDump);
  assert(ClassifyCrashLoggerArtifactNameAscii("THREAD_DUMP_20260713_123456.txt") ==
         CrashLoggerArtifactKind::kThreadDump);
  assert(ClassifyCrashLoggerArtifactNameAscii("renamed-diagnostic.log") ==
         CrashLoggerArtifactKind::kUnknown);
}

static void Test_CrashLoggerPairRankingPrioritizesTimeThenProvenance()
{
  const auto second = kCrashLoggerFileTimeTicksPerSecond;
  assert(IsCrashLoggerPairingDiffAllowed(120u * second));
  assert(!IsCrashLoggerPairingDiffAllowed(120u * second + 1u));
  assert(IsCrashLoggerPairingAmbiguous(0u, 2u * second));
  assert(!IsCrashLoggerPairingAmbiguous(0u, 2u * second + 1u));

  // A renamed but exactly contemporaneous valid log is better than a
  // conventionally named log from nearly two minutes earlier.
  assert(IsBetterCrashLoggerPairCandidate(
    true,
    0u,
    CrashLoggerArtifactKind::kUnknown,
    false,
    L"z-renamed.log",
    119u * second,
    CrashLoggerArtifactKind::kCrash,
    false,
    L"a-crash.log"));

  // With equal timestamps, known incident kind and co-location are useful,
  // deterministic provenance tie-breakers.
  assert(IsBetterCrashLoggerPairCandidate(
    true,
    second,
    CrashLoggerArtifactKind::kCrash,
    false,
    L"z-crash.log",
    second,
    CrashLoggerArtifactKind::kUnknown,
    true,
    L"a-renamed.log"));
  assert(IsBetterCrashLoggerPairCandidate(
    true,
    second,
    CrashLoggerArtifactKind::kCrash,
    true,
    L"z-crash.log",
    second,
    CrashLoggerArtifactKind::kCrash,
    false,
    L"a-crash.log"));
}

static void Test_ParseTopModules_CrashLog()
{
  const std::string s =
    "CrashLoggerSSE v1.17.0\n"
    "CRASH TIME: 2026-01-31 12:34:56\n"
    "PROBABLE CALL STACK:\n"
    "  PrismaUI.dll+0x111\n"
    "  PrismaUI.dll+0x222\n"
    "  kernelbase.dll+0x333\n"
    "MODULES:\n";

  const auto mods = ParseCrashLoggerTopModulesAsciiLower(s);
  assert(mods.size() == 1);
  assert(mods[0] == "prismaui.dll");
}

static void Test_ParseTopModules_CrashLog_NewCallstackFormat()
{
  // CrashLoggerSSE (v1.19+/v1.20) prints indexed callstack rows with an address column:
  //   [0] 0x00007FF... SomeMod.dll+0000123 <details>
  // We only care about extracting module names reliably.
  const std::string s =
    "CrashLoggerSSE v1.20.0 Feb 10 2026 04:25:35\n"
    "CRASH TIME: 2026-02-10 12:34:56\n"
    "PROBABLE CALL STACK:\n"
    "\t[ 0] 0x00007FF612345678 ExampleMod.dll+0000123\tmov eax, eax | ExampleFunc\n"
    "\t[ 1] 0x00007FF612345678 kernel32.dll+0000123\n"
    "\t[ 2] 0x00007FF612345678 SkyrimSE.exe+0000123\n"
    "\n"
    "REGISTERS:\n";

  const auto mods = ParseCrashLoggerTopModulesAsciiLower(s);
  assert(mods.size() == 1);
  assert(mods[0] == "examplemod.dll");
}

static void Test_ParseTopModules_ThreadDump()
{
  const std::string s =
    "CrashLoggerSSE v1.17.0\n"
    "THREAD DUMP (Manual Trigger)\n"
    "===== THREAD 1 (ID: 123) =====\n"
    "\tCALLSTACK:\n"
    "\t\tPrismaUI.dll+0x111\n"
    "\t\tntdll.dll+0x222\n"
    "\n"
    "===== THREAD 2 (ID: 456) =====\n"
    "\tCALLSTACK:\n"
    "\t\tSanguineSymphony.dll+0x333\n";

  const auto mods = ParseCrashLoggerTopModulesAsciiLower(s);
  assert(mods.size() == 2);
  assert(mods[0] == "prismaui.dll");
  assert(mods[1] == "sanguinesymphony.dll");
}

static void Test_ParseTopModules_ThreadDump_NewCallstackFormat()
{
  const std::string s =
    "CrashLoggerSSE v1.20.0 Feb 10 2026 04:25:35\n"
    "========================================\n"
    "THREAD DUMP (Manual Trigger)\n"
    "========================================\n"
    "TIME: 2026-02-10 12:34:56\n"
    "\tCALLSTACK:\n"
    "\t[ 0] 0x00007FF612345678 PrismaUI.dll+0000123\tmov eax, eax\n"
    "\t[ 1] 0x00007FF612345678 ntdll.dll+0000123\n"
    "\n"
    "===== THREAD 2 (ID: 456) =====\n"
    "\tCALLSTACK:\n"
    "\t[ 0] 0x00007FF612345678 SanguineSymphony.dll+0000123\n";

  const auto mods = ParseCrashLoggerTopModulesAsciiLower(s);
  assert(mods.size() == 2);
  assert(mods[0] == "prismaui.dll");
  assert(mods[1] == "sanguinesymphony.dll");
}

static void Test_ParseTopModules_ThreadDump_TieBreaksAlphabetically()
{
  const std::string s =
    "CrashLoggerSSE v1.18.0\n"
    "THREAD DUMP (Manual Trigger)\n"
    "===== THREAD 1 (ID: 123) =====\n"
    "\tCALLSTACK:\n"
    "\t\tmoda.dll+0x222\n"
    "\t\tmodb.dll+0x333\n";

  const auto mods = ParseCrashLoggerTopModulesAsciiLower(s);
  assert(mods.size() == 2);
  assert(mods[0] == "moda.dll");
  assert(mods[1] == "modb.dll");
}

static void Test_StackCorruptionWarning_DoesNotCrash()
{
  const std::string s =
    "CrashLoggerSSE v1.17.0\n"
    "CRASH TIME: 2026-01-31 12:34:56\n"
    "PROBABLE CALL STACK:\n"
    "WARNING: Stack trace capture failed - the call stack was likely corrupted.\n"
    "MODULES:\n";

  const auto mods = ParseCrashLoggerTopModulesAsciiLower(s);
  assert(mods.empty());
}

static void Test_ParseCppExceptionDetails()
{
  const std::string s =
    "CrashLoggerSSE v1.18.0\n"
    "CRASH TIME: 2026-02-02 12:34:56\n"
    "PROBABLE CALL STACK:\n"
    "  SomeMod.dll+0x111\n"
    "  kernelbase.dll+0x222\n"
    "\n"
    "C++ EXCEPTION:\n"
    "\tType: std::runtime_error\n"
    "\tInfo: HRESULT 0x887A0005 (DXGI_ERROR_DEVICE_REMOVED)\n"
    "\tThrow Location: SomeMod.dll+0x1234\n"
    "\tModule: SomeMod.dll\n"
    "\n";

  const auto ex = ParseCrashLoggerCppExceptionDetailsAscii(s);
  assert(ex);
  assert(ex->type == "std::runtime_error");
  assert(ex->info == "HRESULT 0x887A0005 (DXGI_ERROR_DEVICE_REMOVED)");
  assert(ex->throw_location == "SomeMod.dll+0x1234");
  assert(ex->module == "SomeMod.dll");
}

static void Test_ParseCppExceptionDetails_WithFlexibleSpacing()
{
  const std::string s =
    "CrashLoggerSSE v1.18.0\n"
    "CRASH TIME: 2026-02-06 03:21:00\n"
    "PROBABLE CALL STACK:\n"
    "  AnotherMod.dll+0x10\n"
    "\n"
    "C++ EXCEPTION:\n"
    " \tType:   std::bad_alloc   \n"
    "\tInfo:    allocation failed \n"
    " \tThrow Location:   AnotherMod.dll+0x4567  \n"
    "\tModule:   AnotherMod.dll   \n"
    "\n";

  const auto ex = ParseCrashLoggerCppExceptionDetailsAscii(s);
  assert(ex);
  assert(ex->type == "std::bad_alloc");
  assert(ex->info == "allocation failed");
  assert(ex->throw_location == "AnotherMod.dll+0x4567");
  assert(ex->module == "AnotherMod.dll");
}

static void Test_ParseCrashLoggerVersion()
{
  const std::string s =
    "CrashLoggerSSE v1.19.0\n"
    "CRASH TIME: 2026-02-07 12:34:56\n";

  const auto ver = ParseCrashLoggerVersionAscii(s);
  assert(ver);
  assert(*ver == "v1.19.0");
}

static void Test_ParseCrashLoggerVersion_WithBuildTime()
{
  const std::string s =
    "CrashLoggerSSE v1.20.0 Feb 10 2026 04:25:35\n"
    "CRASH TIME: 2026-02-10 12:34:56\n";

  const auto ver = ParseCrashLoggerVersionAscii(s);
  assert(ver);
  assert(*ver == "v1.20.0");
}

static void Test_ParseCrashLoggerVersion_WithHyphensAndBuildTime()
{
  const std::string s =
    "CrashLoggerSSE v1-20-0-0 Feb 10 2026 04:25:35\n"
    "CRASH TIME: 2026-02-10 12:34:56\n";

  const auto ver = ParseCrashLoggerVersionAscii(s);
  assert(ver);
  assert(*ver == "v1-20-0-0");
}

static void Test_ParseCrashLoggerVersion_WithFourPartDottedVersion()
{
  const std::string s =
    "CrashLoggerSSE v1.20.0.0 Feb 10 2026 04:25:35\n"
    "CRASH TIME: 2026-02-10 12:34:56\n";

  const auto ver = ParseCrashLoggerVersionAscii(s);
  assert(ver);
  assert(*ver == "v1.20.0.0");
}

static void Test_ParseCrashLoggerIni_CrashlogDirectory_Basic()
{
  const std::string s =
    "; comment\n"
    "[Debug]\n"
    "Crashlog Directory=C:\\\\Logs\\\\CrashLogger\n";

  const auto dir = ParseCrashLoggerIniCrashlogDirectoryAscii(s);
  assert(dir);
  assert(*dir == "C:\\\\Logs\\\\CrashLogger");
}

static void Test_ParseCrashLoggerIni_CrashlogDirectory_QuotedAndSpaced()
{
  const std::string s =
    "[Other]\n"
    "Crashlog Directory=C:\\\\Wrong\n"
    "\n"
    "[debug]\n"
    " Crashlog Directory =  \"D:\\\\Custom Logs\\\\CrashLogger\"  \n";

  const auto dir = ParseCrashLoggerIniCrashlogDirectoryAscii(s);
  assert(dir);
  assert(*dir == "D:\\\\Custom Logs\\\\CrashLogger");
}

static void Test_ParseCrashLoggerIni_CrashlogDirectory_EmptyIsNone()
{
  const std::string s =
    "[Debug]\n"
    "Crashlog Directory=\n";

  const auto dir = ParseCrashLoggerIniCrashlogDirectoryAscii(s);
  assert(!dir);
}

static void Test_ParseTopModules_ThreadDump_FiltersSystemAndGameExe()
{
  const std::string s =
    "CrashLoggerSSE v1.18.0\n"
    "THREAD DUMP (Manual Trigger)\n"
    "===== THREAD 1 (ID: 123) =====\n"
    "\tCALLSTACK:\n"
    "\t\tSkyrimSE.exe+0x100\n"
    "\t\tkernel32.dll+0x200\n"
    "\t\tUsefulMod.dll+0x300\n"
    "\t\tUsefulMod.dll+0x400\n";

  const auto mods = ParseCrashLoggerTopModulesAsciiLower(s);
  assert(mods.size() == 1);
  assert(mods[0] == "usefulmod.dll");
}

// ── Group 1: LooksLikeCrashLogger edge cases ──

static void Test_LooksLikeCrashLogger_ProcessInfo()
{
  const std::string s =
    "CrashLoggerSSE v1.20.0\n"
    "PROCESS INFO:\n"
    "  SkyrimSE.exe version 1.6.1170\n";
  assert(LooksLikeCrashLoggerLogTextCore(s));
}

static void Test_LooksLikeCrashLogger_NotCrashLogger()
{
  const std::string s = "Some random log file\nWith multiple lines\n";
  assert(!LooksLikeCrashLoggerLogTextCore(s));
}

static void Test_LooksLikeCrashLogger_Empty()
{
  assert(!LooksLikeCrashLoggerLogTextCore(""));
}

// ── Group 2: ParseVersion edge cases ──

static void Test_ParseCrashLoggerVersion_Missing()
{
  const std::string s = "Some random text\nNo version here\n";
  const auto ver = ParseCrashLoggerVersionAscii(s);
  assert(!ver);
}

static void Test_ParseCrashLoggerVersion_Empty()
{
  const auto ver = ParseCrashLoggerVersionAscii("");
  assert(!ver);
}

// ── Group 3: C++ Exception edge cases ──

static void Test_ParseCppExceptionDetails_NoBlock()
{
  const std::string s =
    "CrashLoggerSSE v1.18.0\n"
    "CRASH TIME: 2026-02-23 12:34:56\n"
    "PROBABLE CALL STACK:\n"
    "  SomeMod.dll+0x111\n"
    "\n"
    "REGISTERS:\n";
  const auto ex = ParseCrashLoggerCppExceptionDetailsAscii(s);
  assert(!ex);
}

// ── Group 4: TopModules edge cases ──

static void Test_ParseTopModules_EmptyInput()
{
  const auto mods = ParseCrashLoggerTopModulesAsciiLower("");
  assert(mods.empty());
}

static void Test_ParseTopModules_CrashLog_NoModulesOnlyWarning()
{
  const std::string s =
    "CrashLoggerSSE v1.17.0\n"
    "CRASH TIME: 2026-01-31 12:34:56\n"
    "PROBABLE CALL STACK:\n"
    "WARNING: Stack trace capture failed - the call stack was likely corrupted.\n"
    "REGISTERS:\n";
  const auto mods = ParseCrashLoggerTopModulesAsciiLower(s);
  assert(mods.empty());
}

static void Test_ParseTopModules_ManyModules_CappedAt8()
{
  std::string s =
    "CrashLoggerSSE v1.17.0\n"
    "CRASH TIME: 2026-02-23 12:34:56\n"
    "PROBABLE CALL STACK:\n";
  for (int i = 0; i < 12; ++i) {
    s += "  mod" + std::to_string(i) + ".dll+0x100\n";
  }
  s += "REGISTERS:\n";
  const auto mods = ParseCrashLoggerTopModulesAsciiLower(s);
  assert(mods.size() <= 8);
}

// ── Group 5: INI parsing edge cases ──

// Crash Logger (SimpleIni) and the helper treat only whole lines starting
// with '#' or ';' as comments: a '#' or ';' later in the value is part of the
// folder name (review M23).
static void Test_ParseCrashLoggerIni_HashComment()
{
  const std::string s =
    "# comment line\n"
    "[Debug]\n"
    "; Crashlog Directory=C:\\Commented\n"
    "Crashlog Directory=D:\\Games\\Skyrim #2\\CrashLogs; old\n";
  const auto dir = ParseCrashLoggerIniCrashlogDirectoryAscii(s);
  assert(dir);
  assert(*dir == "D:\\Games\\Skyrim #2\\CrashLogs; old");
}

static void Test_ParseCrashLoggerIni_LastKeyWinsAndBom()
{
  const std::string s =
    "\xEF\xBB\xBF[Debug]\r\n"
    "Crashlog Directory=C:\\First\r\n"
    "[General]\r\n"
    "Crashlog Directory=C:\\Wrong\r\n"
    "[ DEBUG ] ; trailing text after the section name\r\n"
    "crashlog directory = \"E:\\Last Logs\"\r\n";
  const auto dir = ParseCrashLoggerIniCrashlogDirectoryAscii(s);
  assert(dir);
  assert(*dir == "E:\\Last Logs");

  // A later empty value unsets an earlier one; an unpaired quote stays.
  assert(!ParseCrashLoggerIniCrashlogDirectoryAscii("[Debug]\nCrashlog Directory=C:\\A\nCrashlog Directory=\n"));
  const auto unpaired = ParseCrashLoggerIniCrashlogDirectoryAscii("[Debug]\nCrashlog Directory=\"C:\\A\n");
  assert(unpaired && *unpaired == "\"C:\\A");
}

static void Test_ParseCrashLoggerIni_NoDebugSection()
{
  const std::string s =
    "[General]\n"
    "Crashlog Directory=C:\\\\Wrong\n";
  const auto dir = ParseCrashLoggerIniCrashlogDirectoryAscii(s);
  assert(!dir);
}

// ── Group 6: TryExtractModulePlusOffsetTokenAscii direct tests ──

static void Test_TryExtractToken_ValidDll()
{
  const auto tok = TryExtractModulePlusOffsetTokenAscii("  ExampleMod.dll+0x1234");
  assert(tok.has_value());
  assert(tok->find("ExampleMod.dll+") != std::string_view::npos);
}

static void Test_TryExtractToken_ValidExe()
{
  const auto tok = TryExtractModulePlusOffsetTokenAscii("SkyrimSE.exe+0xABCD");
  assert(tok.has_value());
  assert(tok->find("SkyrimSE.exe+") != std::string_view::npos);
}

static void Test_TryExtractToken_NoModule()
{
  const auto tok = TryExtractModulePlusOffsetTokenAscii("just some text");
  assert(!tok.has_value());
}

static void Test_TryExtractToken_Empty()
{
  const auto tok = TryExtractModulePlusOffsetTokenAscii("");
  assert(!tok.has_value());
}

static void Test_TryExtractToken_NewFormat()
{
  const auto tok = TryExtractModulePlusOffsetTokenAscii("\t[ 0] 0x00007FF612345678 ExampleMod.dll+0000123\tmov eax, eax");
  assert(tok.has_value());
  assert(tok->find("ExampleMod.dll+") != std::string_view::npos);
}

// ── Group 7: IsSystemish/IsGameExe direct tests ──

static void Test_IsSystemish_KnownModules()
{
  assert(IsSystemishModuleAsciiLower("kernelbase.dll"));
  assert(IsSystemishModuleAsciiLower("ntdll.dll"));
  assert(IsSystemishModuleAsciiLower("kernel32.dll"));
  assert(IsSystemishModuleAsciiLower("ucrtbase.dll"));
  assert(IsSystemishModuleAsciiLower("user32.dll"));
  assert(IsSystemishModuleAsciiLower("win32u.dll"));
  // Graphics / DirectX (synced with MinidumpUtil.cpp)
  assert(IsSystemishModuleAsciiLower("d3d11.dll"));
  assert(IsSystemishModuleAsciiLower("d3d12.dll"));
  assert(IsSystemishModuleAsciiLower("dxgi.dll"));
  assert(IsSystemishModuleAsciiLower("d3d9.dll"));
  assert(IsSystemishModuleAsciiLower("opengl32.dll"));
  assert(IsSystemishModuleAsciiLower("d3dcompiler_47.dll"));
  assert(IsSystemishModuleAsciiLower("dxcore.dll"));
  // Debugging
  assert(IsSystemishModuleAsciiLower("dbghelp.dll"));
  assert(IsSystemishModuleAsciiLower("dbgcore.dll"));
  // Additional Windows core
  assert(IsSystemishModuleAsciiLower("advapi32.dll"));
  assert(IsSystemishModuleAsciiLower("rpcrt4.dll"));
  assert(IsSystemishModuleAsciiLower("sechost.dll"));
}

static void Test_IsSystemish_NotSystem()
{
  assert(!IsSystemishModuleAsciiLower("mymod.dll"));
  assert(!IsSystemishModuleAsciiLower("hdtsmp64.dll"));
  assert(!IsSystemishModuleAsciiLower(""));
}

static void Test_IsGameExe_KnownExes()
{
  assert(IsGameExeModuleAsciiLower("skyrimse.exe"));
  assert(IsGameExeModuleAsciiLower("skyrimae.exe"));
  assert(IsGameExeModuleAsciiLower("skyrimvr.exe"));
  assert(IsGameExeModuleAsciiLower("skyrim.exe"));
}

static void Test_IsGameExe_NotGameExe()
{
  assert(!IsGameExeModuleAsciiLower("mymod.dll"));
  assert(!IsGameExeModuleAsciiLower(""));
  assert(!IsGameExeModuleAsciiLower("skyrimse.dll"));
}

// ── Group 8: Timestamp parsing ──

static void Test_CompactTimestamp_Standard()
{
  const auto ts = TryExtractCompactTimestampFromStem(L"SkyrimDiag_crash_20260215_143022");
  assert(ts.has_value());
  assert(ts->year == 2026);
  assert(ts->month == 2);
  assert(ts->day == 15);
  assert(ts->hour == 14);
  assert(ts->minute == 30);
  assert(ts->second == 22);
}

static void Test_CompactTimestamp_EmbeddedInLongName()
{
  const auto ts = TryExtractCompactTimestampFromStem(L"prefix_stuff_20251231_235959_suffix");
  assert(ts.has_value());
  assert(ts->year == 2025);
  assert(ts->month == 12);
  assert(ts->day == 31);
  assert(ts->hour == 23);
  assert(ts->minute == 59);
  assert(ts->second == 59);
}

static void Test_CompactTimestamp_NoMatch()
{
  const auto ts = TryExtractCompactTimestampFromStem(L"no_timestamp_here");
  assert(!ts.has_value());
}

static void Test_CompactTimestamp_Empty()
{
  const auto ts = TryExtractCompactTimestampFromStem(L"");
  assert(!ts.has_value());
}

static void Test_CompactTimestamp_PartialDigits()
{
  const auto ts = TryExtractCompactTimestampFromStem(L"2026021X_143022");
  assert(!ts.has_value());
}

static void Test_DashedTimestamp_Standard()
{
  const auto ts = TryExtractDashedTimestampFromStem(L"Crash-2026-02-15-14-30-22");
  assert(ts.has_value());
  assert(ts->year == 2026);
  assert(ts->month == 2);
  assert(ts->day == 15);
  assert(ts->hour == 14);
  assert(ts->minute == 30);
  assert(ts->second == 22);
}

static void Test_DashedTimestamp_InvalidMonth()
{
  const auto ts = TryExtractDashedTimestampFromStem(L"Crash-2026-13-15-14-30-22");
  assert(!ts.has_value());
}

static void Test_DashedTimestamp_NoMatch()
{
  const auto ts = TryExtractDashedTimestampFromStem(L"no_dashed_timestamp");
  assert(!ts.has_value());
}

static void Test_DashedTimestamp_Empty()
{
  const auto ts = TryExtractDashedTimestampFromStem(L"");
  assert(!ts.has_value());
}

static void Test_DashedTimestamp_InvalidHour()
{
  const auto ts = TryExtractDashedTimestampFromStem(L"Crash-2026-01-15-25-30-22");
  assert(!ts.has_value());
}

static void Test_CompactTimestamp_MultipleMatches_TakesFirst()
{
  // Two valid compact patterns; function should return the FIRST one.
  const auto ts = TryExtractCompactTimestampFromStem(L"20250101_000000_crash_20260215_143022");
  assert(ts.has_value());
  assert(ts->year == 2025);
  assert(ts->month == 1);
}

static void Test_CompactTimestamp_InvalidMonth()
{
  const auto ts = TryExtractCompactTimestampFromStem(L"20261301_143022");
  assert(!ts.has_value());
}

static void Test_CompactTimestamp_InvalidHour()
{
  const auto ts = TryExtractCompactTimestampFromStem(L"20260215_253022");
  assert(!ts.has_value());
}

// ── Group 9: ESP/ESM object reference parsing ──

using skydiag::dump_tool::crashlogger_core::CrashLoggerObjectRef;
using skydiag::dump_tool::crashlogger_core::ParseCrashLoggerObjectRefsAscii;
using skydiag::dump_tool::crashlogger_core::AggregateCrashLoggerObjectRefs;
using skydiag::dump_tool::crashlogger_core::IsVanillaDlcEspAsciiLower;
using skydiag::dump_tool::crashlogger_core::ExtractEspNamesFromLine;
using skydiag::dump_tool::crashlogger_core::ExtractFormIdBefore;
using skydiag::dump_tool::crashlogger_core::ExtractEspRefsFromLine;
using skydiag::dump_tool::crashlogger_core::EspRefEntry;

static void Test_ParseObjectRefs_BasicExample()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0\n"
    "CRASH TIME: 2026-03-05 00:09:15\n"
    "PROBABLE CALL STACK:\n"
    "\tSkyrimSE.exe+0x123\n"
    "\n"
    "POSSIBLE RELEVANT OBJECTS:\n"
    "\tRDI: (Character*) \"\xEB\x8F\x84\xEB\xA1\x9C\xEB\xA1\xB1\" [0xFEAD081B] (\"AE_StellarBlade_Doro.esp\")\n"
    "\tRSP+360: (TESObjectREFR*) [0x9D0F0C07] (\"DynDOLOD.esp\") [0x33133209] (DynDOLOD.esm)\n"
    "\n"
    "REGISTERS:\n";

  const auto refs = ParseCrashLoggerObjectRefsAscii(log);
  assert(!refs.empty());
  // Should find AE_StellarBlade_Doro.esp and DynDOLOD.esp and DynDOLOD.esm
  bool foundStellar = false, foundDynEsp = false, foundDynEsm = false;
  for (const auto& r : refs) {
    if (r.esp_name == "AE_StellarBlade_Doro.esp") foundStellar = true;
    if (r.esp_name == "DynDOLOD.esp") foundDynEsp = true;
    if (r.esp_name == "DynDOLOD.esm") foundDynEsm = true;
  }
  assert(foundStellar);
  assert(foundDynEsp);
  assert(foundDynEsm);
}

static void Test_ParseObjectRefs_NoEsp_SkipsLine()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0\n"
    "POSSIBLE RELEVANT OBJECTS:\n"
    "\tRDI: (BSFadeNode*) [0x12345678]\n"
    "\n"
    "REGISTERS:\n";

  const auto refs = ParseCrashLoggerObjectRefsAscii(log);
  assert(refs.empty());
}

static void Test_ParseObjectRefs_UnquotedEsp()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0\n"
    "POSSIBLE RELEVANT OBJECTS:\n"
    "\tRSP+360: (TESObjectREFR*) [0x9D0F0C07] (DynDOLOD.esm)\n"
    "\n"
    "REGISTERS:\n";

  const auto refs = ParseCrashLoggerObjectRefsAscii(log);
  assert(refs.size() == 1);
  assert(refs[0].esp_name == "DynDOLOD.esm");
}

static void Test_ParseObjectRefs_FiltersVanillaEsp()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0\n"
    "POSSIBLE RELEVANT OBJECTS:\n"
    "\tRDI: (TESObjectREFR*) [0x12345678] (\"Skyrim.esm\")\n"
    "\tRSI: (Character*) [0xABCD0001] (\"MyMod.esp\")\n"
    "\n"
    "REGISTERS:\n";

  const auto refs = ParseCrashLoggerObjectRefsAscii(log);
  // Skyrim.esm should be filtered
  assert(refs.size() == 1);
  assert(refs[0].esp_name == "MyMod.esp");
}

static void Test_ParseObjectRefs_ModifiedBySkipped()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0\n"
    "POSSIBLE RELEVANT OBJECTS:\n"
    "\tRDI: (Character*) [0xABCD0001] (\"MyMod.esp\")\n"
    "\tModified by: (\"OverhaulMod.esp\")\n"
    "\tModified by: (\"AnotherPatch.esp\")\n"
    "\n"
    "REGISTERS:\n";

  const auto refs = ParseCrashLoggerObjectRefsAscii(log);
  // Only MyMod.esp should be found, not the Modified by ones
  assert(refs.size() == 1);
  assert(refs[0].esp_name == "MyMod.esp");
}

// A column-0 "RDI: (...)" register line is a synthetic shape no real Crash
// Logger writes (they indent register lines; see the RealRegisterFormat tests);
// kept so the depth rule stays relative to the register line.
static void Test_ParseObjectRefs_RegisterFileField()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0\n"
    "PROBABLE CALL STACK:\n"
    "\tSkyrimSE.exe+0x1\n"
    "\n"
    "REGISTERS:\n"
    "RDI: (TESForm*) [0x12345678] (\"SomeForm.esp\")\n"
    "\tFile: \"DetailedMod.esp\"\n"
    "\n"
    "MODULES:\n";

  const auto refs = ParseCrashLoggerObjectRefsAscii(log);
  bool foundSome = false, foundDetailed = false;
  for (const auto& r : refs) {
    if (r.esp_name == "SomeForm.esp") foundSome = true;
    if (r.esp_name == "DetailedMod.esp") foundDetailed = true;
  }
  assert(foundSome);
  assert(foundDetailed);
}

static void Test_ParseObjectRefs_QuotedPluginNamesWithSpaces()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0\n"
    "POSSIBLE RELEVANT OBJECTS:\n"
    "\tRDI: (TESObjectREFR*) [0x12345678] (\"JK's Skyrim.esp\")\n"
    "\tFile: \"My Compatibility Patch.esm\"\n"
    "\n"
    "REGISTERS:\n";

  const auto refs = ParseCrashLoggerObjectRefsAscii(log);
  bool foundJks = false;
  bool foundPatch = false;
  for (const auto& ref : refs) {
    if (ref.esp_name == "JK's Skyrim.esp") foundJks = true;
    if (ref.esp_name == "My Compatibility Patch.esm") foundPatch = true;
  }
  assert(foundJks);
  assert(foundPatch);
}

static void Test_ParseObjectRefs_RegisterRanksHigher()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0\n"
    "POSSIBLE RELEVANT OBJECTS:\n"
    "\tRDI: (Character*) [0xABCD0001] (\"HighMod.esp\")\n"
    "\tRSP+360: (TESObjectREFR*) [0x12345678] (\"LowMod.esp\")\n"
    "\n"
    "REGISTERS:\n";

  const auto refs = ParseCrashLoggerObjectRefsAscii(log);
  assert(refs.size() == 2);
  // RDI (10) + Character* (8) = 18 should be higher than RSP+360 (3) + TESObjectREFR* (6) = 9
  assert(refs[0].esp_name == "HighMod.esp");
  assert(refs[0].relevance_score > refs[1].relevance_score);
}

static void Test_ParseObjectRefs_ActorRanksHigher()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0\n"
    "POSSIBLE RELEVANT OBJECTS:\n"
    "\tRDI: (BSFadeNode*) [0xABCD0001] (\"LowType.esp\")\n"
    "\tRSI: (Character*) [0x12345678] (\"HighType.esp\")\n"
    "\n"
    "REGISTERS:\n";

  const auto refs = ParseCrashLoggerObjectRefsAscii(log);
  assert(refs.size() == 2);
  // RSI (10) + Character* (8) = 18 vs RDI (10) + BSFadeNode* (2) = 12
  assert(refs[0].esp_name == "HighType.esp");
}

static void Test_AggregateObjectRefs_Dedup()
{
  std::vector<CrashLoggerObjectRef> refs;
  {
    CrashLoggerObjectRef r;
    r.location = "RDI"; r.object_type = "Character*"; r.esp_name = "MyMod.esp";
    r.object_name = "NPC1"; r.relevance_score = 18;
    refs.push_back(r);
  }
  {
    CrashLoggerObjectRef r;
    r.location = "RSP+68"; r.object_type = "TESObjectREFR*"; r.esp_name = "mymod.esp";
    r.relevance_score = 11;
    refs.push_back(r);
  }
  {
    CrashLoggerObjectRef r;
    r.location = "RSI"; r.object_type = "Character*"; r.esp_name = "OtherMod.esp";
    r.object_name = "NPC2"; r.relevance_score = 18;
    refs.push_back(r);
  }

  const auto agg = AggregateCrashLoggerObjectRefs(refs);
  // MyMod.esp and mymod.esp should merge
  assert(agg.size() == 2);
  // The merged one should have ref_count reflected in max_score (the highest)
  bool foundMyMod = false;
  for (const auto& a : agg) {
    const std::string lower = skydiag::dump_tool::crashlogger_core::AsciiLower(a.esp_name);
    if (lower == "mymod.esp") {
      foundMyMod = true;
      assert(a.relevance_score == 18);
      assert(a.object_name == "NPC1");
    }
  }
  assert(foundMyMod);
}

static void Test_ParseObjectRefs_EmptyInput()
{
  const auto refs = ParseCrashLoggerObjectRefsAscii("");
  assert(refs.empty());
}

static void Test_ParseObjectRefs_UnicodeObjectName()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0\n"
    "POSSIBLE RELEVANT OBJECTS:\n"
    "\tRDI: (Character*) \"\xEB\x8F\x84\xEB\xA1\x9C\xEB\xA1\xB1\" [0xFEAD081B] (\"AE_StellarBlade_Doro.esp\")\n"
    "\n"
    "REGISTERS:\n";

  const auto refs = ParseCrashLoggerObjectRefsAscii(log);
  assert(refs.size() == 1);
  assert(refs[0].object_name == "\xEB\x8F\x84\xEB\xA1\x9C\xEB\xA1\xB1"); // 도로롱
}

static void Test_ParseObjectRefs_MixedQuotedUnquoted()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0\n"
    "POSSIBLE RELEVANT OBJECTS:\n"
    "\tRSP+68: (TESObjectREFR*) [0x9D0F0C07] (\"QuotedMod.esp\") [0x33133209] (UnquotedMod.esm)\n"
    "\n"
    "REGISTERS:\n";

  const auto refs = ParseCrashLoggerObjectRefsAscii(log);
  assert(refs.size() == 2);
  bool foundQuoted = false, foundUnquoted = false;
  for (const auto& r : refs) {
    if (r.esp_name == "QuotedMod.esp") foundQuoted = true;
    if (r.esp_name == "UnquotedMod.esm") foundUnquoted = true;
  }
  assert(foundQuoted);
  assert(foundUnquoted);
}

static void Test_IsVanillaDlcEsp_AllKnown()
{
  assert(IsVanillaDlcEspAsciiLower("skyrim.esm"));
  assert(IsVanillaDlcEspAsciiLower("update.esm"));
  assert(IsVanillaDlcEspAsciiLower("dawnguard.esm"));
  assert(IsVanillaDlcEspAsciiLower("hearthfires.esm"));
  assert(IsVanillaDlcEspAsciiLower("dragonborn.esm"));
  assert(!IsVanillaDlcEspAsciiLower("mymod.esp"));
  assert(!IsVanillaDlcEspAsciiLower(""));
}

static void Test_ParseObjectRefs_MalformedQuotedParen_NoInfiniteLoop()
{
  // Malformed line with unclosed ("... — should not infinite loop
  const std::string log =
    "CrashLoggerSSE v1.20.0\n"
    "POSSIBLE RELEVANT OBJECTS:\n"
    "\tRDI: (Character*) (\"BrokenNoClose\n"
    "\tRSI: (TESObjectREFR*) [0xABCD0001] (\"GoodMod.esp\")\n"
    "\n"
    "REGISTERS:\n";

  // Should not hang; GoodMod.esp found on the next line
  const auto refs = ParseCrashLoggerObjectRefsAscii(log);
  bool foundGood = false;
  for (const auto& r : refs) {
    if (r.esp_name == "GoodMod.esp") foundGood = true;
  }
  assert(foundGood);
}

static void Test_ExtractEspNames_NoParens()
{
  // No parens at all — should return empty
  const auto names = ExtractEspNamesFromLine("just some text without parens");
  assert(names.empty());
}

static void Test_ParseObjectRefs_CCContentFiltered()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0\n"
    "POSSIBLE RELEVANT OBJECTS:\n"
    "\tRDI: (TESObjectREFR*) [0x12345678] (\"ccBGSSSE001-Fish.esm\")\n"
    "\tRSI: (Character*) [0xABCD0001] (\"RealMod.esp\")\n"
    "\n"
    "REGISTERS:\n";

  const auto refs = ParseCrashLoggerObjectRefsAscii(log);
  // CC content should be filtered
  assert(refs.size() == 1);
  assert(refs[0].esp_name == "RealMod.esp");
}

static const CrashLoggerObjectRef* FindObjectRefByEsp(
    const std::vector<CrashLoggerObjectRef>& refs,
    const std::string& espName)
{
  for (const auto& ref : refs) {
    if (ref.esp_name == espName) {
      return &ref;
    }
  }
  return nullptr;
}

static void Test_ParseObjectRefs_CrashLoggerV121V122SimplifiedIntrospection()
{
  const std::string log =
    "CrashLoggerSSE v1.22.0 May  2 2026 02:29:15\n"
    "CRASH TIME: 2026-05-04 19:22:00\n"
    "POSSIBLE RELEVANT OBJECTS:\n"
    "\tRDX: RE::SpellItem \"Chain Heal\" [0xFE123456] (MagicOverhaul.esp) {Spell, Concentration, Aimed}\n"
    "\tR8: RE::EffectSetting \"Bleeding Wound\" [0xFE234567] (CombatMagic.esl) {ValueModifier}\n"
    "\tR9: RE::BGSLocation \"Forgotten Crypt\" [0xFE345678] (LocationPatch.esp)\n"
    "\tR10: RE::NavMesh [0xFE456789] (NavmeshPatch.esm)\n"
    "\n"
    "REGISTERS:\n";

  const auto refs = ParseCrashLoggerObjectRefsAscii(log);
  assert(refs.size() == 4);

  const auto* spell = FindObjectRefByEsp(refs, "MagicOverhaul.esp");
  assert(spell);
  assert(spell->location == "RDX");
  assert(spell->object_type == "RE::SpellItem");
  assert(spell->object_name == "Chain Heal");
  assert(spell->form_id == "0xFE123456");

  const auto* effect = FindObjectRefByEsp(refs, "CombatMagic.esl");
  assert(effect);
  assert(effect->location == "R8");
  assert(effect->object_type == "RE::EffectSetting");
  assert(effect->object_name == "Bleeding Wound");
  assert(effect->form_id == "0xFE234567");

  const auto* location = FindObjectRefByEsp(refs, "LocationPatch.esp");
  assert(location);
  assert(location->object_type == "RE::BGSLocation");
  assert(location->object_name == "Forgotten Crypt");
  assert(location->form_id == "0xFE345678");

  const auto* navmesh = FindObjectRefByEsp(refs, "NavmeshPatch.esm");
  assert(navmesh);
  assert(navmesh->object_type == "RE::NavMesh");
  assert(navmesh->object_name.empty());
  assert(navmesh->form_id == "0xFE456789");
}

// ── Group 10: FormID extraction tests ──

static void Test_ExtractFormIdBefore_Basic()
{
  // "[0xFEAD081B] ("AE_StellarBlade_Doro.esp")"
  //               ^ pos at '('
  std::string_view line = R"([0xFEAD081B] ("AE_StellarBlade_Doro.esp"))";
  auto pos = line.find('(');
  auto fid = ExtractFormIdBefore(line, pos);
  assert(fid == "0xFEAD081B");
}

static void Test_ExtractFormIdBefore_NoFormId()
{
  std::string_view line = R"((Character*) ("SomeMod.esp"))";
  auto pos = line.find("(\"Some");
  auto fid = ExtractFormIdBefore(line, pos);
  assert(fid.empty());
}

static void Test_ExtractFormIdBefore_WithSpaces()
{
  std::string_view line = R"([0xABCD1234]  ("MyMod.esp"))";
  auto pos = line.find("(\"My");
  auto fid = ExtractFormIdBefore(line, pos);
  assert(fid == "0xABCD1234");
}

static void Test_ExtractFormIdBefore_AtStart()
{
  // No room before position 0
  auto fid = ExtractFormIdBefore("(test)", 0);
  assert(fid.empty());
}

static void Test_ExtractEspRefs_WithFormId()
{
  std::string_view line = R"(RDI: (Character*) "Doro" [0xFEAD081B] ("AE_StellarBlade_Doro.esp"))";
  auto refs = ExtractEspRefsFromLine(line);
  assert(refs.size() == 1);
  assert(refs[0].esp_name == "AE_StellarBlade_Doro.esp");
  assert(refs[0].form_id == "0xFEAD081B");
}

static void Test_ExtractEspRefs_MultipleEsps()
{
  std::string_view line = R"(RSP+360: (TESObjectREFR*) [0x9D0F0C07] ("DynDOLOD.esp") [0x33133209] (DynDOLOD.esm))";
  auto refs = ExtractEspRefsFromLine(line);
  assert(refs.size() == 2);
  assert(refs[0].esp_name == "DynDOLOD.esp");
  assert(refs[0].form_id == "0x9D0F0C07");
  assert(refs[1].esp_name == "DynDOLOD.esm");
  assert(refs[1].form_id == "0x33133209");
}

static void Test_ExtractEspRefs_NoFormId()
{
  std::string_view line = R"(RSI: (Character*) ("SomeMod.esp"))";
  auto refs = ExtractEspRefsFromLine(line);
  assert(refs.size() == 1);
  assert(refs[0].esp_name == "SomeMod.esp");
  assert(refs[0].form_id.empty());
}

static void Test_ParseObjectRefs_FormIdPropagated()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0\n"
    "POSSIBLE RELEVANT OBJECTS:\n"
    "\tRDI: (Character*) \"\xEB\x8F\x84\xEB\xA1\x9C\xEB\xA1\xB1\" [0xFEAD081B] (\"AE_StellarBlade_Doro.esp\")\n"
    "\n"
    "REGISTERS:\n";

  const auto refs = ParseCrashLoggerObjectRefsAscii(log);
  assert(refs.size() == 1);
  assert(refs[0].form_id == "0xFEAD081B");
  assert(refs[0].esp_name == "AE_StellarBlade_Doro.esp");
}

static void Test_AggregateObjectRefs_FormIdKept()
{
  std::vector<CrashLoggerObjectRef> refs;
  {
    CrashLoggerObjectRef r;
    r.location = "RDI"; r.object_type = "Character*"; r.esp_name = "MyMod.esp";
    r.object_name = "NPC1"; r.form_id = "0xDEAD0001"; r.relevance_score = 18;
    refs.push_back(r);
  }
  {
    CrashLoggerObjectRef r;
    r.location = "RSP+68"; r.object_type = "TESObjectREFR*"; r.esp_name = "mymod.esp";
    r.form_id = "0xDEAD0002"; r.relevance_score = 11;
    refs.push_back(r);
  }

  const auto agg = AggregateCrashLoggerObjectRefs(refs);
  assert(agg.size() == 1);
  // best_form_id should be from the highest-scoring ref
  assert(agg[0].form_id == "0xDEAD0001");
}

static void Test_ParseObjectRefs_RegisterFormId()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0\n"
    "PROBABLE CALL STACK:\n"
    "\tSkyrimSE.exe+0x1\n"
    "\n"
    "REGISTERS:\n"
    "RDI: (TESForm*) [0x12345678] (\"SomeForm.esp\")\n"
    "\n"
    "MODULES:\n";

  const auto refs = ParseCrashLoggerObjectRefsAscii(log);
  bool found = false;
  for (const auto& r : refs) {
    if (r.esp_name == "SomeForm.esp") {
      found = true;
      assert(r.form_id == "0x12345678");
    }
  }
  assert(found);
}

static void Test_CrashLoggerFrameSignals_PublicContract()
{
  const auto header = ReadProjectText("dump_tool/src/CrashLoggerParseCore.h");
  AssertContains(
    header,
    "struct CrashLoggerFrameSignals",
    "Crash Logger parser must declare frame-signal result storage.");
  AssertContains(
    header,
    "ParseCrashLoggerFrameSignalsAscii",
    "Crash Logger parser must expose a frame-signal parsing entry point.");
  AssertContains(
    header,
    "direct_fault_module",
    "Crash Logger parser must expose direct DLL fault module extraction.");
  AssertContains(
    header,
    "first_actionable_probable_module",
    "Crash Logger parser must capture the first actionable probable frame.");
  AssertContains(
    header,
    "probable_streak_module",
    "Crash Logger parser must capture same-DLL probable streak winners.");
  AssertContains(
    header,
    "probable_streak_length",
    "Crash Logger parser must track same-DLL probable streak length.");
}

static void Test_ParseCrashLoggerFrameSignals_LegacyProbableRows()
{
  const std::string log =
    "CrashLoggerSSE v1.18.0\n"
    "Unhandled exception \"EXCEPTION_ACCESS_VIOLATION\" at 0x7FF612345678 Precision.dll+0x00ABCDEF\n"
    "CRASH TIME: 2026-03-24 12:34:56\n"
    "PROBABLE CALL STACK:\n"
    "\tSkyrimSE.exe+0x00000001\n"
    "\tkernelbase.dll+0x00000002\n"
    "\tPrecision.dll+0x00001000\n"
    "\tPrecision.dll+0x00002000\n"
    "\tPrecision.dll+0x00003000\n"
    "\td3d11.dll+0x00004000\n"
    "\n"
    "REGISTERS:\n";

  const CrashLoggerFrameSignals signals = ParseCrashLoggerFrameSignalsAscii(log);
  assert(signals.direct_fault_module == "Precision.dll");
  assert(signals.first_actionable_probable_module == "Precision.dll");
  assert(signals.probable_streak_module == "Precision.dll");
  assert(signals.probable_streak_length == 3u);
  assert(signals.probable_modules_in_order.size() == 6u);
  assert(signals.probable_modules_in_order[0] == "SkyrimSE.exe");
  assert(signals.probable_modules_in_order[1] == "kernelbase.dll");
}

static void Test_ParseCrashLoggerFrameSignals_IndexedProbableRows()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0 Feb 10 2026 04:25:35\n"
    "Unhandled exception \"EXCEPTION_ACCESS_VIOLATION\" at 0x7FF612345678 ExampleMod.dll+00001234\n"
    "CRASH TIME: 2026-03-24 12:34:56\n"
    "PROBABLE CALL STACK:\n"
    "\t[ 0] 0x00007FF612340001 SkyrimSE.exe+00000001\n"
    "\t[ 1] 0x00007FF612340002 KERNELBASE.dll+00000002\n"
    "\t[ 2] 0x00007FF612340003 ExampleMod.dll+00000003\tmov eax, eax | ExampleA\n"
    "\t[ 3] 0x00007FF612340004 ExampleMod.dll+00000004\tmov ecx, ecx | ExampleB\n"
    "\t[ 4] 0x00007FF612340005 HookBridge.dll+00000005\n"
    "\n"
    "REGISTERS:\n";

  const CrashLoggerFrameSignals signals = ParseCrashLoggerFrameSignalsAscii(log);
  assert(signals.direct_fault_module == "ExampleMod.dll");
  assert(signals.first_actionable_probable_module == "ExampleMod.dll");
  assert(signals.probable_streak_module == "ExampleMod.dll");
  assert(signals.probable_streak_length == 2u);
  assert(signals.probable_modules_in_order.size() == 5u);
  assert(signals.probable_modules_in_order[0] == "SkyrimSE.exe");
  assert(signals.probable_modules_in_order[1] == "KERNELBASE.dll");
  assert(signals.probable_modules_in_order[4] == "HookBridge.dll");
}

static void Test_ParseCrashLoggerFrameSignals_MixedProbableAndStackScanRows()
{
  const std::string log =
    "CrashLoggerSSE v1-20-1-0 Feb 11 2026 05:26:32\n"
    "Unhandled exception \"EXCEPTION_ACCESS_VIOLATION\" at 0x7FF87A9D6F4F MaxsuDetectionMeter.dll+0056F4F\n"
    "CRASH TIME: 2026-03-31 17:15:55\n"
    "CALL STACK ([P]robable / [S]tack scan):\n"
    "\t[ 0][P] 0x7FF87A9D6F4F MaxsuDetectionMeter.dll+0056F4F\n"
    "\t[ 1][P] 0x7FF87A9D6D49 MaxsuDetectionMeter.dll+0056D49\n"
    "\t[ 2][P] 0x7FF878746EAA OpenAnimationReplacer.dll+0146EAA\n"
    "\t[ 3][P] 0x7FF875B3FE30 po3_PhotoMode.dll+002FE30\n"
    "\t[18][S] 0x7FF874E52A46 PrismaUI.dll+0002A46\n"
    "\t[21][S] 0x7FF88583B5EC SKSEMenuFramework.dll+029B5EC\n"
    "\n"
    "REGISTERS:\n";

  const CrashLoggerFrameSignals signals = ParseCrashLoggerFrameSignalsAscii(log);
  assert(signals.direct_fault_module == "MaxsuDetectionMeter.dll");
  assert(signals.first_actionable_probable_module == "MaxsuDetectionMeter.dll");
  assert(signals.probable_streak_module == "MaxsuDetectionMeter.dll");
  assert(signals.probable_streak_length == 2u);
  assert(signals.probable_modules_in_order.size() == 4u);
  assert(signals.probable_modules_in_order[0] == "MaxsuDetectionMeter.dll");
  assert(signals.probable_modules_in_order[2] == "OpenAnimationReplacer.dll");
  assert(signals.probable_modules_in_order[3] == "po3_PhotoMode.dll");

  const auto topMods = ParseCrashLoggerFrameTopModulesAsciiLower(log);
  assert(topMods.size() == 3u);
  assert(topMods[0] == "maxsudetectionmeter.dll");
  assert(topMods[1] == "openanimationreplacer.dll");
  assert(topMods[2] == "po3_photomode.dll");
}

static void Test_ParseCrashLoggerFrameSignals_IgnoresFaultingApplicationName()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0 Feb 10 2026 04:25:35\n"
    "Faulting application name: SkyrimSE.exe, version 1.6.1170.0\n"
    "CRASH TIME: 2026-03-24 12:34:56\n"
    "PROBABLE CALL STACK:\n"
    "\t[ 0] 0x00007FF612340001 SkyrimSE.exe+00000001\n"
    "\t[ 1] 0x00007FF612340002 Precision.dll+00000003\n"
    "\n"
    "REGISTERS:\n";

  const CrashLoggerFrameSignals signals = ParseCrashLoggerFrameSignalsAscii(log);
  assert(signals.direct_fault_module.empty());
  assert(signals.first_actionable_probable_module == "Precision.dll");
}

static void Test_ParseCrashLoggerFrameSignals_ExtractsFaultingModulePath()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0 Feb 10 2026 04:25:35\n"
    "Faulting module path: C:\\Windows\\System32\\KERNELBASE.dll\n"
    "CRASH TIME: 2026-03-24 12:34:56\n"
    "PROBABLE CALL STACK:\n"
    "\t[ 0] 0x00007FF612340001 Precision.dll+00000001\n"
    "\n"
    "REGISTERS:\n";

  const CrashLoggerFrameSignals signals = ParseCrashLoggerFrameSignalsAscii(log);
  assert(signals.direct_fault_module == "KERNELBASE.dll");
}

static void Test_ParseCrashLoggerFrameSignals_ExtractsFaultModulePath()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0 Feb 10 2026 04:25:35\n"
    "Fault module path: D:\\Mods\\Precision.dll\n"
    "CRASH TIME: 2026-03-24 12:34:56\n"
    "PROBABLE CALL STACK:\n"
    "\t[ 0] 0x00007FF612340001 Precision.dll+00000001\n"
    "\n"
    "REGISTERS:\n";

  const CrashLoggerFrameSignals signals = ParseCrashLoggerFrameSignalsAscii(log);
  assert(signals.direct_fault_module == "Precision.dll");
}

static void Test_ParseCrashLoggerFrameSignals_PathQualifiedModulesNormalizeToFilename()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0 Feb 10 2026 04:25:35\n"
    "Unhandled exception \"EXCEPTION_ACCESS_VIOLATION\" at 0x7FF612345678 D:\\Mods\\FancyMod.dll+00001234\n"
    "CRASH TIME: 2026-03-24 12:34:56\n"
    "PROBABLE CALL STACK:\n"
    "\t[ 0] 0x00007FF612340001 C:\\Windows\\System32\\KERNELBASE.dll+00000001\n"
    "\t[ 1] 0x00007FF612340002 D:\\Mods\\FancyMod.dll+00000002\n"
    "\t[ 2] 0x00007FF612340003 D:\\Mods\\FancyMod.dll+00000003\n"
    "\n"
    "REGISTERS:\n";

  const CrashLoggerFrameSignals signals = ParseCrashLoggerFrameSignalsAscii(log);
  assert(signals.direct_fault_module == "FancyMod.dll");
  assert(signals.first_actionable_probable_module == "FancyMod.dll");
  assert(signals.probable_streak_module == "FancyMod.dll");
  assert(signals.probable_streak_length == 2u);
  assert(signals.probable_modules_in_order.size() == 3u);
  assert(signals.probable_modules_in_order[0] == "KERNELBASE.dll");
  assert(signals.probable_modules_in_order[1] == "FancyMod.dll");
}

static void Test_ParseCrashLoggerFrameTopModules_PathQualifiedSystemRowsFiltered()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0 Feb 10 2026 04:25:35\n"
    "CRASH TIME: 2026-03-24 12:34:56\n"
    "PROBABLE CALL STACK:\n"
    "\t[ 0] 0x00007FF612340001 C:\\Windows\\System32\\KERNELBASE.dll+00000001\n"
    "\t[ 1] 0x00007FF612340002 D:\\Mods\\FancyMod.dll+00000002\n"
    "\t[ 2] 0x00007FF612340003 D:\\Mods\\FancyMod.dll+00000003\n"
    "\t[ 3] 0x00007FF612340004 E:\\Other\\OtherMod.dll+00000004\n"
    "\n"
    "REGISTERS:\n";

  const auto mods = ParseCrashLoggerFrameTopModulesAsciiLower(log);
  assert(mods.size() == 2u);
  assert(mods[0] == "fancymod.dll");
  assert(mods[1] == "othermod.dll");
}

static void Test_ParseTopModules_CrashLog_PathQualifiedRowsUseNormalizedFrameSignals()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0 Feb 10 2026 04:25:35\n"
    "CRASH TIME: 2026-03-24 12:34:56\n"
    "PROBABLE CALL STACK:\n"
    "\t[ 0] 0x00007FF612340001 C:\\Windows\\System32\\KERNELBASE.dll+00000001\n"
    "\t[ 1] 0x00007FF612340002 D:\\Mods\\hdtSMP64.dll+00000002\n"
    "\t[ 2] 0x00007FF612340003 D:\\Mods\\hdtSMP64.dll+00000003\n"
    "\n"
    "REGISTERS:\n";

  const auto frameMods = ParseCrashLoggerFrameTopModulesAsciiLower(log);
  assert(frameMods.size() == 1u);
  assert(frameMods[0] == "hdtsmp64.dll");

  const auto mods = ParseCrashLoggerTopModulesAsciiLower(log);
  assert(mods.size() == 1u);
  assert(mods[0] == "hdtsmp64.dll");
}

static void Test_ParseTopModules_ThreadDump_PathQualifiedRowsNormalizeBeforeFiltering()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0 Feb 10 2026 04:25:35\n"
    "========================================\n"
    "THREAD DUMP (Manual Trigger)\n"
    "========================================\n"
    "TIME: 2026-03-24 12:34:56\n"
    "\tCALLSTACK:\n"
    "\t[ 0] 0x00007FF612340001 C:\\Windows\\System32\\KERNELBASE.dll+00000001\n"
    "\t[ 1] 0x00007FF612340002 D:\\Mods\\FancyThread.dll+00000002\n"
    "\n"
    "===== THREAD 2 (ID: 456) =====\n"
    "\tCALLSTACK:\n"
    "\t[ 0] 0x00007FF612340003 E:\\Mods\\FancyThread.dll+00000003\n"
    "\t[ 1] 0x00007FF612340004 E:\\Mods\\OtherThread.dll+00000004\n";

  const auto mods = ParseCrashLoggerTopModulesAsciiLower(log);
  assert(mods.size() == 2u);
  assert(mods[0] == "fancythread.dll");
  assert(mods[1] == "otherthread.dll");
}

static void Test_ParseTopModules_ThreadDump_AcceptsNonManualHeaderVariant()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0 Feb 10 2026 04:25:35\n"
    "========================================\n"
    "THREAD DUMP (Background Capture)\n"
    "========================================\n"
    "TIME: 2026-03-24 12:34:56\n"
    "\tCALLSTACK:\n"
    "\t[ 0] 0x00007FF612340001 VariantThread.dll+00000001\n"
    "\t[ 1] 0x00007FF612340002 ntdll.dll+00000002\n"
    "\n"
    "===== THREAD 2 (ID: 456) =====\n"
    "\tCALLSTACK:\n"
    "\t[ 0] 0x00007FF612340003 OtherVariant.dll+00000003\n";

  const auto mods = ParseCrashLoggerTopModulesAsciiLower(log);
  assert(mods.size() == 2u);
  // Equal counts keep the order they first appear in, not alphabetical order.
  assert(mods[0] == "variantthread.dll");
  assert(mods[1] == "othervariant.dll");
}

static void Test_ParseTopModules_CrashLog_IgnoresThreadDumpSubstringNoise()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0 Feb 10 2026 04:25:35\n"
    "CRASH TIME: 2026-03-24 12:34:56\n"
    "Exception detail: previous thread dump note retained for diagnostics only.\n"
    "PROBABLE CALL STACK:\n"
    "\t[ 0] 0x00007FF612340001 NoisyCrash.dll+00000001\n"
    "\t[ 1] 0x00007FF612340002 NoisyCrash.dll+00000002\n"
    "\t[ 2] 0x00007FF612340003 OtherCrash.dll+00000003\n"
    "\n"
    "REGISTERS:\n";

  const auto mods = ParseCrashLoggerTopModulesAsciiLower(log);
  assert(mods.size() == 2u);
  assert(mods[0] == "noisycrash.dll");
  assert(mods[1] == "othercrash.dll");
}

static void Test_ParseTopModules_CrashLog_IgnoresThreadDumpBannerLikeNote()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0 Feb 10 2026 04:25:35\n"
    "CRASH TIME: 2026-03-24 12:34:56\n"
    "THREAD DUMP NOTE: previous manual capture attached below for comparison only.\n"
    "PROBABLE CALL STACK:\n"
    "\t[ 0] 0x00007FF612340001 BannerCrash.dll+00000001\n"
    "\t[ 1] 0x00007FF612340002 BannerCrash.dll+00000002\n"
    "\t[ 2] 0x00007FF612340003 OtherCrash.dll+00000003\n"
    "\n"
    "REGISTERS:\n";

  const auto mods = ParseCrashLoggerTopModulesAsciiLower(log);
  assert(mods.size() == 2u);
  assert(mods[0] == "bannercrash.dll");
  assert(mods[1] == "othercrash.dll");
}

// A thread dump's module tally covers every thread, so the analyzer must not
// rank suspects with it (2026-10-18 hang: plugins listed from idle threads).
// Renamed logs are recognized by the header too.
static void Test_IsCrashLoggerThreadDumpLog_HeaderOnly()
{
  using skydiag::dump_tool::crashlogger_core::IsCrashLoggerThreadDumpLogAscii;

  assert(IsCrashLoggerThreadDumpLogAscii(
    "========================================\r\n"
    "THREAD DUMP (Manual Trigger)\r\n"
    "========================================\r\n"
    "TIME: 2097-07-13 12:00:00\r\n"
    "CrashLoggerSSE v1-25-0-0\r\n"
    "Total Threads: 2\r\n"
    "\r\n"
    "===== THREAD 1 (ID: 1234) =====\r\n"
    "\tCALLSTACK:\r\n"
    "\t[ 0] 0x7FF612340001        IdleWorker.dll+0000001\r\n"));
  assert(IsCrashLoggerThreadDumpLogAscii("CrashLoggerSSE v1.20.0\nTHREAD DUMP\n\tCALLSTACK:\n"));

  assert(!IsCrashLoggerThreadDumpLogAscii(""));
  assert(!IsCrashLoggerThreadDumpLogAscii(
    "CrashLoggerSSE v1.20.0 Feb 10 2026 04:25:35\n"
    "CRASH TIME: 2026-03-24 12:34:56\n"
    "THREAD DUMP NOTE: previous manual capture attached below for comparison only.\n"
    "PROBABLE CALL STACK:\n"
    "\t[ 0] 0x00007FF612340001 BannerCrash.dll+00000001\n"));
  assert(!IsCrashLoggerThreadDumpLogAscii(
    "[12:07:32.476] [41376] [I] Thread dump hotkey monitoring started (Ctrl+Shift+F12)\n"));

  const auto captureInputs = ReadProjectText("dump_tool/src/Analyzer.CaptureInputs.cpp");
  AssertContains(
    captureInputs,
    "pairing.selected_kind == \"thread_dump\" || crashlogger_core::IsCrashLoggerThreadDumpLogAscii(*logUtf8)",
    "A paired thread dump must not supply Crash Logger top modules.");
}

static void Test_ParseCrashLoggerFrameSignals_RowDetailWithStackLabelDoesNotTerminate()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0 Feb 10 2026 04:25:35\n"
    "Unhandled exception \"EXCEPTION_ACCESS_VIOLATION\" at 0x7FF612345678 DetailKeep.dll+00001234\n"
    "CRASH TIME: 2026-03-24 12:34:56\n"
    "PROBABLE CALL STACK:\n"
    "\t[ 0] 0x00007FF612340001 DetailKeep.dll+00000001\tcopied from stack: prior frame note\n"
    "\t[ 1] 0x00007FF612340002 DetailKeep.dll+00000002\n"
    "\t[ 2] 0x00007FF612340003 TrailingRow.dll+00000003\n"
    "\n"
    "REGISTERS:\n";

  const CrashLoggerFrameSignals signals = ParseCrashLoggerFrameSignalsAscii(log);
  assert(signals.probable_modules_in_order.size() == 3u);
  assert(signals.probable_modules_in_order[0] == "DetailKeep.dll");
  assert(signals.probable_modules_in_order[2] == "TrailingRow.dll");
  assert(signals.probable_streak_module == "DetailKeep.dll");
  assert(signals.probable_streak_length == 2u);
}

static void Test_ParseCrashLoggerFrameSignals_IgnoresLateDiagnosticFaultModuleLine()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0 Feb 10 2026 04:25:35\n"
    "CRASH TIME: 2026-03-24 12:34:56\n"
    "PROBABLE CALL STACK:\n"
    "\t[ 0] 0x00007FF612340001 EarlyWinner.dll+00000001\n"
    "\t[ 1] 0x00007FF612340002 EarlyWinner.dll+00000002\n"
    "\n"
    "REGISTERS:\n"
    "fault module path: D:\\Diagnostics\\LateNoise.dll\n";

  const CrashLoggerFrameSignals signals = ParseCrashLoggerFrameSignalsAscii(log);
  assert(signals.direct_fault_module.empty());
  assert(signals.first_actionable_probable_module == "EarlyWinner.dll");
  assert(signals.probable_streak_module == "EarlyWinner.dll");
  assert(signals.probable_streak_length == 2u);
}

static void Test_ParseCrashLoggerFrameSignals_Fixture_DirectFaultDll()
{
  const CrashLoggerFrameSignals signals =
    ParseCrashLoggerFrameSignalsAscii(ReadCrashLoggerFrameFixture("direct_fault_dll.log.txt"));
  assert(signals.direct_fault_module == "Precision.dll");
  assert(signals.first_actionable_probable_module == "Precision.dll");
  assert(signals.probable_streak_module == "Precision.dll");
  assert(signals.probable_streak_length == 2u);
}

static void Test_ParseCrashLoggerFrameSignals_Fixture_ExeVictimFirstProbableDll()
{
  const CrashLoggerFrameSignals signals =
    ParseCrashLoggerFrameSignalsAscii(ReadCrashLoggerFrameFixture("exe_victim_first_probable_dll.log.txt"));
  assert(signals.direct_fault_module == "SkyrimSE.exe");
  assert(signals.first_actionable_probable_module == "ExampleMod.dll");
  assert(signals.probable_streak_module == "ExampleMod.dll");
  assert(signals.probable_streak_length == 2u);
}

static void Test_ParseCrashLoggerFrameSignals_Fixture_FrameObjectRefConflict()
{
  const auto log = ReadCrashLoggerFrameFixture("frame_object_ref_conflict.log.txt");
  const CrashLoggerFrameSignals signals = ParseCrashLoggerFrameSignalsAscii(log);
  const auto refs = skydiag::dump_tool::crashlogger_core::ParseCrashLoggerObjectRefsAscii(log);

  assert(signals.direct_fault_module == "SkyrimSE.exe");
  assert(signals.first_actionable_probable_module == "FrameBacked.dll");
  assert(signals.probable_streak_module == "FrameBacked.dll");
  assert(signals.probable_streak_length == 2u);
  assert(!refs.empty());
  assert(refs[0].esp_name == "OtherRef.esp");
}

static void Test_ParseCrashLoggerFrameSignals_Fixture_HookFrameworkVictimFirstProbableDll()
{
  const CrashLoggerFrameSignals signals =
    ParseCrashLoggerFrameSignalsAscii(ReadCrashLoggerFrameFixture("hook_framework_victim_first_probable_dll.log.txt"));
  assert(signals.direct_fault_module == "CrashLoggerSSE.dll");
  assert(signals.first_actionable_probable_module == "RealCause.dll");
  assert(signals.probable_streak_module == "RealCause.dll");
  assert(signals.probable_streak_length == 2u);
}

static void Test_ParseCrashLoggerFrameSignals_Fixture_CppExceptionModuleSupport()
{
  const auto log = ReadCrashLoggerFrameFixture("cpp_exception_module_support.log.txt");
  const CrashLoggerFrameSignals signals = ParseCrashLoggerFrameSignalsAscii(log);
  const auto ex = ParseCrashLoggerCppExceptionDetailsAscii(log);

  assert(signals.direct_fault_module == "SkyrimSE.exe");
  assert(signals.first_actionable_probable_module == "CppOwner.dll");
  assert(signals.probable_streak_module == "CppOwner.dll");
  assert(signals.probable_streak_length == 2u);
  assert(ex);
  assert(ex->type == "std::runtime_error");
  assert(ex->throw_location == "CppOwner.dll+0x1234");
  assert(ex->module == "CppOwner.dll");
}

static void Test_ParseCrashLoggerFrameSignals_Fixture_SystemDllPathqualifiedFirstProbableDll()
{
  const CrashLoggerFrameSignals signals =
    ParseCrashLoggerFrameSignalsAscii(ReadCrashLoggerFrameFixture("system_dll_pathqualified_first_probable_dll.log.txt"));
  assert(signals.direct_fault_module == "KERNELBASE.dll");
  assert(signals.first_actionable_probable_module == "SystemVictim.dll");
  assert(signals.probable_streak_module == "SystemVictim.dll");
  assert(signals.probable_streak_length == 2u);
}

static void Test_ParseCrashLoggerFrameSignals_Fixture_V124RealFormat()
{
  const auto log = ReadCrashLoggerFrameFixture("v124_real_format_direct_fault.log.txt");
  const auto version = ParseCrashLoggerVersionAscii(log);
  const CrashLoggerFrameSignals signals = ParseCrashLoggerFrameSignalsAscii(log);

  assert(LooksLikeCrashLoggerLogTextCore(log));
  assert(version);
  assert(*version == "v1-24-0-0");
  assert(signals.direct_fault_module == "CalamityAffixes.dll");
  assert(signals.first_actionable_probable_module == "CalamityAffixes.dll");
  assert(signals.probable_streak_module == "CalamityAffixes.dll");
  assert(signals.probable_streak_length == 2u);
}

// The original exception Crash Logger reports (here an assert's int3), and
// the relevant objects that have no plugin, from a real 1.25 log.
static void Test_ParseExceptionLineAndObjectsWithoutPlugin()
{
  using skydiag::dump_tool::crashlogger_core::IsLocationObjectTypeAscii;
  using skydiag::dump_tool::crashlogger_core::ParseCrashLoggerExceptionLineAscii;
  using skydiag::dump_tool::crashlogger_core::ParseCrashLoggerUnattributedObjectsAscii;
  const std::string log =
    "CRASH TIME: 2026-10-05 12:28:58\r\n"
    "CrashLoggerSSE v1-25-0-0 Aug 21 2026 09:19:20\r\n"
    "\r\n"
    "Unhandled exception \"EXCEPTION_BREAKPOINT\" at 0x7FFE68312B24 ucrtbase.dll+00C2B24\tint3\r\n"
    "Exception Flags: 0x00000000 (Continuable)\r\n"
    "\r\n"
    "POSSIBLE RELEVANT OBJECTS:\r\n"
    "\tRSP+4C8: (Character*) \"Blood Dragon\" [0xFF0036F8]\r\n"
    "\tRSP+500: (TESObjectCELL*) [0xFF00114E]\r\n"
    "\tRSP+730: (bhkCharProxyController*)\r\n"
    "\tRSP+8E8: (TESPackage*) [0x00050AF2] (\"Skyrim.esm\")\r\n"
    "\tRSP+AD0: (Character*) \"Snow Hare\" [0x001071A8] (\"Skyrim.esm\")\r\n"
    "\r\n"
    "PROCESS INFO:\r\n"
    "\tProcess ID: 10360\r\n";

  const auto exception = ParseCrashLoggerExceptionLineAscii(log);
  assert(exception.has_value());
  assert(exception->type == "EXCEPTION_BREAKPOINT");
  assert(exception->location == "ucrtbase.dll+00C2B24");
  assert(exception->instruction == "int3");
  assert(!ParseCrashLoggerExceptionLineAscii("no exception line here").has_value());

  const auto objects = ParseCrashLoggerUnattributedObjectsAscii(log);
  assert(objects.size() == 1u);  // the cell, the bare pointer and plugin-backed forms are left out
  assert(objects[0].location == "RSP+4C8");
  assert(objects[0].object_name == "Blood Dragon");
  assert(objects[0].form_id == "0xFF0036F8");
  assert(objects[0].esp_name.empty());

  assert(IsLocationObjectTypeAscii("TESObjectCELL*"));
  assert(IsLocationObjectTypeAscii("TESWorldSpace*"));
  assert(!IsLocationObjectTypeAscii("Character*"));
}

// Modules that appear equally often keep call-stack order (the 2026-09-18
// log listed them alphabetically and lost the first two frames' modules).
static void Test_ParseFrameTopModules_TiesKeepStackOrder()
{
  const std::string log =
    "CrashLoggerSSE v1-25-0-0 Aug 21 2026 09:19:20\n"
    "Unhandled exception \"EXCEPTION_ACCESS_VIOLATION\" at 0x7FFA3CB46144 SkyrimUpscaler.dll+0156144\n"
    "CALL STACK ([P]robable / [S]tack scan):\n"
    "\t[ 0][P] 0x7FFA3CB46144             SkyrimUpscaler.dll+0156144\n"
    "\t[ 1][P] 0x7FF9C67C8BF4           SSEDisplayTweaks.dll+0028BF4\n"
    "\t[ 2][P] 0x7FFA371F4D1E                      dmenu.dll+0034D1E\n"
    "\t[ 3][P] 0x7FF9ED4A19FA                      Modex.dll+00919FA\n"
    "\t[ 4][P] 0x7FF9EBA6303A      OpenAnimationReplacer.dll+014303A\n"
    "\n";
  const auto mods = ParseCrashLoggerFrameTopModulesAsciiLower(log);
  assert(mods.size() >= 4u);
  assert(mods[0] == "skyrimupscaler.dll");
  assert(mods[1] == "ssedisplaytweaks.dll");
  assert(mods[2] == "dmenu.dll");
  assert(mods[3] == "modex.dll");
}

// A log paired by time alone (up to 120 s) may describe another crash; its
// exception location then disagrees with the dump's fault.
static void Test_CrashLoggerLocationContradictsDumpFault()
{
  using skydiag::dump_tool::crashlogger_core::CrashLoggerLocationContradictsDumpFault;

  // Same crash: module (any case, any path) and offset agree.
  assert(!CrashLoggerLocationContradictsDumpFault(L"KERNELBASE.dll+00C483A", L"KernelBase.dll", 0xC483Au));
  assert(!CrashLoggerLocationContradictsDumpFault(L"Precision.dll+0x000FDDC7", L"Precision.dll", 0xFDDC7u));
  assert(!CrashLoggerLocationContradictsDumpFault(L"D:\\Mods\\FancyMod.dll+00001234", L"FancyMod.dll", 0x1234u));

  // Another crash: different module, or same module at another offset.
  assert(CrashLoggerLocationContradictsDumpFault(L"hdtSMP64.dll+0012345", L"SkyrimSE.exe", 0x12345u));
  assert(CrashLoggerLocationContradictsDumpFault(L"SkyrimSE.exe+0043756C", L"SkyrimSE.exe", 0x150E416u));

  // Nothing to compare: no fault module, or no module+offset in the log.
  assert(!CrashLoggerLocationContradictsDumpFault(L"SkyrimSE.exe+0043756C", L"", 0u));
  assert(!CrashLoggerLocationContradictsDumpFault(L"", L"SkyrimSE.exe", 0x43756Cu));
  assert(!CrashLoggerLocationContradictsDumpFault(L"0x7FF612345678", L"SkyrimSE.exe", 0x43756Cu));
  assert(!CrashLoggerLocationContradictsDumpFault(L"SkyrimSE.exe+zz", L"SkyrimSE.exe", 0x43756Cu));

  // The dump faulted inside Crash Logger while it reported the original
  // exception, which is what the log names.
  assert(!CrashLoggerLocationContradictsDumpFault(L"SkyrimSE.exe+0043756C", L"CrashLogger.dll", 0x1000u));
  assert(!CrashLoggerLocationContradictsDumpFault(L"SkyrimSE.exe+0043756C", L"CrashLoggerSSE.dll", 0x1000u));
}

// Crash Logger before 1.22 has no POSSIBLE RELEVANT OBJECTS section: the
// object a register holds is described under it, one tab deeper than the
// register line (the shape of public 1.14/1.15 logs; names made up). Register
// lines themselves start with a tab, which the parser used to read as
// sub-lines, so these plugins were never found.
static void Test_ParseObjectRefs_RealRegisterFormatBefore122()
{
  const std::string log =
    "Skyrim SSE v1.6.1170\n"
    "CrashLoggerSSE v1-15-0-0 Oct 12 2024 11:33:37\n"
    "\n"
    "Unhandled exception \"EXCEPTION_ACCESS_VIOLATION\" at 0x7FF600001000 SkyrimSE.exe+0001000\tmov rcx, [rax+0x28]\n"
    "\n"
    "REGISTERS:\n"
    "\tRAX 0x0                (size_t) [0]\n"
    "\tRBP 0x169B2DF9A80      (TESObjectBOOK*)\n"
    "\t\tFile: \"Book Patches.esp\"\n"
    "\t\tModified by: ccExample.esl -> Book Patches.esp\n"
    "\t\tFlags: 0x00000009 kDestructible | kInitialized\n"
    "\t\tName: \"A Made-up Book\"\n"
    "\t\tFormID: 0x8612AB01\n"
    "\t\tFormType: Book (27)\n"
    "\tRSI 0x7FF6BC030DC0     (Inventory3DManager*)\n"
    "\tR9  0x1B712B1EB00      (TESWeather*)\n"
    "\t\tFile: \"WeatherOverhaul.esm\"\n"
    "\t\tFlags: 0x00000009 kDestructible | kInitialized\n"
    "\t\tFormID: 0x01000ABC\n"
    "\t\tFormType: Weather (54)\n"
    "\n"
    "STACK:\n";

  const auto refs = ParseCrashLoggerObjectRefsAscii(log);
  bool book = false, weather = false;
  for (const auto& r : refs) {
    if (r.esp_name == "Book Patches.esp") {
      book = true;
      assert(r.location == "RBP");
      assert(r.object_type == "TESObjectBOOK*");
      assert(r.form_id == "0x8612AB01");
      assert(r.object_name == "A Made-up Book");
    }
    if (r.esp_name == "WeatherOverhaul.esm") {
      weather = true;
      assert(r.location == "R9");
      assert(r.object_type == "TESWeather*");
      assert(r.form_id == "0x01000ABC");
    }
    // "Modified by:" lists every plugin that touched the record; only the
    // defining file is the object's plugin here.
    assert(r.esp_name != "ccExample.esl");
  }
  assert(book && weather);
}

// Files nested deeper than the register object's own fields belong to other
// objects it points to. In Crash Logger 1.25 logs these are the cell under
// "ParentCell:" (where the player stood, e.g. a location overhaul), which
// must not become the character's plugin.
static void Test_ParseObjectRefs_NestedRegisterFilesAreNotTheObject()
{
  const std::string log =
    "CrashLoggerSSE v1-25-0-0 Aug 21 2026 09:19:20\n"
    "\n"
    "REGISTERS:\n"
    "\tRCX 0x2A1B3C4D5E0      (Character*)\n"
    "\t\tFlags: 0x00000009 kDestructible | kInitialized\n"
    "\t\tName: \"Somebody\"\n"
    "\t\tFormID: 0xFE000801\n"
    "\t\tFormType: ActorCharacter (62)\n"
    "\t\tParentCell: ---\n"
    "\t\t\tFile: \"Location Overhaul.esp\"\n"
    "\t\t\tFormID: 0x000AC5D2\n"
    "\tRDX 0x0                (size_t) [0]\n"
    "\n"
    "STACK:\n";

  for (const auto& r : ParseCrashLoggerObjectRefsAscii(log)) {
    assert(r.esp_name != "Location Overhaul.esp");
  }

  // FormID printed before File: still belongs to the object.
  const std::string formIdFirst =
    "REGISTERS:\n"
    "\tRDI 0x2A1B3C4D5E0      (TESObjectSTAT*)\n"
    "\t\tFormID: 0x8612AB02\n"
    "\t\tFile: \"Statics.esp\"\n"
    "\n";
  bool statics = false;
  for (const auto& r : ParseCrashLoggerObjectRefsAscii(formIdFirst)) {
    if (r.esp_name == "Statics.esp") {
      statics = true;
      assert(r.form_id == "0x8612AB02");
    }
  }
  assert(statics);
}

// A synthetic Crash Logger 1.25 thread dump: every record ends with a call
// stack, and the record of the dumping thread itself, "[CURRENT THREAD]",
// comes last.
static std::string ThreadDumpRecord(int index, int tid, bool current = false)
{
  return "===== THREAD " + std::to_string(index) + " (ID: " + std::to_string(tid) + ")" +
         (current ? " [CURRENT THREAD]" : "") + " =====\r\n"
         "REGISTERS:\r\n"
         "\tRAX 0x1                (size_t) [1]\r\n"
         "\r\n"
         "\tCALLSTACK:\r\n"
         "\t[0] 0x7FFCA13A5A24    ntdll.dll+0165A24\r\n"
         "\t[1] 0x7FFCA033DDA7 KERNEL32.DLL+002DDA7\r\n"
         "\r\n";
}

static std::string ThreadDumpHeader(int totalThreads)
{
  return "========================================\r\n"
         "THREAD DUMP (Manual Trigger)\r\n"
         "========================================\r\n"
         "TIME: 2026-10-18 19:04:09\r\n"
         "Skyrim SSE v1.6.1170\r\n"
         "CrashLoggerSSE v1-25-0-0 Aug 21 2026 09:19:20\r\n"
         "Total Threads: " + std::to_string(totalThreads) + "\r\n"
         "\r\n";
}

static void Test_ThreadDumpProgress_TruncatedAndComplete()
{
  using skydiag::dump_tool::crashlogger_core::ParseCrashLoggerThreadDumpProgressAscii;

  // Cut off inside the fourth record, as a 4 KiB write buffer leaves it: three
  // of five threads, no [CURRENT THREAD] record.
  std::string truncated = ThreadDumpHeader(5);
  truncated += ThreadDumpRecord(1, 30100) + ThreadDumpRecord(6, 38060) + ThreadDumpRecord(36, 45324);
  truncated += "===== THREAD 5 (ID: 43564) =====\r\nREGISTERS:\r\n\tRAX 0x0";
  const auto cut = ParseCrashLoggerThreadDumpProgressAscii(truncated);
  assert(cut.is_thread_dump);
  assert(cut.total_threads == 5u);
  assert(cut.thread_records == 4u);
  assert(!cut.has_current_thread);

  std::string complete = ThreadDumpHeader(3);
  complete += ThreadDumpRecord(1, 30100) + ThreadDumpRecord(2, 38060) + ThreadDumpRecord(3, 43792, /*current=*/true);
  const auto full = ParseCrashLoggerThreadDumpProgressAscii(complete);
  assert(full.is_thread_dump);
  assert(full.total_threads == 3u);
  assert(full.thread_records == 3u);
  assert(full.has_current_thread);

  // A crash log is no thread dump; its "Total Threads"-free text counts nothing.
  const auto crash = ParseCrashLoggerThreadDumpProgressAscii(
    "CrashLoggerSSE v1-25-0-0\nUnhandled exception \"EXCEPTION_ACCESS_VIOLATION\" at 0x7FF6 SkyrimSE.exe+0BAB4CF\n"
    "===== THREAD 1 (ID: 1) =====\n");
  assert(!crash.is_thread_dump);
  assert(crash.thread_records == 0u);

  // A malformed total is ignored, not misread.
  const auto junk = ParseCrashLoggerThreadDumpProgressAscii("THREAD DUMP (Manual Trigger)\nTotal Threads: 29x\n");
  assert(junk.is_thread_dump && junk.total_threads == 0u);
}

int main()
{
  Test_ThreadDumpProgress_TruncatedAndComplete();
  Test_CrashLoggerLocationContradictsDumpFault();
  Test_ParseObjectRefs_RealRegisterFormatBefore122();
  Test_ParseObjectRefs_NestedRegisterFilesAreNotTheObject();
  Test_ParseExceptionLineAndObjectsWithoutPlugin();
  Test_ParseFrameTopModules_TiesKeepStackOrder();
  Test_LooksLikeCrashLogger_CrashLog();
  Test_LooksLikeCrashLogger_ThreadDump();
  Test_LooksLikeCrashLogger_RuntimeLogHotkeyLineIsNotThreadDump();
  Test_ClassifyCrashLoggerArtifactName();
  Test_CrashLoggerPairRankingPrioritizesTimeThenProvenance();
  Test_ParseTopModules_CrashLog();
  Test_ParseTopModules_CrashLog_NewCallstackFormat();
  Test_ParseTopModules_ThreadDump();
  Test_ParseTopModules_ThreadDump_NewCallstackFormat();
  Test_ParseTopModules_ThreadDump_TieBreaksAlphabetically();
  Test_StackCorruptionWarning_DoesNotCrash();
  Test_ParseCppExceptionDetails();
  Test_ParseCppExceptionDetails_WithFlexibleSpacing();
  Test_ParseCrashLoggerVersion();
  Test_ParseCrashLoggerVersion_WithBuildTime();
  Test_ParseCrashLoggerVersion_WithHyphensAndBuildTime();
  Test_ParseCrashLoggerVersion_WithFourPartDottedVersion();
  Test_ParseCrashLoggerIni_CrashlogDirectory_Basic();
  Test_ParseCrashLoggerIni_CrashlogDirectory_QuotedAndSpaced();
  Test_ParseCrashLoggerIni_CrashlogDirectory_EmptyIsNone();
  Test_ParseTopModules_ThreadDump_FiltersSystemAndGameExe();

  // Group 1: LooksLikeCrashLogger edge cases
  Test_LooksLikeCrashLogger_ProcessInfo();
  Test_LooksLikeCrashLogger_NotCrashLogger();
  Test_LooksLikeCrashLogger_Empty();

  // Group 2: ParseVersion edge cases
  Test_ParseCrashLoggerVersion_Missing();
  Test_ParseCrashLoggerVersion_Empty();

  // Group 3: C++ Exception edge cases
  Test_ParseCppExceptionDetails_NoBlock();

  // Group 4: TopModules edge cases
  Test_ParseTopModules_EmptyInput();
  Test_ParseTopModules_CrashLog_NoModulesOnlyWarning();
  Test_ParseTopModules_ManyModules_CappedAt8();

  // Group 5: INI parsing edge cases
  Test_ParseCrashLoggerIni_HashComment();
  Test_ParseCrashLoggerIni_LastKeyWinsAndBom();
  Test_ParseCrashLoggerIni_NoDebugSection();

  // Group 6: TryExtractToken direct tests
  Test_TryExtractToken_ValidDll();
  Test_TryExtractToken_ValidExe();
  Test_TryExtractToken_NoModule();
  Test_TryExtractToken_Empty();
  Test_TryExtractToken_NewFormat();

  // Group 7: IsSystemish/IsGameExe direct tests
  Test_IsSystemish_KnownModules();
  Test_IsSystemish_NotSystem();
  Test_IsGameExe_KnownExes();
  Test_IsGameExe_NotGameExe();

  // Group 8: Timestamp parsing
  Test_CompactTimestamp_Standard();
  Test_CompactTimestamp_EmbeddedInLongName();
  Test_CompactTimestamp_NoMatch();
  Test_CompactTimestamp_Empty();
  Test_CompactTimestamp_PartialDigits();
  Test_DashedTimestamp_Standard();
  Test_DashedTimestamp_InvalidMonth();
  Test_DashedTimestamp_NoMatch();
  Test_DashedTimestamp_Empty();
  Test_DashedTimestamp_InvalidHour();
  Test_CompactTimestamp_MultipleMatches_TakesFirst();
  Test_CompactTimestamp_InvalidMonth();
  Test_CompactTimestamp_InvalidHour();

  // Group 9: ESP/ESM object reference parsing
  Test_ParseObjectRefs_BasicExample();
  Test_ParseObjectRefs_NoEsp_SkipsLine();
  Test_ParseObjectRefs_UnquotedEsp();
  Test_ParseObjectRefs_FiltersVanillaEsp();
  Test_ParseObjectRefs_ModifiedBySkipped();
  Test_ParseObjectRefs_RegisterFileField();
  Test_ParseObjectRefs_QuotedPluginNamesWithSpaces();
  Test_ParseObjectRefs_RegisterRanksHigher();
  Test_ParseObjectRefs_ActorRanksHigher();
  Test_AggregateObjectRefs_Dedup();
  Test_ParseObjectRefs_EmptyInput();
  Test_ParseObjectRefs_UnicodeObjectName();
  Test_ParseObjectRefs_MixedQuotedUnquoted();
  Test_IsVanillaDlcEsp_AllKnown();
  Test_ParseObjectRefs_MalformedQuotedParen_NoInfiniteLoop();
  Test_ExtractEspNames_NoParens();
  Test_ParseObjectRefs_CCContentFiltered();
  Test_ParseObjectRefs_CrashLoggerV121V122SimplifiedIntrospection();

  // Group 10: FormID extraction tests
  Test_ExtractFormIdBefore_Basic();
  Test_ExtractFormIdBefore_NoFormId();
  Test_ExtractFormIdBefore_WithSpaces();
  Test_ExtractFormIdBefore_AtStart();
  Test_ExtractEspRefs_WithFormId();
  Test_ExtractEspRefs_MultipleEsps();
  Test_ExtractEspRefs_NoFormId();
  Test_ParseObjectRefs_FormIdPropagated();
  Test_AggregateObjectRefs_FormIdKept();
  Test_ParseObjectRefs_RegisterFormId();
  Test_CrashLoggerFrameSignals_PublicContract();
  Test_ParseCrashLoggerFrameSignals_LegacyProbableRows();
  Test_ParseCrashLoggerFrameSignals_IndexedProbableRows();
  Test_ParseCrashLoggerFrameSignals_MixedProbableAndStackScanRows();
  Test_ParseCrashLoggerFrameSignals_IgnoresFaultingApplicationName();
  Test_ParseCrashLoggerFrameSignals_ExtractsFaultingModulePath();
  Test_ParseCrashLoggerFrameSignals_ExtractsFaultModulePath();
  Test_ParseCrashLoggerFrameSignals_PathQualifiedModulesNormalizeToFilename();
  Test_ParseCrashLoggerFrameTopModules_PathQualifiedSystemRowsFiltered();
  Test_ParseTopModules_CrashLog_PathQualifiedRowsUseNormalizedFrameSignals();
  Test_ParseTopModules_ThreadDump_PathQualifiedRowsNormalizeBeforeFiltering();
  Test_ParseTopModules_ThreadDump_AcceptsNonManualHeaderVariant();
  Test_ParseTopModules_CrashLog_IgnoresThreadDumpSubstringNoise();
  Test_ParseTopModules_CrashLog_IgnoresThreadDumpBannerLikeNote();
  Test_IsCrashLoggerThreadDumpLog_HeaderOnly();
  Test_ParseCrashLoggerFrameSignals_RowDetailWithStackLabelDoesNotTerminate();
  Test_ParseCrashLoggerFrameSignals_IgnoresLateDiagnosticFaultModuleLine();
  Test_ParseCrashLoggerFrameSignals_Fixture_DirectFaultDll();
  Test_ParseCrashLoggerFrameSignals_Fixture_ExeVictimFirstProbableDll();
  Test_ParseCrashLoggerFrameSignals_Fixture_FrameObjectRefConflict();
  Test_ParseCrashLoggerFrameSignals_Fixture_HookFrameworkVictimFirstProbableDll();
  Test_ParseCrashLoggerFrameSignals_Fixture_CppExceptionModuleSupport();
  Test_ParseCrashLoggerFrameSignals_Fixture_SystemDllPathqualifiedFirstProbableDll();
  Test_ParseCrashLoggerFrameSignals_Fixture_V124RealFormat();

  return 0;
}
