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

static void RequireNotContains(const std::string& haystack, const char* needle, const char* message)
{
  if (haystack.find(needle) != std::string::npos) {
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
  assert(xaml.find("ReviewFeedbackExpander") != std::string::npos && "Review feedback should be wrapped in a secondary expander.");
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

static void TestMainWindowHasCrashLoggerFirstReadingPath()
{
  const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  const auto xaml = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.xaml");
  RequireContains(xaml, "CrashLoggerContextCard", "Triage surface must show a dedicated CrashLogger-first context card.");
  RequireContains(xaml, "CrashContextCard", "Fault-module details should live in a lower-priority crash-context card.");
  RequireContains(xaml, "ConflictCandidatesPanel", "Conflicting candidates should be rendered in their own comparison block.");
  RequireContains(xaml, "RecommendationGroupsPanel", "Recommendations should be grouped by action type, not only shown as a flat list.");
  RequireContains(xaml, "Do This Now", "Grouped recommendation UI must expose an immediate-action heading.");
  RequireContains(xaml, "Recapture or Compare", "Grouped recommendation UI must expose a recapture/compare heading.");

  const auto vm = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindowViewModel.cs");
  RequireContains(vm, "CrashLoggerContextSummary", "View model must expose a CrashLogger-first context summary.");
  RequireContains(vm, "CrashContextSummary", "View model must expose a lower-priority crash context summary.");
  RequireContains(vm, "RecommendationGroups", "View model must expose grouped recommendations.");
  RequireContains(vm, "BuildRecommendationGroups", "View model must build grouped recommendations.");
  RequireContains(vm, "BuildConflictComparisonRows", "View model must build conflict comparison rows.");
}

static void TestAnalyzePanelHasDumpDiscoveryFlow()
{
  const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  const auto xaml = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.xaml");
  RequireContains(xaml, "RecentDumpsCard", "Analyze/start screen must expose a recent-dumps discovery card.");
  RequireContains(xaml, "RecentDumpList", "Analyze/start screen must render discovered dumps in a list.");
  RequireContains(xaml, "ManageDumpFoldersButton", "Analyze/start screen must expose a folder-management entry point.");
  RequireContains(xaml, "RescanDumpsButton", "Analyze/start screen must expose a rescan action.");
  RequireContains(xaml, "DirectSelectDumpButton", "Analyze/start screen must keep a direct dump-selection action.");
  RequireContains(xaml, "MO2 overwrite", "Empty state guidance must directly mention MO2 overwrite.");
  RequireContains(xaml, "덤프 출력 위치", "Folder-management UX must use dump-output-location wording.");

  const auto vm = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindowViewModel.cs");
  RequireContains(vm, "RecentDumps", "View model must expose recent dump items.");
  RequireContains(vm, "DumpSearchLocations", "View model must expose registered dump search locations.");
  RequireContains(vm, "DumpDiscoveryItem", "View model must define a recent-dump item model.");
  RequireContains(vm, "DumpSearchLocationItem", "View model must define a dump-search-location item model.");

  const auto cs = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.xaml.cs");
  RequireContains(cs, "RefreshDiscoveredDumpsAsync", "Main window must refresh discovered dumps on startup and after folder changes.");
  RequireContains(cs, "AnalyzeRecentDump_Click", "Analyze/start screen must let users analyze a discovered dump directly.");
  RequireContains(cs, "ManageDumpFoldersButton_Click", "Folder-management entry point handler missing.");
  RequireContains(cs, "AddDumpSearchLocation_Click", "Folder-management UI must let users add dump search locations.");
  RequireContains(cs, "RemoveDumpSearchLocation_Click", "Folder-management UI must let users remove dump search locations.");

  const auto store = ReadAllText(repoRoot / "dump_tool_winui" / "DumpDiscoveryStore.cs");
  RequireContains(store, "RegisteredRoots", "Dump discovery store must persist registered search roots.");
  RequireContains(store, "LearnedRoots", "Dump discovery store must persist learned search roots.");
}

static void TestDumpDiscoveryUsesOutputLocationsOnly()
{
  const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  const auto service = ReadAllText(repoRoot / "dump_tool_winui" / "DumpDiscoveryService.cs");
  RequireContains(service, "SkyrimDiagHelper.ini", "Discovery service must inspect SkyrimDiagHelper.ini to infer the real output root.");
  RequireContains(service, "OutputDir", "Discovery service must honor SkyrimDiagHelper.ini OutputDir.");
  RequireContains(service, "overwrite", "Discovery service must infer MO2 overwrite when OutputDir is blank.");
  RequireNotContains(service, "CrashDumps", "Generic CrashDumps fallback should not appear in output-root-only discovery.");

  const auto cs = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.xaml.cs");
  RequireContains(cs, "덤프 출력 위치", "User-facing copy must talk about dump output locations.");
  RequireContains(cs, "OutputDir", "Empty-state/help copy must mention OutputDir for custom output roots.");

  const auto store = ReadAllText(repoRoot / "dump_tool_winui" / "DumpDiscoveryStore.cs");
  RequireContains(store, "CrashDumps", "Store migration must recognize the legacy CrashDumps root.");
  RequireContains(store, "IsLegacyExcludedRoot", "Store must sanitize legacy non-output roots from persisted state.");

  RequireContains(service, "CanPromoteLearnedRoot", "Discovery service must gate learned-root promotion to supported output roots.");
  RequireContains(cs, "CanPromoteLearnedRoot", "Main window must not learn arbitrary direct-selected dump folders.");
}

static void TestWinUiConsumesRecaptureContext()
{
  const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  const auto summary = ReadAllText(repoRoot / "dump_tool_winui" / "AnalysisSummary.cs");
  RequireContains(summary, "HasRecaptureEvaluation", "AnalysisSummary must expose recapture-evaluation presence.");
  RequireContains(summary, "RecaptureTriggered", "AnalysisSummary must expose recapture trigger state.");
  RequireContains(summary, "RecaptureTargetProfile", "AnalysisSummary must expose recapture target profile.");
  RequireContains(summary, "incident.recapture_evaluation", "AnalysisSummary must parse incident.recapture_evaluation.");

  const auto vm = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindowViewModel.cs");
  RequireContains(vm, "ShowRecaptureContext", "View model must expose recapture-context visibility.");
  RequireContains(vm, "RecaptureContextTitle", "View model must expose recapture-context title.");
  RequireContains(vm, "RecaptureContextDetails", "View model must expose recapture-context details.");
  RequireContains(vm, "PopulateRecaptureContext", "View model must compute recapture context from summary metadata.");

  const auto xaml = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.xaml");
  RequireContains(xaml, "RecaptureContextCard", "Recommendations area must render a dedicated recapture-context card.");
  RequireContains(xaml, "RecaptureContextTitleText", "Recapture context card must expose a title text element.");
  RequireContains(xaml, "RecaptureContextDetailsText", "Recapture context card must expose a detail text element.");

  const auto cs = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.xaml.cs");
  RequireContains(cs, "RecaptureContextCard.Visibility", "Main window must toggle recapture context visibility when rendering.");
  RequireContains(cs, "RecaptureContextTitleText.Text", "Main window must render the recapture-context title.");
  RequireContains(cs, "RecaptureContextDetailsText.Text", "Main window must render the recapture-context details.");
}

static void TestWinUiUsesRepresentativeCandidateIdentifiers()
{
  const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  const auto summary = ReadAllText(repoRoot / "dump_tool_winui" / "AnalysisSummary.cs");
  RequireContains(summary, "ReadString(item, \"primary_identifier\")", "AnalysisSummary must parse actionable candidate primary_identifier.");
  RequireContains(summary, "ReadString(item, \"secondary_label\")", "AnalysisSummary must parse actionable candidate secondary_label.");
  RequireContains(summary, "string PrimaryIdentifier", "ActionableCandidateItem must expose primary_identifier.");
  RequireContains(summary, "string SecondaryLabel", "ActionableCandidateItem must expose secondary_label.");

  const auto vm = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindowViewModel.cs");
  RequireContains(vm, "candidate.PrimaryIdentifier", "WinUI titles must prefer the representative primary identifier.");
  RequireContains(vm, "candidate.SecondaryLabel", "WinUI subordinate text must retain the secondary label.");
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
  RequireContains(vm, "CrashLoggerContextSummary", "Top reading path must expose CrashLogger-first context.");
  RequireContains(vm, "RecommendationGroups", "View model must expose grouped recommendation collections.");

  const auto cs = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.xaml.cs");
  assert(cs.find("CrashLogger context") != std::string::npos && "Quick primary label must use CrashLogger-first wording");
  assert(cs.find("Evidence agreement") != std::string::npos && "Quick agreement label must use evidence-agreement wording");
  RequireContains(cs, "Next action", "Quick actions label must focus on the next action, not only a count.");

  TestMainWindowHasCorrelationBadge();
  TestMainWindowHasTroubleshootingSection();
  TestMainWindowHasTriageReviewEditor();
  TestMainWindowHasCrashLoggerFirstReadingPath();
  TestAnalyzePanelHasDumpDiscoveryFlow();
  TestDumpDiscoveryUsesOutputLocationsOnly();
  TestWinUiConsumesRecaptureContext();
  TestWinUiUsesRepresentativeCandidateIdentifiers();

  // Accessibility: interactive elements must have AutomationProperties.Name
  assert(xaml.find("AutomationProperties.Name") != std::string::npos && "No AutomationProperties.Name found in XAML");

  // Keyboard accessibility: primary actions should have KeyboardAccelerator
  assert(xaml.find("KeyboardAccelerator") != std::string::npos && "No KeyboardAccelerator found in XAML");

  return 0;
}
