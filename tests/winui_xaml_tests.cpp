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
  const auto raw = ss.str();
  std::string normalized;
  normalized.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') {
      continue;
    }
    normalized.push_back(raw[i]);
  }
  return normalized;
}

static std::string ReadMainWindowViewModelText(const std::filesystem::path& repoRoot)
{
  std::ostringstream ss;
  ss << ReadAllText(repoRoot / "dump_tool_winui" / "MainWindowViewModel.cs");
  ss << ReadAllText(repoRoot / "dump_tool_winui" / "MainWindowViewModel.DumpDiscovery.cs");
  ss << ReadAllText(repoRoot / "dump_tool_winui" / "MainWindowViewModel.Recommendations.cs");
  ss << ReadAllText(repoRoot / "dump_tool_winui" / "MainWindowViewModel.Candidates.cs");
  ss << ReadAllText(repoRoot / "dump_tool_winui" / "MainWindowViewModel.ShareText.cs");
  return ss.str();
}

static std::string ReadMainWindowCodeBehindText(const std::filesystem::path& repoRoot)
{
  std::ostringstream ss;
  ss << ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.xaml.cs");
  ss << ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.Localization.cs");
  ss << ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.DumpDiscovery.cs");
  ss << ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.Analysis.cs");
  ss << ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.Triage.cs");
  ss << ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.Layout.cs");
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

  const auto cs = ReadMainWindowCodeBehindText(repoRoot);
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

  const auto cs = ReadMainWindowCodeBehindText(repoRoot);
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

  const auto cs = ReadMainWindowCodeBehindText(repoRoot);
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

  const auto vm = ReadMainWindowViewModelText(repoRoot);
  RequireContains(vm, "CrashLoggerContextSummary", "View model must expose a CrashLogger-first context summary.");
  RequireContains(vm, "CrashContextSummary", "View model must expose a lower-priority crash context summary.");
  RequireContains(vm, "RecommendationGroups", "View model must expose grouped recommendations.");
  RequireContains(vm, "BuildRecommendationGroups", "View model must build grouped recommendations.");
  RequireContains(vm, "BuildConflictComparisonRows", "View model must build conflict comparison rows.");
}

static void TestMainWindowCrashLoggerFrameFirstWordingAlignment()
{
  const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  const auto xaml = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.xaml");
  RequireContains(xaml, "CrashLogger-first reading path", "UI must keep the CrashLogger-first reading-path section aligned.");

  const auto localization = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.Localization.cs");
  RequireContains(localization, "CrashLogger context", "CrashLogger-first quick label must remain visible.");

  const auto vm = ReadMainWindowViewModelText(repoRoot);
  RequireContains(vm, "Crash Logger frame", "CrashLogger-first wording must align with the stronger frame-first CTD output.");
  RequireContains(vm, "DLL guidance", "Frame-backed DLL guidance should be reflected in the WinUI reading path copy.");
}

static void TestMainWindowCrashLoggerExpandedFixtureWordingAlignment()
{
  const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  const auto summary = ReadAllText(repoRoot / "dump_tool_winui" / "AnalysisSummary.cs");
  RequireContains(
    summary,
    "ReadInt32(crashLoggerNode, \"frame_signal_strength\")",
    "WinUI summary loader must keep consuming Crash Logger frame signal strength.");
  RequireContains(
    summary,
    "CrashLoggerDirectFaultModule = ReadString(crashLoggerNode, \"direct_fault_module\")",
    "WinUI summary loader must keep consuming direct DLL fault modules.");
  RequireContains(
    summary,
    "CrashLoggerFirstActionableProbableModule = ReadString(crashLoggerNode, \"first_actionable_probable_module\")",
    "WinUI summary loader must keep consuming first actionable probable DLL modules.");
  RequireContains(
    summary,
    "CrashLoggerProbableStreakModule = ReadString(crashLoggerNode, \"probable_streak_module\")",
    "WinUI summary loader must keep consuming probable streak DLL modules.");
  RequireContains(
    summary,
    "CrashLoggerDirectFaultEligible = ReadBool(crashLoggerNode, \"direct_fault_eligible\")",
    "WinUI must parse actionable eligibility separately from raw Crash Logger observations.");
  RequireContains(
    summary,
    "HasCrashLoggerFrameEligibilityMetadata = hasCrashLoggerFrameEligibilityMetadata",
    "WinUI must distinguish legacy summaries from eligibility-aware summaries.");

  const auto vm = ReadMainWindowViewModelText(repoRoot);
  RequireContains(
    vm,
    "IsCrashLoggerDirectFaultActionable(summary)",
    "Frame-first wording must be gated by actionable eligibility.");
  RequireContains(
    vm,
    "not promoted as a causal candidate",
    "Ineligible raw frames must be labeled as observations rather than causes.");
  RequireContains(
    vm,
    "LegacyCrashLoggerFrameCandidateMatches",
    "Legacy frame summaries must require candidate-level module evidence instead of reusing aggregate strength.");
  RequireNotContains(
    vm,
    "!summary.HasCrashLoggerFrameEligibilityMetadata && summary.CrashLoggerFrameSignalStrength > 0",
    "Legacy aggregate strength must not make every raw frame actionable.");
  RequireContains(
    vm,
    "Crash Logger frame first probable DLL frame",
    "Expanded WinUI wording must keep first probable DLL frame guidance.");
  RequireContains(
    vm,
    "Crash Logger frame first probable frame streak x",
    "Expanded WinUI wording must keep probable streak guidance.");
  RequireContains(
    vm,
    "DLL guidance conflict",
    "Expanded WinUI wording must keep DLL guidance conflict copy for disagreement cases.");
  RequireContains(
    vm,
    "Corroborated fault location",
    "Related Crash Logger frame plus same-event stack must be labeled as fault-location corroboration, not independent agreement.");
  RequireContains(
    vm,
    "상호 확인된 오류 위치",
    "Korean WinUI wording must distinguish fault-location corroboration from independent validation.");
  RequireContains(
    vm,
    "Crash Logger frame + first-chance",
    "Expanded WinUI wording must expose frame plus first-chance agreement when both support the DLL candidate.");
  RequireContains(
    vm,
    "Crash Logger frame + history",
    "Expanded WinUI wording must expose frame plus history agreement when repeated bucket evidence supports the DLL candidate.");
  RequireContains(
    vm,
    "Crash Logger frame + resource",
    "Expanded WinUI wording must expose frame plus resource agreement when nearby resource providers support the DLL candidate.");
  RequireContains(
    vm,
    "Tullius callstack first",
    "Expanded WinUI wording must expose standalone Tullius callstack-first guidance for strong stackwalk-backed candidates.");
  RequireContains(
    vm,
    "Tullius callstack: check",
    "Expanded WinUI wording must surface a standalone Tullius callstack next action for strong stackwalk-backed candidates.");
}

static void TestMainWindowShareTextUsesCrashLoggerReadingPath()
{
  const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto shareText = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindowViewModel.ShareText.cs");

  RequireContains(
    shareText,
    "BuildCrashLoggerContextSummary(summary)",
    "Share text must reuse the CrashLogger-first reading path summary.");
  RequireContains(
    shareText,
    "BuildNextActionSummary(summary)",
    "Share text must reuse the next-action summary instead of picking a raw recommendation line.");
  RequireContains(
    shareText,
    "CrashLogger context: ",
    "Share text must label the CrashLogger-first reading path explicitly.");
  RequireContains(
    shareText,
    "Next action: ",
    "Share text must label the next action explicitly.");
  RequireContains(
    shareText,
    "유력 후보",
    "Korean fallback share text must describe an uncorroborated suspect as a candidate.");
  RequireNotContains(
    shareText,
    "\"유력 원인\"",
    "Korean fallback share text must not present a heuristic suspect as a confirmed cause.");
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

  const auto vm = ReadMainWindowViewModelText(repoRoot);
  RequireContains(vm, "RecentDumps", "View model must expose recent dump items.");
  RequireContains(vm, "DumpSearchLocations", "View model must expose registered dump search locations.");
  RequireContains(vm, "DumpDiscoveryItem", "View model must define a recent-dump item model.");
  RequireContains(vm, "DumpSearchLocationItem", "View model must define a dump-search-location item model.");

  const auto cs = ReadMainWindowCodeBehindText(repoRoot);
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
  RequireContains(service, "Tullius Ctd Logs", "Discovery service must know the default output subfolder name.");
  RequireContains(service, "BuildDefaultOutputRoots", "Discovery service must build new and legacy blank-default roots explicitly.");
  RequireContains(
    service,
    "ResolveHelperDirectoryFromBaseDirectory",
    "Dump discovery must support the launcher layout where the real WinUI app lives under an app subfolder.");
  RequireContains(
    service,
    "string.Equals(baseInfo.Name, \"app\", StringComparison.OrdinalIgnoreCase)",
    "Dump discovery must step out of the app subfolder before looking for SkyrimDiagHelper.ini.");
  RequireContains(
    service,
    "Path.Combine(mo2BaseDirectory, \"overwrite\", \"SKSE\", \"Plugins\", \"Tullius Ctd Logs\")",
    "MO2 blank default should target the dedicated output subfolder.");
  RequireNotContains(service, "CrashDumps", "Generic CrashDumps fallback should not appear in output-root-only discovery.");

  const auto cs = ReadMainWindowCodeBehindText(repoRoot);
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

  const auto vm = ReadMainWindowViewModelText(repoRoot);
  RequireContains(vm, "ShowRecaptureContext", "View model must expose recapture-context visibility.");
  RequireContains(vm, "RecaptureContextTitle", "View model must expose recapture-context title.");
  RequireContains(vm, "RecaptureContextDetails", "View model must expose recapture-context details.");
  RequireContains(vm, "PopulateRecaptureContext", "View model must compute recapture context from summary metadata.");

  const auto xaml = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.xaml");
  RequireContains(xaml, "RecaptureContextCard", "Recommendations area must render a dedicated recapture-context card.");
  RequireContains(xaml, "RecaptureContextTitleText", "Recapture context card must expose a title text element.");
  RequireContains(xaml, "RecaptureContextDetailsText", "Recapture context card must expose a detail text element.");

  const auto cs = ReadMainWindowCodeBehindText(repoRoot);
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

  const auto vm = ReadMainWindowViewModelText(repoRoot);
  RequireContains(vm, "candidate.PrimaryIdentifier", "WinUI titles must prefer the representative primary identifier.");
  RequireContains(vm, "candidate.SecondaryLabel", "WinUI subordinate text must retain the secondary label.");
}

static void TestWinUiRawDataTextBoxesExposeScrollbars()
{
  const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto xaml = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.xaml");

  RequireContains(
    xaml,
    "x:Name=\"WctTextBox\"\n                         IsReadOnly=\"True\" AcceptsReturn=\"True\" TextWrapping=\"NoWrap\"\n                         ScrollViewer.HorizontalScrollBarVisibility=\"Auto\"\n                         ScrollViewer.VerticalScrollBarVisibility=\"Auto\"",
    "Raw Data WCT textbox must expose both horizontal and vertical scrollbars.");
  RequireContains(
    xaml,
    "x:Name=\"ReportTextBox\"\n                         IsReadOnly=\"True\" AcceptsReturn=\"True\" TextWrapping=\"Wrap\"\n                         ScrollViewer.HorizontalScrollBarVisibility=\"Disabled\"\n                         ScrollViewer.VerticalScrollBarVisibility=\"Auto\"",
    "Raw Data report textbox must expose a stable vertical scrollbar.");
}

static void TestWinUiUsesAvailableWidthAndStacksQuickCards()
{
  const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto xaml = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.xaml");

  RequireContains(
    xaml,
    "x:Name=\"RootScrollViewer\"\n          HorizontalContentAlignment=\"Stretch\"",
    "Root scroll content must stretch into the available window width.");
  RequireContains(
    xaml,
    "x:Name=\"RootContentGrid\"\n              MaxWidth=\"1240\"\n              MinWidth=\"640\"",
    "Root content must retain the bounded wide layout while using available width.");
  RequireContains(xaml, "x:Name=\"QuickSummaryGrid\"", "Quick-summary grid must remain addressable by adaptive layout code.");
  RequireContains(xaml, "x:Name=\"EvidenceAgreementCard\"", "Evidence agreement card must be named for narrow stacking.");
  RequireContains(xaml, "x:Name=\"NextActionCard\"", "Next-action card must be named for narrow stacking.");

  const auto cs = ReadMainWindowCodeBehindText(repoRoot);
  RequireContains(cs, "tier == LayoutTier.Narrow ? 0", "Narrow layout must not force a desktop minimum width.");
  RequireContains(cs, "RootScrollViewer.ViewportWidth", "Adaptive layout must size content from the real scroll viewport.");
  RequireContains(cs, "RootContentGrid.Width = Math.Clamp", "Adaptive layout must explicitly fill the available viewport up to its maximum width.");
  RequireContains(cs, "Grid.SetRow(EvidenceAgreementCard, 1)", "Narrow layout must stack evidence agreement below CrashLogger context.");
  RequireContains(cs, "Grid.SetRow(NextActionCard, 2)", "Narrow layout must stack next action below evidence agreement.");
}

static void TestWinUiUsesFocusedEmptyStates()
{
  const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto xaml = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.xaml");

  RequireContains(xaml, "x:Name=\"TriageEmptyStateCard\"", "Triage must show a focused pre-analysis empty state.");
  RequireContains(xaml, "x:Name=\"RawDataEmptyStateCard\"", "Raw Data must show a focused empty state before artifacts exist.");
  RequireContains(xaml, "x:Name=\"WctCard\"\n                    Visibility=\"Collapsed\"", "WCT card must start collapsed instead of showing an empty shell.");
  RequireContains(xaml, "x:Name=\"ReportCard\"\n                    Visibility=\"Collapsed\"", "Report card must start collapsed instead of showing an empty shell.");

  const auto cs = ReadMainWindowCodeBehindText(repoRoot);
  RequireContains(cs, "SetAnalysisContentVisibility(false)", "Main window must start with analysis-result cards hidden.");
  RequireContains(cs, "SetRawDataContentVisibility(hasReport: false, hasWct: false)", "Main window must start with raw-data artifact cards hidden.");
  RequireContains(cs, "SetAnalysisContentVisibility(true)", "Successful analysis must reveal analysis-result cards.");
  RequireContains(cs, "SetRawDataContentVisibility(artifacts.HasReport, artifacts.HasWct)", "Raw Data visibility must follow real artifact availability.");

  const auto vm = ReadMainWindowViewModelText(repoRoot);
  RequireContains(vm, "public bool HasReport", "Advanced artifacts must record report availability.");
  RequireContains(vm, "public bool HasWct", "Advanced artifacts must record WCT availability.");
}

static void TestWinUiKoreanStaticCopyIsLocalized()
{
  const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto localization = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.Localization.cs");
  RequireContains(localization, "준비됨.", "Korean UI must localize the idle status.");
  RequireContains(localization, "문제 해결 절차", "Korean UI must localize the troubleshooting title.");
  RequireContains(localization, "리포트 및 원시 데이터", "Korean UI must localize the Raw Data section title.");

  const auto cs = ReadMainWindowCodeBehindText(repoRoot);
  RequireContains(cs, "SkyrimDiagDumpToolNative.dll을 SkyrimDiagDumpToolWinUI.exe 옆에서 찾지 못했습니다.", "Korean UI must localize the missing-native startup warning.");

  const auto vm = ReadMainWindowViewModelText(repoRoot);
  RequireContains(vm, "Tullius 콜스택 첫 후보", "Korean candidate copy must localize the Tullius callstack-first label.");
  RequireContains(vm, "DLL 점검 안내", "Korean recommendation copy must localize DLL guidance.");
  RequireContains(vm, "Crash Logger 직접 오류 프레임", "Korean candidate copy must localize the direct-fault frame label.");
}

static void TestReviewFeedbackFollowsPrimaryGuidance()
{
  const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto xaml = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.xaml");

  const auto crashContext = xaml.find("x:Name=\"CrashContextCard\"");
  const auto recommendations = xaml.find("x:Name=\"NextStepsSectionTitleText\"");
  const auto reviewFeedback = xaml.find("x:Name=\"ReviewFeedbackCard\"");
  if (crashContext == std::string::npos || recommendations == std::string::npos ||
      reviewFeedback == std::string::npos || !(crashContext < recommendations && recommendations < reviewFeedback)) {
    std::cerr << "Review feedback must follow crash context and recommended actions in the reading order.\n";
    std::exit(1);
  }
}

static void TestWinUiRootScrollViewerKeepsStableVerticalScrollbarWidth()
{
  const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto xaml = ReadAllText(repoRoot / "dump_tool_winui" / "MainWindow.xaml");

  RequireContains(
    xaml,
    "VerticalScrollBarVisibility=\"Visible\"",
    "Root scroll viewer must reserve vertical scrollbar width to prevent Triage layout shifts.");
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

  const auto vm = ReadMainWindowViewModelText(repoRoot);
  assert(vm.find("Skyrim Snapshot Report") != std::string::npos && "Snapshot community share headline missing");
  assert(vm.find("Skyrim Freeze/ILS Report") != std::string::npos && "Hang community share headline missing");
  assert(vm.find("Cross-validated candidate") != std::string::npos && "Community share text must expose cross-validated wording");
  assert(vm.find("Actionable candidate") != std::string::npos && "View model must expose actionable-candidate wording");
  assert(vm.find("Conflicting candidates") != std::string::npos && "View model must expose conflicting-candidates wording");
  RequireContains(vm, "BuildNextActionSummary", "Quick actions card must summarize the next candidate-specific action.");
  RequireContains(vm, "BuildConflictCandidateLine", "Conflict UX must explain each conflicting candidate separately in share/clipboard text.");
  RequireContains(vm, "CrashLoggerContextSummary", "Top reading path must expose CrashLogger-first context.");
  RequireContains(vm, "RecommendationGroups", "View model must expose grouped recommendation collections.");

  const auto cs = ReadMainWindowCodeBehindText(repoRoot);
  assert(cs.find("CrashLogger context") != std::string::npos && "Quick primary label must use CrashLogger-first wording");
  assert(cs.find("Evidence agreement") != std::string::npos && "Quick agreement label must use evidence-agreement wording");
  RequireContains(cs, "Next action", "Quick actions label must focus on the next action, not only a count.");

  TestMainWindowHasCorrelationBadge();
  TestMainWindowHasTroubleshootingSection();
  TestMainWindowHasTriageReviewEditor();
  TestMainWindowHasCrashLoggerFirstReadingPath();
  TestMainWindowCrashLoggerFrameFirstWordingAlignment();
  TestMainWindowCrashLoggerExpandedFixtureWordingAlignment();
  TestMainWindowShareTextUsesCrashLoggerReadingPath();
  TestAnalyzePanelHasDumpDiscoveryFlow();
  TestDumpDiscoveryUsesOutputLocationsOnly();
  TestWinUiConsumesRecaptureContext();
  TestWinUiUsesRepresentativeCandidateIdentifiers();
  TestWinUiRawDataTextBoxesExposeScrollbars();
  TestWinUiUsesAvailableWidthAndStacksQuickCards();
  TestWinUiUsesFocusedEmptyStates();
  TestWinUiKoreanStaticCopyIsLocalized();
  TestReviewFeedbackFollowsPrimaryGuidance();
  TestWinUiRootScrollViewerKeepsStableVerticalScrollbarWidth();

  // Accessibility: interactive elements must have AutomationProperties.Name
  assert(xaml.find("AutomationProperties.Name") != std::string::npos && "No AutomationProperties.Name found in XAML");

  // Keyboard accessibility: primary actions should have KeyboardAccelerator
  assert(xaml.find("KeyboardAccelerator") != std::string::npos && "No KeyboardAccelerator found in XAML");

  return 0;
}
