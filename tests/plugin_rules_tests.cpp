#include <cassert>
#include <filesystem>
#include <string>

#include "SourceGuardTestUtils.h"

namespace {
using skydiag::tests::source_guard::ProjectRoot;
using skydiag::tests::source_guard::ReadProjectText;

void TestPluginRulesJsonExists()
{
  const std::filesystem::path p = ProjectRoot() / "dump_tool" / "data" / "plugin_rules.json";
  assert(std::filesystem::exists(p));
}

void TestHasBeesRule()
{
  const auto content = ReadProjectText("dump_tool/data/plugin_rules.json");
  assert(content.find("BEES") != std::string::npos || content.find("bees") != std::string::npos);
  assert(content.find("1.71") != std::string::npos);
}

void TestHasMissingMasterRule()
{
  const auto content = ReadProjectText("dump_tool/data/plugin_rules.json");
  assert(content.find("MISSING_MASTER") != std::string::npos);
}

void TestAnalyzerReadsPluginStream()
{
  const auto impl = ReadProjectText("dump_tool/src/Analyzer.cpp");
  assert(impl.find("kMinidumpUserStream_PluginInfo") != std::string::npos);
}

void TestOutputHasPluginSection()
{
  const auto impl = ReadProjectText("dump_tool/src/OutputWriter.cpp");
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
