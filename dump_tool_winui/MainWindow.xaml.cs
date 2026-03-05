using System.Diagnostics;
using System.Globalization;

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
    private readonly MainWindowViewModel _vm;
    private readonly bool _isKorean;
    private enum LayoutTier { Wide, Compact, Narrow }
    private LayoutTier _currentLayoutTier = (LayoutTier)(-1); // force first apply
    private CancellationTokenSource? _analysisCts;

    internal MainWindow(DumpToolInvocationOptions startupOptions, string? startupWarning)
    {
        _startupOptions = startupOptions;
        _isKorean = string.Equals(_startupOptions.Language, "ko", StringComparison.OrdinalIgnoreCase) ||
                    (string.IsNullOrWhiteSpace(_startupOptions.Language) &&
                     string.Equals(CultureInfo.CurrentUICulture.TwoLetterISOLanguageName, "ko", StringComparison.OrdinalIgnoreCase));
        _vm = new MainWindowViewModel(_isKorean);

        InitializeComponent();

        SystemBackdrop = new MicaBackdrop();

        ApplyLocalizedStaticText();
        HookWheelChainingForNestedControls();
        RootGrid.SizeChanged += RootGrid_SizeChanged;

        SuspectsList.ItemsSource = _vm.Suspects;
        RecommendationsList.ItemsSource = _vm.Recommendations;
        CallstackList.ItemsSource = _vm.CallstackFrames;
        EvidenceList.ItemsSource = _vm.EvidenceItems;
        ResourcesList.ItemsSource = _vm.ResourceItems;
        EventsList.ItemsSource = _vm.EventItems;

        CopySummaryButton.IsEnabled = false;
        CopyShareButton.IsEnabled = false;

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
            DispatcherQueue.TryEnqueue(async () => await AnalyzeAsync(preferExistingArtifacts: true));
        }

        ApplyAdaptiveLayout();
    }

    private void ApplyLocalizedStaticText()
    {
        Title = T("Tullius CTD Logger", "툴리우스 CTD 로거");

        // Navigation items
        NavAnalyze.Content = T("Dashboard", "대시보드");
        NavTriage.Content = T("Triage", "분석 결과");
        NavRawData.Content = T("Raw Data", "원시 데이터");

        HeaderSubtitleText.Text = T(
            "Skyrim SE detected. Ready for dump triage.",
            "Skyrim SE가 감지되었습니다. 덤프 원인 분석을 시작할 수 있습니다.");
        HeaderBadgeText.Text = T("STATUS READY", "상태 준비됨");

        AnalyzeSectionTitleText.Text = T("Dump Intake", "덤프 입력");

        SnapshotSectionTitleText.Text = T("Crash Summary", "크래시 요약");
        NextStepsSectionTitleText.Text = T("Recommended Next Steps", "권장 다음 단계");
        SuspectsSectionTitleText.Text = T("Top Cause Candidates", "주요 원인 후보");
        QuickPrimaryLabelText.Text = T("Primary suspect", "주요 원인");
        QuickConfidenceLabelText.Text = T("Confidence", "신뢰도");
        QuickActionsLabelText.Text = T("Next actions", "권장 조치");
        QuickEventsLabelText.Text = T("Blackbox events", "블랙박스 이벤트");
        QuickPrimaryValueText.Text = "-";
        QuickConfidenceValueText.Text = "-";
        QuickActionsValueText.Text = "-";
        QuickEventsValueText.Text = "-";

        SummarySentenceText.Text = T("No analysis yet.", "아직 분석 결과가 없습니다.");
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
        CancelAnalyzeButton.Content = T("Cancel analysis", "분석 취소");
        OpenOutputButton.Content = T("Open report folder", "리포트 폴더 열기");
        CopySummaryButton.Content = T("Copy summary", "요약 복사");
        CopyShareButton.Content = T("Share", "공유");
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

        var delta = e.GetCurrentPoint(RootScrollViewer).Properties.MouseWheelDelta;
        if (delta == 0)
        {
            return;
        }

        // Find the innermost ScrollViewer (e.g. inside a ListView) that is not
        // the root.  If it still has room to scroll in the wheel direction, let
        // it consume the event exclusively — do NOT chain to the root.
        var inner = FindInnerScrollViewer(e.OriginalSource as DependencyObject);
        if (inner != null && inner.ScrollableHeight > 0)
        {
            bool atEdge = (delta > 0 && inner.VerticalOffset <= 0)
                       || (delta < 0 && inner.VerticalOffset >= inner.ScrollableHeight - 0.5);
            if (!atEdge)
            {
                return;
            }
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

    private ScrollViewer? FindInnerScrollViewer(DependencyObject? source)
    {
        for (var current = source; current is not null; current = VisualTreeHelper.GetParent(current))
        {
            if (current is ScrollViewer sv && sv != RootScrollViewer)
            {
                return sv;
            }

            if (current == RootScrollViewer || current == RootGrid)
            {
                break;
            }
        }

        return null;
    }

    private async void AnalyzeButton_Click(object sender, RoutedEventArgs e)
    {
        await AnalyzeAsync(preferExistingArtifacts: false);
    }

    private void CancelAnalyzeButton_Click(object sender, RoutedEventArgs e)
    {
        _analysisCts?.Cancel();
    }

    private async Task<bool> TryLoadExistingAnalysisAsync(string dumpPath, string outDir, CancellationToken cancellationToken)
    {
        var summaryPath = NativeAnalyzerBridge.ResolveSummaryPath(dumpPath, outDir);

        // Helper may launch the viewer immediately after headless analysis, so the summary
        // can appear shortly after startup. Wait a bit to avoid duplicate analysis work.
        for (var i = 0; i < 15 && !File.Exists(summaryPath); i++)
        {
            cancellationToken.ThrowIfCancellationRequested();
            await Task.Delay(100, cancellationToken);
        }

        if (!File.Exists(summaryPath))
        {
            return false;
        }

        for (var attempt = 0; attempt < 5; attempt++)
        {
            try
            {
                cancellationToken.ThrowIfCancellationRequested();
                var summary = AnalysisSummary.LoadFromSummaryFile(summaryPath);
                RenderSummary(summary);
                await RenderAdvancedArtifactsAsync(dumpPath, outDir, cancellationToken);
                SetBusy(false, T(
                    "Loaded existing analysis artifacts. Click Analyze to refresh.",
                    "기존 분석 결과를 불러왔습니다. 다시 분석하려면 \"지금 분석\"을 누르세요."));
                NavView.SelectedItem = NavTriage;
                return true;
            }
            catch (OperationCanceledException)
            {
                throw;
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"TryLoadExistingAnalysisAsync attempt {attempt}: {ex.GetType().Name}: {ex.Message}");
                await Task.Delay(100, cancellationToken);
            }
        }

        return false;
    }

    private async Task AnalyzeAsync(bool preferExistingArtifacts)
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

        _analysisCts?.Cancel();
        _analysisCts?.Dispose();
        using var analysisCts = new CancellationTokenSource();
        _analysisCts = analysisCts;
        var cancellationToken = analysisCts.Token;

        var options = _vm.BuildInvocationOptions(
            dumpPath, OutputDirBox.Text.Trim(), _startupOptions.Language, false, _startupOptions);
        var outDir = NativeAnalyzerBridge.ResolveOutputDirectory(dumpPath, options.OutDir);
        _vm.CurrentDumpPath = dumpPath;
        _vm.CurrentOutDir = outDir;

        try
        {
            if (preferExistingArtifacts)
            {
                SetBusy(true, T(
                    "Checking for existing analysis artifacts...",
                    "기존 분석 결과를 확인 중입니다..."));
                if (await TryLoadExistingAnalysisAsync(dumpPath, outDir, cancellationToken))
                {
                    return;
                }
            }

            SetBusy(true, T("Analyzing dump with native engine...", "네이티브 엔진으로 덤프를 분석 중입니다..."));

            var (exitCode, nativeErr) = await NativeAnalyzerBridge.RunAnalyzeAsync(options, cancellationToken);
            cancellationToken.ThrowIfCancellationRequested();
            if (exitCode == NativeAnalyzerBridge.UserCanceledExitCode)
            {
                SetBusy(false, T("Analysis canceled.", "분석이 취소되었습니다."));
                return;
            }

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

            var summary = AnalysisSummary.LoadFromSummaryFile(summaryPath);
            RenderSummary(summary);
            await RenderAdvancedArtifactsAsync(dumpPath, outDir, cancellationToken);
            SetBusy(false, T("Analysis complete. Review the candidates and checklist.", "분석 완료. 원인 후보와 체크리스트를 확인하세요."));
            NavView.SelectedItem = NavTriage;
        }
        catch (OperationCanceledException)
        {
            SetBusy(false, T("Analysis canceled.", "분석이 취소되었습니다."));
        }
        catch (Exception ex)
        {
            SetBusy(false, T("Failed to read summary JSON: ", "요약 JSON을 읽지 못했습니다: ") + ex.Message);
        }
        finally
        {
            if (ReferenceEquals(_analysisCts, analysisCts))
            {
                _analysisCts = null;
            }
        }
    }

    private void RenderSummary(AnalysisSummary summary)
    {
        _vm.PopulateSummary(summary);

        SummarySentenceText.Text = _vm.SummarySentence;
        BucketText.Text = _vm.BucketText;

        if (_vm.ShowCorrelationBadge)
        {
            CorrelationBadge.Text = _vm.CorrelationBadgeText;
            CorrelationBadge.Visibility = Visibility.Visible;
        }
        else
        {
            CorrelationBadge.Visibility = Visibility.Collapsed;
        }

        ModuleText.Text = _vm.ModuleText;
        ModNameText.Text = _vm.ModNameText;

        CopySummaryButton.IsEnabled = true;
        CopyShareButton.IsEnabled = true;

        QuickPrimaryValueText.Text = _vm.QuickPrimaryValue;
        QuickConfidenceValueText.Text = _vm.QuickConfidenceValue;
        QuickPrimaryLabelText.Text = _vm.QuickPrimaryLabel;
        QuickActionsValueText.Text = _vm.QuickActionsValue;

        if (_vm.ShowTroubleshooting)
        {
            TroubleshootingExpander.Header = _vm.TroubleshootingTitle;
            TroubleshootingCard.Visibility = Visibility.Visible;
            TroubleshootingList.ItemsSource = _vm.TroubleshootingSteps;
        }
        else
        {
            TroubleshootingCard.Visibility = Visibility.Collapsed;
        }
    }

    private void CopySummaryButton_Click(object sender, RoutedEventArgs e)
    {
        var text = _vm.BuildSummaryClipboardText();
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

    private void CopyShareButton_Click(object sender, RoutedEventArgs e)
    {
        var text = _vm.BuildCommunityShareText();
        if (string.IsNullOrWhiteSpace(text))
        {
            StatusText.Text = T("No summary to share yet.", "아직 공유할 요약이 없습니다.");
            return;
        }

        try
        {
            var dataPackage = new DataPackage();
            dataPackage.SetText(text);
            Clipboard.SetContent(dataPackage);
            Clipboard.Flush();
            StatusText.Text = T("Copied community share text to clipboard.", "커뮤니티 공유용 요약을 클립보드에 복사했습니다.");
        }
        catch (Exception ex)
        {
            StatusText.Text = T("Failed to copy to clipboard: ", "클립보드 복사 실패: ") + ex.Message;
        }
    }

    private async Task RenderAdvancedArtifactsAsync(string dumpPath, string outDir, CancellationToken cancellationToken)
    {
        var artifacts = await Task.Run(
            () => MainWindowViewModel.LoadAdvancedArtifacts(
                dumpPath,
                outDir,
                T("Report file not found.", "리포트 파일이 없습니다."),
                T("WCT file not found for this dump.", "이 덤프에 대한 WCT 파일이 없습니다."),
                cancellationToken),
            cancellationToken);
        cancellationToken.ThrowIfCancellationRequested();

        _vm.PopulateAdvancedArtifacts(artifacts);
        QuickEventsValueText.Text = _vm.QuickEventsValue;
        ReportTextBox.Text = artifacts.ReportText;
        WctTextBox.Text = artifacts.WctText;
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
        var outDir = _vm.CurrentOutDir;
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

    private void NavView_SelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (args.SelectedItem is NavigationViewItem item && item.Tag is string tag)
        {
            AnalyzePanel.Visibility = tag == "analyze" ? Visibility.Visible : Visibility.Collapsed;
            TriagePanel.Visibility  = tag == "triage"  ? Visibility.Visible : Visibility.Collapsed;
            RawDataPanel.Visibility = tag == "rawdata" ? Visibility.Visible : Visibility.Collapsed;
        }
    }

    private void RootGrid_SizeChanged(object sender, SizeChangedEventArgs e)
    {
        ApplyAdaptiveLayout();
    }

    private void ApplyAdaptiveLayout()
    {
        var width = RootGrid.ActualWidth;
        var height = RootGrid.ActualHeight;

        LayoutTier tier;
        if (width < 1100)
            tier = LayoutTier.Narrow;
        else if (width < 1550 || height < 900)
            tier = LayoutTier.Compact;
        else
            tier = LayoutTier.Wide;

        if (tier == _currentLayoutTier)
        {
            return;
        }

        _currentLayoutTier = tier;
        var compact = tier != LayoutTier.Wide;
        var narrow = tier == LayoutTier.Narrow;

        NavView.OpenPaneLength = narrow ? 52 : (compact ? 206 : 228);
        NavView.IsPaneOpen = !narrow;
        NavView.IsPaneToggleButtonVisible = narrow;

        RootContentGrid.MaxWidth = narrow ? 960 : (compact ? 1140 : 1240);
        RootContentGrid.MinWidth = narrow ? 480 : (compact ? 640 : 1060);
        RootContentGrid.Padding = narrow
            ? new Thickness(10, 8, 10, 8)
            : compact ? new Thickness(14, 12, 14, 12) : new Thickness(22, 18, 22, 18);

        AnalyzePanel.Spacing = compact ? 12 : 16;
        TriagePanel.Spacing = compact ? 12 : 16;
        RawDataPanel.Spacing = compact ? 12 : 16;

        AnalyzeSectionTitleText.FontSize = compact ? 22 : 24;
        SnapshotSectionTitleText.FontSize = narrow ? 22 : (compact ? 26 : 30);

        // Triage 2-column → 1-column adaptive
        if (narrow)
        {
            // Single column: sidebar stacks below main
            TriageTwoColumnGrid.ColumnDefinitions[0].Width = new GridLength(1, GridUnitType.Star);
            TriageTwoColumnGrid.ColumnDefinitions[1].Width = new GridLength(0);

            if (TriageTwoColumnGrid.RowDefinitions.Count < 2)
            {
                TriageTwoColumnGrid.RowDefinitions.Clear();
                TriageTwoColumnGrid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
                TriageTwoColumnGrid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            }
            Grid.SetRow(TriageSidebar, 1);
            Grid.SetColumn(TriageSidebar, 0);
            MainColumnDivider.BorderThickness = new Thickness(0);
        }
        else
        {
            // Two columns
            TriageTwoColumnGrid.RowDefinitions.Clear();
            Grid.SetRow(TriageSidebar, 0);
            Grid.SetColumn(TriageSidebar, 1);
            MainColumnDivider.BorderThickness = new Thickness(0, 0, 1, 0);

            TriageTwoColumnGrid.ColumnDefinitions[0].Width = compact
                ? new GridLength(5, GridUnitType.Star)
                : new GridLength(3, GridUnitType.Star);
            TriageTwoColumnGrid.ColumnDefinitions[1].Width = compact
                ? new GridLength(3, GridUnitType.Star)
                : new GridLength(2, GridUnitType.Star);
        }

        QuickPrimaryValueText.FontSize = compact ? 16 : 18;
        QuickConfidenceValueText.FontSize = compact ? 16 : 18;
        QuickActionsValueText.FontSize = compact ? 16 : 18;
        QuickEventsValueText.FontSize = compact ? 16 : 18;

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
        CancelAnalyzeButton.IsEnabled = isBusy;
        BrowseDumpButton.IsEnabled = !isBusy;
        BrowseOutputButton.IsEnabled = !isBusy;
        DumpPathBox.IsEnabled = !isBusy;
        OutputDirBox.IsEnabled = !isBusy;
        OpenOutputButton.IsEnabled = !isBusy;
        StatusText.Text = message;
    }

    private string T(string en, string ko) => _vm.T(en, ko);
}
