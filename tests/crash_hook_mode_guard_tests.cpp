#include <cassert>
#include <filesystem>
#include <fstream>
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

static void AssertContains(const std::string& haystack, const char* needle, const char* message)
{
  assert(haystack.find(needle) != std::string::npos && message);
}

int main()
{
  const std::filesystem::path repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  const std::filesystem::path pluginIniPath = repoRoot / "dist" / "SkyrimDiag.ini";
  assert(std::filesystem::exists(pluginIniPath) && "dist/SkyrimDiag.ini not found");
  const std::string pluginIni = ReadAllText(pluginIniPath);
  AssertContains(pluginIni, "EnableUnsafeCrashHookMode2=0", "Missing explicit unsafe mode guard key in plugin ini");
  AssertContains(
    pluginIni,
    "EnableAdaptiveResourceLogThrottle",
    "Missing adaptive resource log throttle toggle in plugin ini");
  AssertContains(
    pluginIni,
    "ResourceLogThrottleHighWatermarkPerSec",
    "Missing resource log throttle high-watermark key in plugin ini");
  AssertContains(
    pluginIni,
    "ResourceLogThrottleMaxSampleDivisor",
    "Missing resource log throttle divisor key in plugin ini");

  const std::filesystem::path pluginMainPath = repoRoot / "plugin" / "src" / "PluginMain.cpp";
  const std::filesystem::path crashHandlerPath = repoRoot / "plugin" / "src" / "CrashHandler.cpp";
  assert(std::filesystem::exists(pluginMainPath) && "plugin/src/PluginMain.cpp not found");
  assert(std::filesystem::exists(crashHandlerPath) && "plugin/src/CrashHandler.cpp not found");
  const std::string pluginMain = ReadAllText(pluginMainPath);
  const std::string crashHandler = ReadAllText(crashHandlerPath);
  AssertContains(pluginMain, "EnableUnsafeCrashHookMode2", "Plugin config loader must read unsafe mode guard key");
  AssertContains(pluginMain, "mode == 2", "Plugin config must check mode==2 before allowing all-exception hook mode");
  AssertContains(
    crashHandler,
    "IsFatalExceptionCode(",
    "Crash handler mode 1 must route through fatal-only classification.");
  AssertContains(
    crashHandler,
    "return IsFatalExceptionCode(code);",
    "Crash handler mode 1 must not record every non-ignorable first-chance exception.");
  AssertContains(
    pluginMain,
    "EnableAdaptiveResourceLogThrottle",
    "Plugin config loader must read adaptive resource log throttle toggle");
  AssertContains(
    pluginMain,
    "ConfigureResourceLogThrottle(",
    "Plugin must apply resource log throttle config before installing resource hooks");
  return 0;
}
