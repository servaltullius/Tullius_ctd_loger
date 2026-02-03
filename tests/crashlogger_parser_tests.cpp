#include "CrashLoggerParseCore.h"

#include <cassert>
#include <string>
#include <vector>

using skydiag::dump_tool::LooksLikeCrashLoggerLogTextCore;
using skydiag::dump_tool::crashlogger_core::ParseCrashLoggerCppExceptionDetailsAscii;
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

int main()
{
  Test_LooksLikeCrashLogger_CrashLog();
  Test_LooksLikeCrashLogger_ThreadDump();
  Test_ParseTopModules_CrashLog();
  Test_ParseTopModules_ThreadDump();
  Test_StackCorruptionWarning_DoesNotCrash();
  Test_ParseCppExceptionDetails();
  return 0;
}
