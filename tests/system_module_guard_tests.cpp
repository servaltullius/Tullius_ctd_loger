#include <string>

#include "SourceGuardTestUtils.h"

using skydiag::tests::source_guard::AssertContains;
using skydiag::tests::source_guard::ReadProjectText;

int main()
{
  const std::string minidumpUtil = ReadProjectText("dump_tool/src/MinidumpUtil.cpp");
  const std::string analyzer = ReadProjectText("dump_tool/src/Analyzer.cpp");
  const std::string summary = ReadProjectText("dump_tool/src/EvidenceBuilderSummary.cpp");
  const std::string rec = ReadProjectText("dump_tool/src/EvidenceBuilderRecommendations.cpp");
  const std::string crashLogger = ReadProjectText("dump_tool/src/CrashLogger.cpp");
  const std::string crashLoggerParseCore = ReadProjectText("dump_tool/src/CrashLoggerParseCore.cpp");
  const std::string crashLoggerParseCoreHeader = ReadProjectText("dump_tool/src/CrashLoggerParseCore.h");

  AssertContains(
    minidumpUtil,
    "win32u.dll",
    "System module list must include win32u.dll.");
  AssertContains(
    minidumpUtil,
    "IsLikelyWindowsSystemModulePathLower",
    "Minidump util must keep Windows system-path detection.");
  AssertContains(
    minidumpUtil,
    "IsLikelyWindowsSystemModulePath(",
    "Minidump util must expose Windows system-path helper.");

  AssertContains(
    analyzer,
    "IsLikelyWindowsSystemModulePath(out.fault_module_path)",
    "Analyzer must use module path when classifying Windows system modules.");
  AssertContains(
    analyzer,
    "faultIsSystem",
    "Analyzer must gate inferred mod names for system modules.");
  AssertContains(
    analyzer,
    "out.inferred_mod_name.clear()",
    "Analyzer must clear bogus inferred mod names in guarded cases.");

  AssertContains(
    summary,
    "topSuspectIsSystem",
    "Summary builder must detect top system-DLL suspects.");
  AssertContains(
    summary,
    "waiting/victim location",
    "Summary builder must avoid over-blaming system DLLs for hang captures.");

  AssertContains(
    rec,
    "topStackCandidateIsSystem",
    "Recommendations must detect top system-DLL stack candidates.");
  AssertContains(
    rec,
    "Windows system DLL",
    "Recommendations must include system-DLL caution guidance.");

  AssertContains(
    crashLogger,
    "IsSystemishModule",
    "CrashLogger module filter must delegate to IsSystemishModule for system module detection.");
  AssertContains(
    crashLogger,
    "BuildNormalizedCrashLoggerFrameSignals",
    "CrashLogger normalization must compute analyzer-facing frame signals separately from raw parsing.");
  AssertContains(
    crashLogger,
    "NormalizeCrashLoggerModuleFilename",
    "CrashLogger wrapper normalization must strip directory prefixes before canonical lookup and filtering.");
  AssertContains(
    crashLogger,
    "first_actionable_probable_module",
    "CrashLogger normalization must preserve a first actionable probable module field.");
  AssertContains(
    crashLogger,
    "probable_streak_length",
    "CrashLogger normalization must preserve same-DLL streak length for analyzer use.");
  AssertContains(
    crashLoggerParseCore,
    "win32u.dll",
    "CrashLogger parse core filter must include win32u.dll as system module.");
  AssertContains(
    crashLoggerParseCore,
    "probable_modules_in_order",
    "CrashLogger raw frame parsing must preserve probable rows before analyzer filtering.");

  AssertContains(
    crashLoggerParseCoreHeader,
    "IsSystemishModuleAsciiLower",
    "CrashLogger parse core header must keep system-module helper available while frame signals are added.");
  AssertContains(
    crashLoggerParseCoreHeader,
    "IsGameExeModuleAsciiLower",
    "CrashLogger parse core header must keep game-exe helper available while frame signals are added.");

  return 0;
}
