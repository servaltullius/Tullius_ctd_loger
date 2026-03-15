#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

static std::string ReadAllText(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::in | std::ios::binary);
  assert(in && "Failed to open file");
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static void RequireContains(const std::string& haystack, const char* needle, const char* message)
{
  if (haystack.find(needle) == std::string::npos) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

int main()
{
  const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  const auto csproj = ReadAllText(repoRoot / "dump_tool_winui" / "SkyrimDiagDumpToolWinUI.csproj");
  RequireContains(
    csproj,
    "DISABLE_XAML_GENERATED_MAIN",
    "WinUI project must disable the generated XAML Main so headless mode can short-circuit startup.");

  const auto program = ReadAllText(repoRoot / "dump_tool_winui" / "Program.cs");
  RequireContains(program, "static int Main(string[] args)", "Program.cs must define a custom WinUI entry point.");
  RequireContains(program, "DumpToolInvocationOptions.Parse(args)", "Custom WinUI entry point must parse headless arguments before bootstrapping XAML.");
  RequireContains(program, "if (options.Headless)", "Custom WinUI entry point must short-circuit headless launches.");
  RequireContains(program, "HeadlessEntryPoint.Run(options)", "Headless entry point must delegate to the shared headless runner.");
  RequireContains(program, "global::Microsoft.UI.Xaml.Application.Start", "Custom entry point must still bootstrap WinUI for interactive launches.");

  const auto headless = ReadAllText(repoRoot / "dump_tool_winui" / "HeadlessEntryPoint.cs");
  RequireContains(headless, "NativeAnalyzerBridge.RunAnalyzeAsync", "Shared headless runner must delegate to NativeAnalyzerBridge.");
  RequireContains(headless, "GetAwaiter().GetResult()", "Shared headless runner must synchronously return the native analysis exit code.");
  RequireContains(headless, "return exitCode;", "Shared headless runner must return the native analysis exit code.");

  const auto bootstrapLog = ReadAllText(repoRoot / "dump_tool_winui" / "HeadlessBootstrapLog.cs");
  RequireContains(bootstrapLog, "SkyrimDiagDumpToolWinUI_headless_bootstrap.log", "Headless bootstrap logging must write to a deterministic file for CI diagnostics.");
  RequireContains(bootstrapLog, "File.AppendAllText", "Headless bootstrap logging must append stage markers to disk.");

  return 0;
}
