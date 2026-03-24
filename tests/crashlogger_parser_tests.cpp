#include "CrashLoggerParseCore.h"
#include "SourceGuardTestUtils.h"

#include <cassert>
#include <string>
#include <vector>

using skydiag::dump_tool::LooksLikeCrashLoggerLogTextCore;
using skydiag::dump_tool::crashlogger_core::ParseCrashLoggerCppExceptionDetailsAscii;
using skydiag::dump_tool::crashlogger_core::ParseCrashLoggerIniCrashlogDirectoryAscii;
using skydiag::dump_tool::crashlogger_core::ParseCrashLoggerVersionAscii;
using skydiag::dump_tool::ParseCrashLoggerTopModulesAsciiLower;
using skydiag::dump_tool::crashlogger_core::TryExtractModulePlusOffsetTokenAscii;
using skydiag::dump_tool::crashlogger_core::IsSystemishModuleAsciiLower;
using skydiag::dump_tool::crashlogger_core::IsGameExeModuleAsciiLower;
using skydiag::dump_tool::crashlogger_core::TryExtractCompactTimestampFromStem;
using skydiag::dump_tool::crashlogger_core::TryExtractDashedTimestampFromStem;
using skydiag::dump_tool::crashlogger_core::ParsedTimestamp;
using skydiag::tests::source_guard::AssertContains;
using skydiag::tests::source_guard::ReadProjectText;

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

static void Test_ParseCrashLoggerIni_HashComment()
{
  const std::string s =
    "# comment line\n"
    "[Debug]\n"
    "Crashlog Directory=C:\\\\Logs # inline comment\n";
  const auto dir = ParseCrashLoggerIniCrashlogDirectoryAscii(s);
  assert(dir);
  assert(*dir == "C:\\\\Logs");
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

int main()
{
  Test_LooksLikeCrashLogger_CrashLog();
  Test_LooksLikeCrashLogger_ThreadDump();
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

  return 0;
}
