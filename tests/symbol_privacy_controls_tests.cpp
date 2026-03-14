#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

static std::string ReadAllText(const std::filesystem::path& path)
{
  std::ostringstream ss;
  const auto append = [&](const std::filesystem::path& inputPath) {
    std::ifstream in(inputPath, std::ios::in | std::ios::binary);
    assert(in && "Failed to open file");
    ss << in.rdbuf();
  };
  append(path);
  if (path.filename() == "Analyzer.cpp") {
    append(path.parent_path() / "Analyzer.CaptureInputs.cpp");
    append(path.parent_path() / "Analyzer.History.cpp");
  }
  if (path.filename() == "OutputWriter.cpp") {
    append(path.parent_path() / "OutputWriter.Summary.cpp");
    append(path.parent_path() / "OutputWriter.Report.cpp");
  }
  return ss.str();
}

static void AssertContains(const std::string& haystack, const char* needle, const char* message)
{
  assert(haystack.find(needle) != std::string::npos && message);
}

int main()
{
  const std::filesystem::path repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  const std::filesystem::path helperIniPath = repoRoot / "dist" / "SkyrimDiagHelper.ini";
  assert(std::filesystem::exists(helperIniPath) && "SkyrimDiagHelper.ini not found");
  const std::string helperIni = ReadAllText(helperIniPath);
  AssertContains(helperIni, "AllowOnlineSymbols=0", "Missing explicit default for online symbol access");

  const std::filesystem::path helperConfigPath = repoRoot / "helper" / "src" / "Config.cpp";
  assert(std::filesystem::exists(helperConfigPath) && "helper/src/Config.cpp not found");
  const std::string helperConfig = ReadAllText(helperConfigPath);
  AssertContains(helperConfig, "AllowOnlineSymbols", "Helper config loader must read AllowOnlineSymbols");

  const std::filesystem::path winUiOptionsPath = repoRoot / "dump_tool_winui" / "DumpToolInvocationOptions.cs";
  assert(std::filesystem::exists(winUiOptionsPath) && "dump_tool_winui/DumpToolInvocationOptions.cs not found");
  const std::string winUiOptions = ReadAllText(winUiOptionsPath);
  AssertContains(winUiOptions, "--allow-online-symbols", "WinUI option parser must accept --allow-online-symbols");
  AssertContains(winUiOptions, "--no-online-symbols", "WinUI option parser must accept --no-online-symbols");

  const std::filesystem::path analyzerPath = repoRoot / "dump_tool" / "src" / "Analyzer.cpp";
  assert(std::filesystem::exists(analyzerPath) && "dump_tool/src/Analyzer.cpp not found");
  const std::string analyzer = ReadAllText(analyzerPath);
  AssertContains(analyzer, "allow_online_symbols", "Analyzer must expose online symbol policy handling");
  AssertContains(analyzer, "[Symbols]", "Analyzer diagnostics must distinguish symbol runtime degradation.");

  const std::filesystem::path stackwalkSymbolsPath = repoRoot / "dump_tool" / "src" / "AnalyzerInternalsStackwalkSymbols.cpp";
  assert(std::filesystem::exists(stackwalkSymbolsPath) && "dump_tool/src/AnalyzerInternalsStackwalkSymbols.cpp not found");
  const std::string stackwalkSymbols = ReadAllText(stackwalkSymbolsPath);
  AssertContains(stackwalkSymbols, "dbghelp.dll", "Stackwalk symbol session must inspect dbghelp runtime.");
  AssertContains(stackwalkSymbols, "msdia140.dll", "Stackwalk symbol session must inspect msdia140 runtime.");
  AssertContains(stackwalkSymbols, "runtimeDegraded", "Stackwalk symbol session must track degraded symbol runtime state.");
  AssertContains(stackwalkSymbols, "runtimeDiagnostics", "Stackwalk symbol session must surface symbol runtime diagnostics.");

  const std::filesystem::path outputWriterPath = repoRoot / "dump_tool" / "src" / "OutputWriter.cpp";
  assert(std::filesystem::exists(outputWriterPath) && "dump_tool/src/OutputWriter.cpp not found");
  const std::string outputWriter = ReadAllText(outputWriterPath);
  AssertContains(outputWriter, "path_redaction_applied", "Summary output must declare whether path redaction was applied");
  AssertContains(outputWriter, "online_symbol_source_allowed", "Summary output must declare whether online symbols were allowed");
  AssertContains(outputWriter, "MaybeRedactPath(rr.path", "Resource paths must respect path redaction in summary/report outputs");
  AssertContains(outputWriter, "dbghelp_path", "Summary output must report dbghelp runtime path.");
  AssertContains(outputWriter, "msdia_available", "Summary output must report DIA runtime availability.");
  return 0;
}
