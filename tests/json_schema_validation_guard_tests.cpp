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

static void TestTroubleshootingGuidesJsonHasVersionField()
{
  const std::filesystem::path repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
  auto src = ReadAllText(repoRoot / "dump_tool" / "data" / "troubleshooting_guides.json");
  AssertContains(src, "\"version\"", "troubleshooting_guides.json must have version field");
  AssertContains(src, "\"guides\"", "troubleshooting_guides.json must have guides array");
}

int main()
{
  const std::filesystem::path repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  // All JSON data files must have a "version" field
  const auto hookFw = ReadAllText(repoRoot / "dump_tool" / "data" / "hook_frameworks.json");
  const auto sigs = ReadAllText(repoRoot / "dump_tool" / "data" / "crash_signatures.json");
  const auto rules = ReadAllText(repoRoot / "dump_tool" / "data" / "plugin_rules.json");
  const auto gfx = ReadAllText(repoRoot / "dump_tool" / "data" / "graphics_injection_rules.json");

  AssertContains(hookFw, "\"version\"", "hook_frameworks.json must have a version field.");
  AssertContains(sigs, "\"version\"", "crash_signatures.json must have a version field.");
  AssertContains(rules, "\"version\"", "plugin_rules.json must have a version field.");
  AssertContains(gfx, "\"version\"", "graphics_injection_rules.json must have a version field.");

  // Loaders must check version field
  const auto minidumpUtil = ReadAllText(repoRoot / "dump_tool" / "src" / "MinidumpUtil.cpp");
  const auto pluginRules = ReadAllText(repoRoot / "dump_tool" / "src" / "PluginRules.cpp");
  const auto sigDb = ReadAllText(repoRoot / "dump_tool" / "src" / "SignatureDatabase.cpp");

  AssertContains(minidumpUtil, "\"version\"", "LoadHookFrameworksFromJson must validate version field.");
  AssertContains(pluginRules, "\"version\"", "PluginRules::LoadFromJson must validate version field.");
  AssertContains(sigDb, "\"version\"", "SignatureDatabase::LoadFromJson must validate version field.");

  TestTroubleshootingGuidesJsonHasVersionField();

  return 0;
}
