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

static void TestMainWindowHasTriageReviewEditor()
{
  const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  const auto xaml = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.xaml");
  assert(xaml.find("ReviewStatusComboBox") != std::string::npos && "Review status selector missing in XAML");
  assert(xaml.find("ReviewStatusConfirmedItem") != std::string::npos && "Confirmed review status option missing in XAML");
  assert(xaml.find("ReviewStatusTriagedItem") != std::string::npos && "Triaged review status option missing in XAML");
  assert(xaml.find("ReviewStatusDoneItem") != std::string::npos && "Done review status option missing in XAML");
  assert(xaml.find("SaveTriageButton") != std::string::npos && "Save triage button missing in XAML");
  assert(xaml.find("GroundTruthModBox") != std::string::npos && "Ground truth mod field missing in XAML");
  assert(xaml.find("ReviewNotesBox") != std::string::npos && "Review notes field missing in XAML");

  const auto cs = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.xaml.cs");
  assert(cs.find("SaveTriageButton_Click") != std::string::npos && "Save triage click handler missing");
  assert(cs.find("PopulateTriageEditor") != std::string::npos && "Triage editor population logic missing");
  assert(cs.find("DescribeReviewStatus") != std::string::npos && "Review status formatter missing");

  const auto summary = ReadAllText(repoRoot / "dump_tool_winui" / "AnalysisSummary.cs");
  assert(summary.find("Triage = ParseTriage") != std::string::npos && "AnalysisSummary must load triage fields");

  const auto store = ReadAllText(repoRoot / "dump_tool_winui" / "SummaryTriageStore.cs");
  assert(store.find("SaveAsync") != std::string::npos && "Summary triage save helper missing");
  assert(store.find("HasReviewContent") != std::string::npos && "Summary triage review-content helper missing");
  assert(store.find("\"confirmed\" => \"confirmed\"") != std::string::npos && "Legacy review status values must round-trip");
  assert(store.find("\"ground_truth_mod\"") != std::string::npos && "Summary triage save helper must persist ground_truth_mod");
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

  const auto vm = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindowViewModel.cs");
  assert(vm.find("Skyrim Snapshot Report") != std::string::npos && "Snapshot community share headline missing");
  assert(vm.find("Skyrim Freeze/ILS Report") != std::string::npos && "Hang community share headline missing");
  assert(vm.find("Cross-validated candidate") != std::string::npos && "Community share text must expose cross-validated wording");
  assert(vm.find("Actionable candidate") != std::string::npos && "View model must expose actionable-candidate wording");
  assert(vm.find("Conflicting candidates") != std::string::npos && "View model must expose conflicting-candidates wording");
  RequireContains(vm, "BuildNextActionSummary", "Quick actions card must summarize the next candidate-specific action.");
  RequireContains(vm, "BuildConflictCandidateLine", "Conflict UX must explain each conflicting candidate separately in share/clipboard text.");

  const auto cs = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.xaml.cs");
  assert(cs.find("Actionable candidate") != std::string::npos && "Quick primary label must use actionable-candidate wording");
  assert(cs.find("Evidence agreement") != std::string::npos && "Quick agreement label must use evidence-agreement wording");
  RequireContains(cs, "Next action", "Quick actions label must focus on the next action, not only a count.");

  TestMainWindowHasCorrelationBadge();
  TestMainWindowHasTroubleshootingSection();
  TestMainWindowHasTriageReviewEditor();

  // Accessibility: interactive elements must have AutomationProperties.Name
  assert(xaml.find("AutomationProperties.Name") != std::string::npos && "No AutomationProperties.Name found in XAML");

  // Keyboard accessibility: primary actions should have KeyboardAccelerator
  assert(xaml.find("KeyboardAccelerator") != std::string::npos && "No KeyboardAccelerator found in XAML");

  return 0;
}
