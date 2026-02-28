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

int main()
{
  const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto src = ReadAllText(repoRoot / "dump_tool_winui" / "AnalysisSummary.cs");

  // Verify new helpers exist
  assert(src.find("ParseStringArray") != std::string::npos && "ParseStringArray helper missing");
  assert(src.find("ParseObjectArray") != std::string::npos && "ParseObjectArray helper missing");

  // Verify LoadFromSummaryFile calls both helpers
  assert(src.find("ParseStringArray(root,") != std::string::npos && "ParseStringArray not called from LoadFromSummaryFile");
  assert(src.find("ParseObjectArray(root,") != std::string::npos && "ParseObjectArray not called from LoadFromSummaryFile");

  // Verify existing helpers are still present
  assert(src.find("ReadString(") != std::string::npos && "ReadString helper missing");
  assert(src.find("ReadBool(") != std::string::npos && "ReadBool helper missing");
  assert(src.find("FirstNonEmpty(") != std::string::npos && "FirstNonEmpty helper missing");

  // Verify dotted path support is used (callstack.frames)
  assert(src.find("\"callstack.frames\"") != std::string::npos && "Dotted path support for callstack.frames missing");

  return 0;
}
