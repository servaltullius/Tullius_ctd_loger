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

  const std::filesystem::path iniPath = repoRoot / "dist" / "SkyrimDiagHelper.ini";
  assert(std::filesystem::exists(iniPath) && "SkyrimDiagHelper.ini not found");
  const std::string ini = ReadAllText(iniPath);

  // Option: avoid popping the viewer for first-chance exceptions when the game is still running.
  AssertContains(ini, "AutoOpenCrashOnlyIfProcessExited", "Missing crash viewer gating option in ini");
  AssertContains(ini, "AutoOpenCrashWaitForExitMs", "Missing crash exit wait timeout option in ini");

  const std::filesystem::path configCppPath = repoRoot / "helper" / "src" / "Config.cpp";
  assert(std::filesystem::exists(configCppPath) && "Config.cpp not found");
  const std::string configCpp = ReadAllText(configCppPath);

  // Ensure the helper actually reads the INI keys.
  AssertContains(configCpp, "AutoOpenCrashOnlyIfProcessExited", "Helper config loader does not read crash gating option");
  AssertContains(configCpp, "AutoOpenCrashWaitForExitMs", "Helper config loader does not read crash wait timeout option");
  return 0;
}

