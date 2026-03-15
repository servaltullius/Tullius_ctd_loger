#include <cassert>
#include <string>

#include "SourceGuardTestUtils.h"

using skydiag::tests::source_guard::AssertContains;
using skydiag::tests::source_guard::ProjectRoot;
using skydiag::tests::source_guard::ReadProjectText;

int main()
{
  const auto helperIniPath = ProjectRoot() / "dist" / "SkyrimDiagHelper.ini";
  assert(std::filesystem::exists(helperIniPath) && "SkyrimDiagHelper.ini not found");
  const std::string helperIni = skydiag::tests::source_guard::ReadAllText(helperIniPath);
  AssertContains(helperIni, "AllowOnlineSymbols=0", "Missing explicit default for online symbol access");

  const std::string helperConfig = ReadProjectText("helper/src/Config.cpp");
  AssertContains(helperConfig, "AllowOnlineSymbols", "Helper config loader must read AllowOnlineSymbols");

  const std::string winUiOptions = ReadProjectText("dump_tool_winui/DumpToolInvocationOptions.cs");
  AssertContains(winUiOptions, "--allow-online-symbols", "WinUI option parser must accept --allow-online-symbols");
  AssertContains(winUiOptions, "--no-online-symbols", "WinUI option parser must accept --no-online-symbols");

  const std::string analyzer = ReadProjectText("dump_tool/src/Analyzer.cpp");
  AssertContains(analyzer, "allow_online_symbols", "Analyzer must expose online symbol policy handling");
  AssertContains(analyzer, "[Symbols]", "Analyzer diagnostics must distinguish symbol runtime degradation.");

  const std::string stackwalkSymbols = ReadProjectText("dump_tool/src/AnalyzerInternalsStackwalkSymbols.cpp");
  AssertContains(stackwalkSymbols, "dbghelp.dll", "Stackwalk symbol session must inspect dbghelp runtime.");
  AssertContains(stackwalkSymbols, "msdia140.dll", "Stackwalk symbol session must inspect msdia140 runtime.");
  AssertContains(stackwalkSymbols, "runtimeDegraded", "Stackwalk symbol session must track degraded symbol runtime state.");
  AssertContains(stackwalkSymbols, "runtimeDiagnostics", "Stackwalk symbol session must surface symbol runtime diagnostics.");

  const std::string outputWriter = ReadProjectText("dump_tool/src/OutputWriter.cpp");
  AssertContains(outputWriter, "path_redaction_applied", "Summary output must declare whether path redaction was applied");
  AssertContains(outputWriter, "online_symbol_source_allowed", "Summary output must declare whether online symbols were allowed");
  AssertContains(outputWriter, "MaybeRedactPath(rr.path", "Resource paths must respect path redaction in summary/report outputs");
  AssertContains(outputWriter, "dbghelp_path", "Summary output must report dbghelp runtime path.");
  AssertContains(outputWriter, "msdia_available", "Summary output must report DIA runtime availability.");
  return 0;
}
