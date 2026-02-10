using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Globalization;
using System.Text.Json;

using Microsoft.UI;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml;

using Windows.ApplicationModel.DataTransfer;
using Windows.Storage.Pickers;

namespace SkyrimDiagDumpToolWinUI;

public sealed partial class MainWindow : Window
{
    private readonly DumpToolInvocationOptions _startupOptions;
    private readonly ObservableCollection<SuspectItem> _suspects = new();
    private readonly ObservableCollection<string> _recommendations = new();
    private readonly ObservableCollection<string> _callstackFrames = new();
    private readonly ObservableCollection<EvidenceViewItem> _evidenceItems = new();
    private readonly ObservableCollection<ResourceViewItem> _resourceItems = new();
    private readonly ObservableCollection<string> _eventItems = new();
    private readonly bool _isKorean;
    private bool _isCompactLayout;

    private string? _currentDumpPath;
    private string? _currentOutDir;
    private AnalysisSummary? _currentSummary;

    internal MainWindow(DumpToolInvocationOptions startupOptions, string? startupWarning)
    {
        _startupOptions = startupOptions;
        _isKorean = string.Equals(_startupOptions.Language, "ko", StringComparison.OrdinalIgnoreCase) ||
                    (string.IsNullOrWhiteSpace(_startupOptions.Language) &&
                     string.Equals(CultureInfo.CurrentUICulture.TwoLetterISOLanguageName, "ko", StringComparison.OrdinalIgnoreCase));

        InitializeComponent();

        SystemBackdrop = new MicaBackdrop();

        ApplyLocalizedStaticText();
        ApplyNavigationSelectionEmphasis(NavAnalyze);
        HookWheelChainingForNestedControls();
        RootGrid.SizeChanged += RootGrid_SizeChanged;

        SuspectsList.ItemsSource = _suspects;
        RecommendationsList.ItemsSource = _recommendations;
        CallstackList.ItemsSource = _callstackFrames;
        EvidenceList.ItemsSource = _evidenceItems;
        ResourcesList.ItemsSource = _resourceItems;
        EventsList.ItemsSource = _eventItems;

        CopySummaryButton.IsEnabled = false;

        if (!string.IsNullOrWhiteSpace(startupOptions.DumpPath))
        {
            DumpPathBox.Text = startupOptions.DumpPath!;
        }
        if (!string.IsNullOrWhiteSpace(startupOptions.OutDir))
        {
            OutputDirBox.Text = startupOptions.OutDir!;
        }
        if (!string.IsNullOrWhiteSpace(startupWarning))
        {
            StatusText.Text = startupWarning;
        }

        if (!string.IsNullOrWhiteSpace(_startupOptions.DumpPath))
        {
            DispatcherQueue.TryEnqueue(async () => await AnalyzeAsync());
        }

        ApplyAdaptiveLayout();
    }

    private void ApplyLocalizedStaticText()
    {
        Title = T("Tullius CTD Logger (Nordic UI)", "툴리우스 CTD 로거 (노르딕 UI)");

        // Navigation items
        NavAnalyze.Content = T("Dashboard", "대시보드");
        NavSummary.Content = T("Crash Summary", "크래시 요약");
        NavEvidence.Content = T("Evidence", "근거");
        NavEvents.Content = T("Events", "이벤트");
        NavReport.Content = T("Reports", "리포트");

        HeaderTitleText.Text = T("Recent Crash", "최근 크래시");
        HeaderSubtitleText.Text = T(
            "Skyrim SE detected. Ready for dump triage.",
            "Skyrim SE가 감지되었습니다. 덤프 원인 분석을 시작할 수 있습니다.");
        HeaderBadgeText.Text = T("STATUS READY", "상태 준비됨");

        AnalyzeSectionTitleText.Text = T("Dump Intake", "덤프 입력");
        WorkflowSectionTitleText.Text = T("Crash History Timeline", "크래시 히스토리 타임라인");
        StepOneTitleText.Text = T("01 Select Dump", "01 덤프 선택");
        StepOneDescText.Text = T("Choose latest crash dump from your mod profile.", "모드 프로필에서 최신 크래시 덤프를 선택하세요.");
        StepTwoTitleText.Text = T("02 Analyze", "02 분석 실행");
        StepTwoDescText.Text = T("Parse bucket, callstack, and evidence chains.", "버킷, 콜스택, 근거 체인을 분석합니다.");
        StepThreeTitleText.Text = T("03 Triage", "03 원인 선별");
        StepThreeDescText.Text = T("Start with suspect list, then drill into evidence.", "원인 후보부터 보고 근거를 순서대로 확인하세요.");

        SnapshotSectionTitleText.Text = T("Crash Summary", "크래시 요약");
        NextStepsSectionTitleText.Text = T("Recommended Next Steps", "권장 다음 단계");
        SuspectsSectionTitleText.Text = T("Top Cause Candidates", "주요 원인 후보");
        AdvancedSectionTitleText.Text = T("Evidence & Analysis", "근거 및 분석");
        QuickPrimaryLabelText.Text = T("Primary suspect", "주요 원인");
        QuickConfidenceLabelText.Text = T("Confidence", "신뢰도");
        QuickActionsLabelText.Text = T("Next actions", "권장 조치");
        QuickEventsLabelText.Text = T("Blackbox events", "블랙박스 이벤트");
        QuickPrimaryValueText.Text = "-";
        QuickConfidenceValueText.Text = "-";
        QuickActionsValueText.Text = "-";
        QuickEventsValueText.Text = "-";

        CallstackLabelText.Text = T("Callstack", "콜스택");
        EvidenceLabelText.Text = T("Evidence", "근거");
        ResourcesLabelText.Text = T("Recent Resources", "최근 리소스");
        EventsLabelText.Text = T("Events (Blackbox)", "이벤트 (블랙박스)");
        WctLabelText.Text = T("WCT JSON", "WCT JSON");
        ReportLabelText.Text = T("Report", "리포트");

        DumpPathBox.PlaceholderText = T("Select a .dmp file", ".dmp 파일을 선택하세요");
        OutputDirBox.PlaceholderText = T("Optional output directory (empty = dump folder)", "선택 출력 폴더 (비우면 덤프 폴더)");

        BrowseDumpButton.Content = T("Select dump", "덤프 선택");
        BrowseOutputButton.Content = T("Select folder", "폴더 선택");
        AnalyzeButton.Content = T("ANALYZE NOW", "지금 분석");
        OpenOutputButton.Content = T("Open report folder", "리포트 폴더 열기");
        CopySummaryButton.Content = T("Copy summary", "요약 복사");
    }

    private void HookWheelChainingForNestedControls()
    {
        RootGrid.AddHandler(
            UIElement.PointerWheelChangedEvent,
            new PointerEventHandler(OnRootPointerWheelChanged),
            handledEventsToo: true);
    }

    private void OnRootPointerWheelChanged(object sender, PointerRoutedEventArgs e)
    {
        if (!e.Handled)
        {
            return;
        }

        if (!ShouldChainToRootScrollViewer(e.OriginalSource as DependencyObject))
        {
            return;
        }

        var delta = e.GetCurrentPoint(RootScrollViewer).Properties.MouseWheelDelta;
        if (delta == 0)
        {
            return;
        }

        var targetOffset = Math.Clamp(
            RootScrollViewer.VerticalOffset - delta,
            0.0,
            RootScrollViewer.ScrollableHeight);

        if (Math.Abs(targetOffset - RootScrollViewer.VerticalOffset) < 0.1)
        {
            return;
        }

        RootScrollViewer.ChangeView(horizontalOffset: null, verticalOffset: targetOffset, zoomFactor: null, disableAnimation: true);
    }

    private static bool ShouldChainToRootScrollViewer(DependencyObject? source)
    {
        for (var current = source; current is not null; current = VisualTreeHelper.GetParent(current))
        {
            if (current is TextBox or ListView or ScrollViewer)
            {
                return true;
            }
        }

        return false;
    }

    private async void AnalyzeButton_Click(object sender, RoutedEventArgs e)
    {
        await AnalyzeAsync();
    }

    private async Task AnalyzeAsync()
    {
        var dumpPath = DumpPathBox.Text.Trim();
        if (string.IsNullOrWhiteSpace(dumpPath))
        {
            StatusText.Text = T("Select a .dmp file first.", "먼저 .dmp 파일을 선택하세요.");
            return;
        }

        dumpPath = Path.GetFullPath(dumpPath);
        if (!File.Exists(dumpPath))
        {
            StatusText.Text = T("Dump file not found: ", "덤프 파일을 찾을 수 없습니다: ") + dumpPath;
            return;
        }

        var options = BuildCurrentInvocationOptions(dumpPath, true);
        var outDir = NativeAnalyzerBridge.ResolveOutputDirectory(dumpPath, options.OutDir);
        _currentDumpPath = dumpPath;
        _currentOutDir = outDir;

        SetBusy(true, T("Analyzing dump with native engine...", "네이티브 엔진으로 덤프를 분석 중입니다..."));

        var (exitCode, nativeErr) = await NativeAnalyzerBridge.RunAnalyzeAsync(options, CancellationToken.None);
        if (exitCode != 0)
        {
            var prefix = T("Analysis failed. Exit code: ", "분석 실패. 종료 코드: ");
            var msg = prefix + exitCode;
            if (!string.IsNullOrWhiteSpace(nativeErr))
            {
                msg += "\n" + nativeErr;
            }
            SetBusy(false, msg);
            return;
        }

        var summaryPath = NativeAnalyzerBridge.ResolveSummaryPath(dumpPath, outDir);
        if (!File.Exists(summaryPath))
        {
            SetBusy(false, T("Analysis finished but summary file is missing: ", "분석은 끝났지만 요약 파일이 없습니다: ") + summaryPath);
            return;
        }

        try
        {
            var summary = AnalysisSummary.LoadFromSummaryFile(summaryPath);
            RenderSummary(summary);
            RenderAdvancedArtifacts(dumpPath, outDir);
            SetBusy(false, T("Analysis complete. Review the candidates and checklist.", "분석 완료. 원인 후보와 체크리스트를 확인하세요."));
            NavView.SelectedItem = NavSummary;
        }
        catch (Exception ex)
        {
            SetBusy(false, T("Failed to read summary JSON: ", "요약 JSON을 읽지 못했습니다: ") + ex.Message);
        }
    }

    private void RenderSummary(AnalysisSummary summary)
    {
        _currentSummary = summary;

        SummarySentenceText.Text = string.IsNullOrWhiteSpace(summary.SummarySentence)
            ? T("No summary sentence produced.", "요약 문장이 생성되지 않았습니다.")
            : summary.SummarySentence;

        BucketText.Text = string.IsNullOrWhiteSpace(summary.CrashBucketKey)
            ? T("Crash bucket: unavailable", "크래시 버킷: 없음")
            : T("Crash bucket: ", "크래시 버킷: ") + summary.CrashBucketKey;

        ModuleText.Text = string.IsNullOrWhiteSpace(summary.ModulePlusOffset)
            ? T("Fault module: unavailable", "오류 모듈: 없음")
            : T("Fault module: ", "오류 모듈: ") + summary.ModulePlusOffset;

        ModNameText.Text = string.IsNullOrWhiteSpace(summary.InferredModName)
            ? T("Inferred mod: unavailable", "추정 모드: 없음")
            : T("Inferred mod: ", "추정 모드: ") + summary.InferredModName;

        CopySummaryButton.IsEnabled = true;

        _suspects.Clear();
        foreach (var suspect in summary.Suspects.Take(5))
        {
            _suspects.Add(suspect);
        }
        if (_suspects.Count == 0)
        {
            _suspects.Add(new SuspectItem(
                T("Unknown", "알 수 없음"),
                T("No strong suspect was extracted.", "강한 원인 후보를 추출하지 못했습니다."),
                T("Try sharing the dump + report for deeper analysis.", "덤프 + 리포트를 공유해 추가 분석을 진행하세요.")));
        }
        var primarySuspect = _suspects.FirstOrDefault();
        QuickPrimaryValueText.Text = primarySuspect is null
            ? T("Unknown", "알 수 없음")
            : primarySuspect.Module;
        QuickConfidenceValueText.Text = primarySuspect is null || string.IsNullOrWhiteSpace(primarySuspect.Confidence)
            ? T("Unrated", "미평가")
            : primarySuspect.Confidence;

        _recommendations.Clear();
        foreach (var recommendation in summary.Recommendations.Take(12))
        {
            _recommendations.Add(recommendation);
        }
        var recommendationCount = _recommendations.Count;
        if (_recommendations.Count == 0)
        {
            _recommendations.Add(T("No recommendation text was generated.", "권장 조치 문구가 생성되지 않았습니다."));
        }
        QuickActionsValueText.Text = recommendationCount == 0
            ? T("None", "없음")
            : $"{recommendationCount} {T("items", "개 항목")}";

        _callstackFrames.Clear();
        foreach (var frame in summary.CallstackFrames.Take(160))
        {
            _callstackFrames.Add(frame);
        }
        if (_callstackFrames.Count == 0)
        {
            _callstackFrames.Add(T("No callstack frames were extracted.", "콜스택 프레임을 추출하지 못했습니다."));
        }

        _evidenceItems.Clear();
        foreach (var evidence in summary.EvidenceItems.Take(80))
        {
            _evidenceItems.Add(evidence);
        }
        if (_evidenceItems.Count == 0)
        {
            _evidenceItems.Add(new EvidenceViewItem(
                T("Unknown", "알 수 없음"),
                T("No evidence list was generated.", "근거 목록이 생성되지 않았습니다."),
                ""));
        }

        _resourceItems.Clear();
        foreach (var resource in summary.ResourceItems.Take(120))
        {
            _resourceItems.Add(resource);
        }
        if (_resourceItems.Count == 0)
        {
            _resourceItems.Add(new ResourceViewItem(
                "resource",
                T("No resource traces were found.", "리소스 추적이 없습니다."),
                "-",
                ""));
        }
    }

    private void CopySummaryButton_Click(object sender, RoutedEventArgs e)
    {
        var text = BuildSummaryClipboardText();
        if (string.IsNullOrWhiteSpace(text))
        {
            StatusText.Text = T("No summary to copy yet.", "아직 복사할 요약이 없습니다.");
            return;
        }

        try
        {
            var dataPackage = new DataPackage();
            dataPackage.SetText(text);
            Clipboard.SetContent(dataPackage);
            Clipboard.Flush();
            StatusText.Text = T("Copied crash summary to clipboard.", "크래시 요약을 클립보드에 복사했습니다.");
        }
        catch (Exception ex)
        {
            StatusText.Text = T("Failed to copy to clipboard: ", "클립보드 복사 실패: ") + ex.Message;
        }
    }

    private string? BuildSummaryClipboardText()
    {
        var summary = _currentSummary;
        if (summary is null)
        {
            return null;
        }

        var lines = new List<string>
        {
            _isKorean ? "SkyrimDiag 리포트" : "SkyrimDiag report",
        };

        if (!string.IsNullOrWhiteSpace(_currentDumpPath))
        {
            lines.Add((_isKorean ? "덤프: " : "Dump: ") + _currentDumpPath);
        }

        if (!string.IsNullOrWhiteSpace(summary.SummarySentence))
        {
            lines.Add((_isKorean ? "결론: " : "Conclusion: ") + summary.SummarySentence);
        }

        if (!string.IsNullOrWhiteSpace(summary.CrashBucketKey))
        {
            lines.Add((_isKorean ? "크래시 버킷 키: " : "Crash bucket key: ") + summary.CrashBucketKey);
        }

        if (!string.IsNullOrWhiteSpace(summary.ModulePlusOffset))
        {
            lines.Add((_isKorean ? "Module+Offset: " : "Module+Offset: ") + summary.ModulePlusOffset);
        }

        if (!string.IsNullOrWhiteSpace(summary.InferredModName))
        {
            lines.Add((_isKorean ? "추정 모드: " : "Inferred mod: ") + summary.InferredModName);
        }

        return string.Join(Environment.NewLine, lines);
    }

    private void RenderAdvancedArtifacts(string dumpPath, string outDir)
    {
        _eventItems.Clear();

        var blackboxPath = NativeAnalyzerBridge.ResolveBlackboxPath(dumpPath, outDir);
        if (File.Exists(blackboxPath))
        {
            var lines = File.ReadAllLines(blackboxPath);
            foreach (var line in lines.Skip(Math.Max(0, lines.Length - 200)))
            {
                if (!string.IsNullOrWhiteSpace(line))
                {
                    _eventItems.Add(line);
                }
            }
        }
        var eventCount = _eventItems.Count;
        if (_eventItems.Count == 0)
        {
            _eventItems.Add(T("No blackbox events were found.", "블랙박스 이벤트를 찾지 못했습니다."));
        }
        QuickEventsValueText.Text = eventCount == 0
            ? T("0 events", "0개")
            : $"{eventCount} {T("events", "개")}";

        var reportPath = NativeAnalyzerBridge.ResolveReportPath(dumpPath, outDir);
        ReportTextBox.Text = File.Exists(reportPath)
            ? File.ReadAllText(reportPath)
            : T("Report file not found.", "리포트 파일이 없습니다.");

        var wctPath = NativeAnalyzerBridge.ResolveWctPath(dumpPath, outDir);
        if (File.Exists(wctPath))
        {
            var raw = File.ReadAllText(wctPath);
            try
            {
                using var doc = JsonDocument.Parse(raw);
                WctTextBox.Text = JsonSerializer.Serialize(doc.RootElement, new JsonSerializerOptions
                {
                    WriteIndented = true,
                });
            }
            catch
            {
                WctTextBox.Text = raw;
            }
        }
        else
        {
            WctTextBox.Text = T("WCT file not found for this dump.", "이 덤프에 대한 WCT 파일이 없습니다.");
        }
    }

    private async void BrowseDump_Click(object sender, RoutedEventArgs e)
    {
        var picker = new FileOpenPicker();
        picker.FileTypeFilter.Add(".dmp");
        picker.SuggestedStartLocation = PickerLocationId.DocumentsLibrary;
        WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(this));

        var file = await picker.PickSingleFileAsync();
        if (file is not null)
        {
            DumpPathBox.Text = file.Path;
            StatusText.Text = T("Dump selected.", "덤프 파일을 선택했습니다.");
        }
    }

    private async void BrowseOutputFolder_Click(object sender, RoutedEventArgs e)
    {
        var picker = new FolderPicker();
        picker.FileTypeFilter.Add("*");
        picker.SuggestedStartLocation = PickerLocationId.DocumentsLibrary;
        WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(this));

        var folder = await picker.PickSingleFolderAsync();
        if (folder is not null)
        {
            OutputDirBox.Text = folder.Path;
            StatusText.Text = T("Output folder selected.", "출력 폴더를 선택했습니다.");
        }
    }

    private void OpenOutputButton_Click(object sender, RoutedEventArgs e)
    {
        var outDir = _currentOutDir;
        if (string.IsNullOrWhiteSpace(outDir) || !Directory.Exists(outDir))
        {
            StatusText.Text = T("Output folder is not available yet.", "출력 폴더가 아직 없습니다.");
            return;
        }

        try
        {
            Process.Start(new ProcessStartInfo
            {
                FileName = outDir,
                UseShellExecute = true,
                Verb = "open",
            });
        }
        catch (Exception ex)
        {
            StatusText.Text = T("Failed to open output folder: ", "출력 폴더 열기 실패: ") + ex.Message;
        }
    }

    private DumpToolInvocationOptions BuildCurrentInvocationOptions(string dumpPath, bool headless)
    {
        var outDir = OutputDirBox.Text.Trim();
        var lang = string.IsNullOrWhiteSpace(_startupOptions.Language)
            ? (_isKorean ? "ko" : "en")
            : _startupOptions.Language;

        return new DumpToolInvocationOptions
        {
            DumpPath = dumpPath,
            OutDir = string.IsNullOrWhiteSpace(outDir) ? null : outDir,
            Language = lang,
            Headless = headless,
            Debug = _startupOptions.Debug,
            AllowOnlineSymbols = _startupOptions.AllowOnlineSymbols,
            ForceAdvancedUi = _startupOptions.ForceAdvancedUi,
            ForceSimpleUi = _startupOptions.ForceSimpleUi,
        };
    }

    private void NavView_SelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (args.SelectedItem is NavigationViewItem item && item.Tag is string tag)
        {
            ApplyNavigationSelectionEmphasis(item);
            AnalyzePanel.Visibility = tag == "analyze" ? Visibility.Visible : Visibility.Collapsed;
            SummaryPanel.Visibility = tag == "summary" ? Visibility.Visible : Visibility.Collapsed;
            EvidencePanel.Visibility = tag == "evidence" ? Visibility.Visible : Visibility.Collapsed;
            EventsPanel.Visibility = tag == "events" ? Visibility.Visible : Visibility.Collapsed;
            ReportPanel.Visibility = tag == "report" ? Visibility.Visible : Visibility.Collapsed;
        }
    }

    private void ApplyNavigationSelectionEmphasis(NavigationViewItem selectedItem)
    {
        SetNavItemVisual(NavAnalyze, selectedItem == NavAnalyze);
        SetNavItemVisual(NavSummary, selectedItem == NavSummary);
        SetNavItemVisual(NavEvidence, selectedItem == NavEvidence);
        SetNavItemVisual(NavEvents, selectedItem == NavEvents);
        SetNavItemVisual(NavReport, selectedItem == NavReport);
    }

    private static void SetNavItemVisual(NavigationViewItem item, bool isSelected)
    {
        if (isSelected)
        {
            item.Background = new SolidColorBrush(ColorHelper.FromArgb(0x85, 0x24, 0x32, 0x43));
            item.Foreground = new SolidColorBrush(ColorHelper.FromArgb(0xFF, 0xF5, 0xE8, 0xCC));
            item.BorderBrush = new SolidColorBrush(ColorHelper.FromArgb(0xC6, 0x9D, 0x7A, 0x44));
            item.BorderThickness = new Thickness(1);
            return;
        }

        item.Background = new SolidColorBrush(ColorHelper.FromArgb(0x00, 0x00, 0x00, 0x00));
        item.Foreground = new SolidColorBrush(ColorHelper.FromArgb(0xFF, 0xD2, 0xBC, 0x91));
        item.BorderBrush = null;
        item.BorderThickness = new Thickness(0);
    }

    private void RootGrid_SizeChanged(object sender, SizeChangedEventArgs e)
    {
        ApplyAdaptiveLayout();
    }

    private void ApplyAdaptiveLayout()
    {
        var width = RootGrid.ActualWidth;
        var height = RootGrid.ActualHeight;
        var compact = width < 1550 || height < 900;

        if (compact == _isCompactLayout)
        {
            return;
        }

        _isCompactLayout = compact;

        NavView.OpenPaneLength = compact ? 206 : 228;
        RootContentGrid.MaxWidth = compact ? 1140 : 1240;
        RootContentGrid.MinWidth = compact ? 920 : 1060;
        RootContentGrid.Padding = compact ? new Thickness(14, 12, 14, 12) : new Thickness(22, 18, 22, 18);

        AnalyzePanel.Spacing = compact ? 12 : 18;
        SummaryPanel.Spacing = compact ? 12 : 16;
        EvidencePanel.Spacing = compact ? 12 : 16;
        EventsPanel.Spacing = compact ? 12 : 16;
        ReportPanel.Spacing = compact ? 12 : 16;

        HeaderTitleText.FontSize = compact ? 42 : 50;
        HeaderSubtitleText.FontSize = compact ? 14 : 15;
        AnalyzeSectionTitleText.FontSize = compact ? 22 : 26;
        WorkflowSectionTitleText.FontSize = compact ? 24 : 28;
        SnapshotSectionTitleText.FontSize = compact ? 26 : 30;
        AdvancedSectionTitleText.FontSize = compact ? 26 : 30;
        EventsLabelText.FontSize = compact ? 26 : 30;

        QuickPrimaryValueText.FontSize = compact ? 16 : 18;
        QuickConfidenceValueText.FontSize = compact ? 16 : 18;
        QuickActionsValueText.FontSize = compact ? 16 : 18;
        QuickEventsValueText.FontSize = compact ? 16 : 18;

        StepOneDescText.MaxLines = compact ? 1 : 2;
        StepTwoDescText.MaxLines = compact ? 1 : 2;
        StepThreeDescText.MaxLines = compact ? 1 : 2;

        SuspectsList.MaxHeight = compact ? 240 : 320;
        RecommendationsList.MaxHeight = compact ? 240 : 320;
        CallstackList.MaxHeight = compact ? 320 : 500;
        EvidenceList.MaxHeight = compact ? 320 : 500;
        ResourcesList.MaxHeight = compact ? 320 : 500;
        EventsList.MaxHeight = compact ? 380 : 600;

        WctTextBox.MinHeight = compact ? 160 : 200;
        WctTextBox.MaxHeight = compact ? 300 : 400;
        ReportTextBox.MinHeight = compact ? 180 : 200;
        ReportTextBox.MaxHeight = compact ? 360 : 500;
    }

    private void SetBusy(bool isBusy, string message)
    {
        BusyRing.IsActive = isBusy;
        AnalyzeButton.IsEnabled = !isBusy;
        OpenOutputButton.IsEnabled = !isBusy;
        StatusText.Text = message;
    }

    private string T(string en, string ko)
    {
        return _isKorean ? ko : en;
    }
}
