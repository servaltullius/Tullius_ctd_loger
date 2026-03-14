#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string ReadFile(const char* relPath)
{
  const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
  assert(root);
  std::filesystem::path p = std::filesystem::path(root) / relPath;
  std::ostringstream ss;
  const auto append = [&](const std::filesystem::path& path) {
    std::ifstream f(path);
    assert(f.is_open());
    ss << f.rdbuf();
  };
  append(p);
  if (p.filename() == "Analyzer.cpp") {
    append(p.parent_path() / "Analyzer.CaptureInputs.cpp");
    append(p.parent_path() / "Analyzer.History.cpp");
  }
  if (p.filename() == "OutputWriter.cpp") {
    append(p.parent_path() / "OutputWriter.Summary.cpp");
    append(p.parent_path() / "OutputWriter.Report.cpp");
  }
  return ss.str();
}

void TestPluginRulesJsonExists()
{
  const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
  assert(root);
  std::filesystem::path p = std::filesystem::path(root) / "dump_tool" / "data" / "plugin_rules.json";
  assert(std::filesystem::exists(p));
}

void TestHasBeesRule()
{
  const auto content = ReadFile("dump_tool/data/plugin_rules.json");
  assert(content.find("BEES") != std::string::npos || content.find("bees") != std::string::npos);
  assert(content.find("1.71") != std::string::npos);
}

void TestHasMissingMasterRule()
{
  const auto content = ReadFile("dump_tool/data/plugin_rules.json");
  assert(content.find("MISSING_MASTER") != std::string::npos);
}

void TestAnalyzerReadsPluginStream()
{
  const auto impl = ReadFile("dump_tool/src/Analyzer.cpp");
  assert(impl.find("kMinidumpUserStream_PluginInfo") != std::string::npos);
}

void TestOutputHasPluginSection()
{
  const auto impl = ReadFile("dump_tool/src/OutputWriter.cpp");
  assert(impl.find("\"plugin_scan\"") != std::string::npos);
  assert(impl.find("\"missing_masters\"") != std::string::npos);
  assert(impl.find("\"needs_bees\"") != std::string::npos);
}

}  // namespace

int main()
{
  TestPluginRulesJsonExists();
  TestHasBeesRule();
  TestHasMissingMasterRule();
  TestAnalyzerReadsPluginStream();
  TestOutputHasPluginSection();
  return 0;
}
