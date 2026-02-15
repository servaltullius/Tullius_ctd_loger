#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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

static void TestJsonFileExists()
{
  const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
  assert(root && "SKYDIAG_PROJECT_ROOT must be set");

  const std::filesystem::path p = std::filesystem::path(root) / "dump_tool" / "data" / "hook_frameworks.json";
  assert(std::filesystem::exists(p) && "hook_frameworks.json must exist");
}

static void TestKnownDllsPresent()
{
  const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
  assert(root && "SKYDIAG_PROJECT_ROOT must be set");

  const std::filesystem::path p = std::filesystem::path(root) / "dump_tool" / "data" / "hook_frameworks.json";
  const std::string content = ReadAllText(p);

  // Original hardcoded core list.
  assert(content.find("enginefixes.dll") != std::string::npos);
  assert(content.find("crashlogger.dll") != std::string::npos);
  assert(content.find("crashloggersse.dll") != std::string::npos);
  assert(content.find("sl.interposer.dll") != std::string::npos);
  assert(content.find("skse64.dll") != std::string::npos);
  assert(content.find("po3_tweaks.dll") != std::string::npos);
  assert(content.find("hdtsmp64.dll") != std::string::npos);

  // Added injection/framework examples.
  assert(content.find("d3d11.dll") != std::string::npos);
  assert(content.find("dxgi.dll") != std::string::npos);
}

static void TestEvidenceBuilderDelegatesToMinidumpUtil()
{
  const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
  assert(root && "SKYDIAG_PROJECT_ROOT must be set");

  const std::filesystem::path evidencePath = std::filesystem::path(root) / "dump_tool" / "src" / "EvidenceBuilderInternalsUtil.cpp";
  const std::string content = ReadAllText(evidencePath);
  assert(content.find("minidump::IsKnownHookFramework") != std::string::npos);
}

int main()
{
  TestJsonFileExists();
  TestKnownDllsPresent();
  TestEvidenceBuilderDelegatesToMinidumpUtil();
  return 0;
}
