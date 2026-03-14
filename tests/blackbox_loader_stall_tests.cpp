#include "SourceGuardTestUtils.h"

#include <filesystem>

using skydiag::tests::source_guard::AssertContains;
using skydiag::tests::source_guard::ReadAllText;

namespace {

std::string ReadJoined(const std::filesystem::path& path)
{
  std::string text = ReadAllText(path);
  if (path.filename() == "OutputWriter.cpp") {
    text += "\n" + ReadAllText(path.parent_path() / "OutputWriter.Summary.cpp");
    text += "\n" + ReadAllText(path.parent_path() / "OutputWriter.Report.cpp");
  }
  return text;
}

void TestBlackboxLoaderStallSourceContracts()
{
  const std::filesystem::path repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  const auto sharedHeader = repoRoot / "shared" / "SkyrimDiagShared.h";
  const auto pluginBlackbox = repoRoot / "plugin" / "src" / "Blackbox.cpp";
  const auto crashHandler = repoRoot / "plugin" / "src" / "CrashHandler.cpp";
  const auto analyzerHeader = repoRoot / "dump_tool" / "src" / "Analyzer.h";
  const auto analyzerInternals = repoRoot / "dump_tool" / "src" / "AnalyzerInternals.cpp";
  const auto analyzerFirstChance = repoRoot / "dump_tool" / "src" / "AnalyzerInternalsFirstChance.cpp";
  const auto outputWriter = repoRoot / "dump_tool" / "src" / "OutputWriter.cpp";

  assert(std::filesystem::exists(sharedHeader) && "shared/SkyrimDiagShared.h not found");
  assert(std::filesystem::exists(pluginBlackbox) && "plugin/src/Blackbox.cpp not found");
  assert(std::filesystem::exists(crashHandler) && "plugin/src/CrashHandler.cpp not found");
  assert(std::filesystem::exists(analyzerHeader) && "dump_tool/src/Analyzer.h not found");
  assert(std::filesystem::exists(analyzerInternals) && "dump_tool/src/AnalyzerInternals.cpp not found");
  assert(std::filesystem::exists(analyzerFirstChance) && "dump_tool/src/AnalyzerInternalsFirstChance.cpp not found");
  assert(std::filesystem::exists(outputWriter) && "dump_tool/src/OutputWriter.cpp not found");

  const auto sharedHeaderText = ReadAllText(sharedHeader);
  const auto pluginBlackboxText = ReadAllText(pluginBlackbox);
  const auto crashHandlerText = ReadAllText(crashHandler);
  const auto analyzerHeaderText = ReadAllText(analyzerHeader);
  const auto analyzerInternalsText = ReadAllText(analyzerInternals);
  const auto analyzerFirstChanceText = ReadAllText(analyzerFirstChance);
  const auto outputWriterText = ReadJoined(outputWriter);

  AssertContains(sharedHeaderText, "kModuleLoad", "EventType must include module load events.");
  AssertContains(sharedHeaderText, "kModuleUnload", "EventType must include module unload events.");
  AssertContains(sharedHeaderText, "kThreadCreate", "EventType must include thread create events.");
  AssertContains(sharedHeaderText, "kThreadExit", "EventType must include thread exit events.");
  AssertContains(sharedHeaderText, "kFirstChanceException", "EventType must include first-chance exception telemetry.");

  AssertContains(pluginBlackboxText, "kModuleLoad", "Plugin blackbox must be able to emit module load events.");
  AssertContains(pluginBlackboxText, "kModuleUnload", "Plugin blackbox must be able to emit module unload events.");
  AssertContains(pluginBlackboxText, "kThreadCreate", "Plugin blackbox must be able to emit thread create events.");
  AssertContains(pluginBlackboxText, "kThreadExit", "Plugin blackbox must be able to emit thread exit events.");
  AssertContains(pluginBlackboxText, "PushFirstChanceExceptionEvent", "Plugin blackbox must provide a first-chance telemetry emitter.");
  AssertContains(crashHandlerText, "IsBenignFirstChanceException", "Crash handler must filter benign first-chance exceptions.");
  AssertContains(crashHandlerText, "ConsumeFirstChanceTelemetryBudget", "Crash handler must rate-limit and dedupe first-chance telemetry.");

  AssertContains(analyzerHeaderText, "BlackboxFreezeSummary", "Analyzer must define a blackbox freeze aggregate model.");
  AssertContains(analyzerHeaderText, "FirstChanceSummary", "Analyzer must define a first-chance aggregate model.");
  AssertContains(analyzerHeaderText, "repeated_signature_count", "FirstChanceSummary must track repeated suspicious signatures.");
  AssertContains(analyzerInternalsText, "ModuleLoad", "Analyzer event naming must expose module load events.");
  AssertContains(analyzerInternalsText, "ModuleUnload", "Analyzer event naming must expose module unload events.");
  AssertContains(analyzerInternalsText, "ThreadCreate", "Analyzer event naming must expose thread create events.");
  AssertContains(analyzerInternalsText, "ThreadExit", "Analyzer event naming must expose thread exit events.");
  AssertContains(analyzerInternalsText, "FirstChanceException", "Analyzer event naming must expose first-chance exception events.");
  AssertContains(analyzerFirstChanceText, "kFirstChanceException", "Analyzer first-chance aggregate must scan first-chance events.");
  AssertContains(analyzerFirstChanceText, "loading_window_count", "Analyzer first-chance aggregate must count loading-window activity.");

  AssertContains(outputWriterText, "blackbox", "Output must surface blackbox-derived freeze context when present.");
  AssertContains(outputWriterText, "module churn", "Output must mention module churn-derived freeze reasons.");
  AssertContains(outputWriterText, "first_chance_context", "Output must surface aggregated first-chance context.");
}

}  // namespace

int main()
{
  TestBlackboxLoaderStallSourceContracts();
  return 0;
}
