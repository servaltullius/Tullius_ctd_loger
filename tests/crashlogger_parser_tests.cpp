#include "CrashLoggerParseCore.h"

#include <cassert>
#include <string>
#include <vector>

using skydiag::dump_tool::LooksLikeCrashLoggerLogTextCore;
using skydiag::dump_tool::crashlogger_core::ParseCrashLoggerCppExceptionDetailsAscii;
using skydiag::dump_tool::crashlogger_core::ParseCrashLoggerIniCrashlogDirectoryAscii;
using skydiag::dump_tool::crashlogger_core::ParseCrashLoggerVersionAscii;
using skydiag::dump_tool::ParseCrashLoggerTopModulesAsciiLower;

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
  return 0;
}
