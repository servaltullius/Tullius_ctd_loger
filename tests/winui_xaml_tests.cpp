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

static void TestMainWindowHasCorrelationBadge()
{
  const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  const auto xaml = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.xaml");
  assert(xaml.find("CorrelationBadge") != std::string::npos);

  const auto cs = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.xaml.cs");
  assert(cs.find("CorrelationBadge") != std::string::npos);

  const auto summary = ReadAllText(repoRoot / "dump_tool_winui" / "AnalysisSummary.cs");
  assert(summary.find("HistoryCorrelationCount") != std::string::npos);
  assert(summary.find("is_snapshot_like") != std::string::npos);
  assert(summary.find("is_hang_like") != std::string::npos);
  assert(summary.find("is_crash_like") != std::string::npos);
}

static void TestMainWindowHasTroubleshootingSection()
{
  const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  const auto xaml = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.xaml");
  assert(xaml.find("TroubleshootingExpander") != std::string::npos);

  const auto cs = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.xaml.cs");
  assert(cs.find("TroubleshootingSteps") != std::string::npos || cs.find("troubleshooting_steps") != std::string::npos);

  const auto summary = ReadAllText(repoRoot / "dump_tool_winui" / "AnalysisSummary.cs");
  assert(summary.find("TroubleshootingSteps") != std::string::npos);
}

int main()
{
  const std::filesystem::path repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
  const std::filesystem::path xamlPath = repoRoot / "dump_tool_winui" / "MainWindow.xaml";

  assert(std::filesystem::exists(xamlPath) && "MainWindow.xaml not found");
  const std::string xaml = ReadAllText(xamlPath);

  // Triage UX: allow users to copy the crash summary section quickly for sharing.
  assert(xaml.find("CopySummaryButton") != std::string::npos && "Copy summary button missing in XAML");
  assert(xaml.find("CopySummaryButton_Click") != std::string::npos && "Copy summary click handler not wired in XAML");

  // Community share copy button for Discord/Reddit
  assert(xaml.find("CopyShareButton") != std::string::npos && "Community share copy button missing in XAML");
  assert(xaml.find("CopyShareButton_Click") != std::string::npos && "Community share click handler not wired in XAML");

  const auto cs = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.xaml.cs");
  assert(cs.find("Skyrim Snapshot Report") != std::string::npos && "Snapshot community share headline missing");
  assert(cs.find("Skyrim Freeze/ILS Report") != std::string::npos && "Hang community share headline missing");

  TestMainWindowHasCorrelationBadge();
  TestMainWindowHasTroubleshootingSection();
  return 0;
}
