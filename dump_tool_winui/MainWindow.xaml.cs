using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Globalization;
using System.Linq;
using System.Text.Json;

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
    private enum LayoutTier { Wide, Compact, Narrow }
    private LayoutTier _currentLayoutTier = (LayoutTier)(-1); // force first apply
    private CancellationTokenSource? _analysisCts;

    private string? _currentDumpPath;
    private string? _currentOutDir;
    private AnalysisSummary? _currentSummary;

    private sealed class AdvancedArtifactsData
    {
        public List<string> EventLines { get; } = new();
        public int EventCount { get; set; }
        public string ReportText { get; set; } = string.Empty;
        public string WctText { get; set; } = string.Empty;
    }

    internal MainWindow(DumpToolInvocationOptions startupOptions, string? startupWarning)
    {
        _startupOptions = startupOptions;
        _isKorean = string.Equals(_startupOptions.Language, "ko", StringComparison.OrdinalIgnoreCase) ||
                    (string.IsNullOrWhiteSpace(_startupOptions.Language) &&
                     string.Equals(CultureInfo.CurrentUICulture.TwoLetterISOLanguageName, "ko", StringComparison.OrdinalIgnoreCase));

        InitializeComponent();

        SystemBackdrop = new MicaBackdrop();

        ApplyLocalizedStaticText();
        HookWheelChainingForNestedControls();
        RootGrid.SizeChanged += RootGrid_SizeChanged;

        SuspectsList.ItemsSource = _suspects;
        RecommendationsList.ItemsSource = _recommendations;
        CallstackList.ItemsSource = _callstackFrames;
        EvidenceList.ItemsSource = _evidenceItems;
        ResourcesList.ItemsSource = _resourceItems;
        EventsList.ItemsSource = _eventItems;

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

        var options = BuildCurrentInvocationOptions(dumpPath, false);
        var outDir = NativeAnalyzerBridge.ResolveOutputDirectory(dumpPath, options.OutDir);
        _currentDumpPath = dumpPath;
        _currentOutDir = outDir;

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
        _currentSummary = summary;

        SummarySentenceText.Text = string.IsNullOrWhiteSpace(summary.SummarySentence)
            ? T("No summary sentence produced.", "요약 문장이 생성되지 않았습니다.")
            : summary.SummarySentence;

        BucketText.Text = string.IsNullOrWhiteSpace(summary.CrashBucketKey)
            ? T("Crash bucket: unavailable", "크래시 버킷: 없음")
            : T("Crash bucket: ", "크래시 버킷: ") + summary.CrashBucketKey;

        if (summary.HistoryCorrelationCount > 1)
        {
            CorrelationBadge.Text = _isKorean
                ? $"\u26a0 동일 패턴 {summary.HistoryCorrelationCount}회 반복 발생"
                : $"\u26a0 Same pattern repeated {summary.HistoryCorrelationCount} times";
            CorrelationBadge.Visibility = Microsoft.UI.Xaml.Visibility.Visible;
        }
        else
        {
            CorrelationBadge.Visibility = Microsoft.UI.Xaml.Visibility.Collapsed;
        }

        ModuleText.Text = string.IsNullOrWhiteSpace(summary.ModulePlusOffset)
            ? T("Fault module: unavailable", "오류 모듈: 없음")
            : T("Fault module: ", "오류 모듈: ") + summary.ModulePlusOffset;

        if (summary.CrashLoggerRefs.Count > 0 && !string.IsNullOrWhiteSpace(summary.InferredModName))
            ModNameText.Text = T("Referenced mod: ", "참조 모드: ") + summary.InferredModName;
        else
            ModNameText.Text = string.IsNullOrWhiteSpace(summary.InferredModName)
                ? T("Inferred mod: unavailable", "추정 모드: 없음")
                : T("Inferred mod: ", "추정 모드: ") + summary.InferredModName;

        CopySummaryButton.IsEnabled = true;
        CopyShareButton.IsEnabled = true;

        _suspects.Clear();

        // CrashLogger ESP refs → SuspectItem (상위 3개)
        foreach (var espRef in summary.CrashLoggerRefs.Take(3))
        {
            _suspects.Add(new SuspectItem(
                MapRelevanceToConfidence(espRef.RelevanceScore),
                espRef.EspName,
                BuildEspRefReason(espRef)));
        }

        // DLL suspects (ESP와 합쳐 최대 7개)
        var dllSlots = Math.Max(0, 7 - _suspects.Count);
        foreach (var suspect in summary.Suspects.Take(dllSlots))
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

        if (summary.CrashLoggerRefs.Count > 0)
            QuickPrimaryLabelText.Text = T("Referenced mod (ESP)", "참조 모드 (ESP)");
        else
            QuickPrimaryLabelText.Text = T("Primary suspect", "주요 원인");

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

        if (summary.TroubleshootingSteps.Count > 0)
        {
            TroubleshootingExpander.Header = string.IsNullOrWhiteSpace(summary.TroubleshootingTitle)
                ? T("Troubleshooting", "트러블슈팅 가이드")
                : summary.TroubleshootingTitle;
            TroubleshootingCard.Visibility = Microsoft.UI.Xaml.Visibility.Visible;
            var numberedSteps = summary.TroubleshootingSteps
                .Select((step, i) => $"{i + 1}. {step}")
                .ToList();
            TroubleshootingList.ItemsSource = numberedSteps;
        }
        else
        {
            TroubleshootingCard.Visibility = Microsoft.UI.Xaml.Visibility.Collapsed;
        }

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

    private void CopyShareButton_Click(object sender, RoutedEventArgs e)
    {
        var text = BuildCommunityShareText();
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

        if (summary.CrashLoggerRefs.Count > 0)
        {
            var espNames = string.Join(", ", summary.CrashLoggerRefs.Select(r => r.EspName));
            lines.Add((_isKorean ? "CrashLogger 참조 모드: " : "CrashLogger referenced mods: ") + espNames);
        }

        return string.Join(Environment.NewLine, lines);
    }

    private string? BuildCommunityShareText()
    {
        var summary = _currentSummary;
        if (summary is null)
        {
            return null;
        }

        var lines = new List<string>();

        static bool HasAnyPrefix(IEnumerable<string> values, params string[] prefixes)
        {
            foreach (var v in values)
            {
                if (string.IsNullOrWhiteSpace(v))
                {
                    continue;
                }

                foreach (var p in prefixes)
                {
                    if (v.StartsWith(p, StringComparison.OrdinalIgnoreCase))
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        var recs = summary.Recommendations;
        var looksSnapshotByText = summary.SummarySentence.Contains("snapshot", StringComparison.OrdinalIgnoreCase) ||
                                  summary.SummarySentence.Contains("스냅샷", StringComparison.Ordinal);
        var looksHangByText = summary.SummarySentence.Contains("freeze", StringComparison.OrdinalIgnoreCase) ||
                              summary.SummarySentence.Contains("infinite loading", StringComparison.OrdinalIgnoreCase) ||
                              summary.SummarySentence.Contains("프리징", StringComparison.Ordinal) ||
                              summary.SummarySentence.Contains("무한로딩", StringComparison.Ordinal);

        var isSnapshotLike = summary.IsSnapshotLike ||
                             looksSnapshotByText ||
                             HasAnyPrefix(recs, "[Snapshot]", "[정상/스냅샷]", "[Manual]", "[수동]");
        var isHangLike = !isSnapshotLike &&
                         (summary.IsHangLike || looksHangByText || HasAnyPrefix(recs, "[Hang]", "[프리징]"));
        var isCrashLike = !isSnapshotLike && !isHangLike && summary.IsCrashLike;

        lines.Add(isSnapshotLike
            ? (_isKorean ? "🟡 Skyrim 상태 스냅샷 리포트 — SkyrimDiag" : "🟡 Skyrim Snapshot Report — SkyrimDiag")
            : isHangLike
                ? (_isKorean ? "🟠 Skyrim 프리징/무한로딩 리포트 — SkyrimDiag" : "🟠 Skyrim Freeze/ILS Report — SkyrimDiag")
                : (_isKorean ? "🔴 Skyrim CTD 리포트 — SkyrimDiag" : "🔴 Skyrim CTD Report — SkyrimDiag"));

        if (summary.CrashLoggerRefs.Count > 0)
        {
            lines.Add($"📌 {(_isKorean ? "참조 모드 (ESP)" : "Referenced mod (ESP)")}: {summary.CrashLoggerRefs[0].EspName}");
            if (summary.Suspects.Count > 0)
            {
                var topSuspect = summary.Suspects[0];
                var conf = !string.IsNullOrWhiteSpace(topSuspect.Confidence) ? topSuspect.Confidence : "?";
                lines.Add($"🔧 {(_isKorean ? "DLL 후보" : "DLL suspect")}: {topSuspect.Module} ({conf})");
            }
        }
        else if (summary.Suspects.Count > 0)
        {
            var top = summary.Suspects[0];
            var conf = !string.IsNullOrWhiteSpace(top.Confidence) ? top.Confidence : "?";
            lines.Add($"📌 {(_isKorean ? (isSnapshotLike ? "참고 후보" : "유력 원인") : (isSnapshotLike ? "Reference candidate" : "Primary suspect"))}: {top.Module} ({conf})");
        }

        if (!string.IsNullOrWhiteSpace(summary.CrashBucketKey))
        {
            var typeLabel = isSnapshotLike
                ? (_isKorean ? "분류" : "Category")
                : (_isKorean ? "유형" : "Type");
            var typeValue = isSnapshotLike
                ? (_isKorean ? "SNAPSHOT" : "SNAPSHOT")
                : isHangLike
                    ? (_isKorean ? "HANG" : "HANG")
                    : summary.CrashBucketKey;
            lines.Add($"🔍 {typeLabel}: {typeValue}");
        }

        if (!string.IsNullOrWhiteSpace(summary.ModulePlusOffset))
        {
            lines.Add($"📍 Module+Offset: {summary.ModulePlusOffset}");
        }

        if (!string.IsNullOrWhiteSpace(summary.SummarySentence))
        {
            lines.Add($"💡 {(_isKorean ? "결론" : "Conclusion")}: {summary.SummarySentence}");
        }

        if (summary.Recommendations.Count > 0)
        {
            string firstAction;
            if (isSnapshotLike)
            {
                firstAction = summary.Recommendations.FirstOrDefault(r =>
                    r.StartsWith("[Snapshot]", StringComparison.OrdinalIgnoreCase) ||
                    r.StartsWith("[정상/스냅샷]", StringComparison.Ordinal) ||
                    r.StartsWith("[Manual]", StringComparison.OrdinalIgnoreCase) ||
                    r.StartsWith("[수동]", StringComparison.Ordinal)) ?? summary.Recommendations[0];
            }
            else if (isHangLike)
            {
                firstAction = summary.Recommendations.FirstOrDefault(r =>
                    r.StartsWith("[Hang]", StringComparison.OrdinalIgnoreCase) ||
                    r.StartsWith("[프리징]", StringComparison.Ordinal)) ?? summary.Recommendations[0];
            }
            else
            {
                firstAction = summary.Recommendations[0];
            }
            lines.Add($"🛠️ {(_isKorean ? "권장" : "Action")}: {firstAction}");
        }

        lines.Add("— Tullius CTD Logger");

        return string.Join(Environment.NewLine, lines);
    }

    private async Task RenderAdvancedArtifactsAsync(string dumpPath, string outDir, CancellationToken cancellationToken)
    {
        var artifacts = await Task.Run(
            () => LoadAdvancedArtifactsCore(
                dumpPath,
                outDir,
                T("Report file not found.", "리포트 파일이 없습니다."),
                T("WCT file not found for this dump.", "이 덤프에 대한 WCT 파일이 없습니다."),
                cancellationToken),
            cancellationToken);
        cancellationToken.ThrowIfCancellationRequested();

        _eventItems.Clear();
        foreach (var line in artifacts.EventLines)
        {
            _eventItems.Add(line);
        }

        var eventCount = artifacts.EventCount;
        if (_eventItems.Count == 0)
        {
            _eventItems.Add(T("No blackbox events were found.", "블랙박스 이벤트를 찾지 못했습니다."));
        }
        QuickEventsValueText.Text = eventCount == 0
            ? T("0 events", "0개")
            : $"{eventCount} {T("events", "개")}";

        ReportTextBox.Text = artifacts.ReportText;
        WctTextBox.Text = artifacts.WctText;
    }

    private static AdvancedArtifactsData LoadAdvancedArtifactsCore(
        string dumpPath,
        string outDir,
        string missingReportText,
        string missingWctText,
        CancellationToken cancellationToken)
    {
        var data = new AdvancedArtifactsData();

        var blackboxPath = NativeAnalyzerBridge.ResolveBlackboxPath(dumpPath, outDir);
        if (File.Exists(blackboxPath))
        {
            var tail = new Queue<string>(capacity: 200);
            foreach (var line in File.ReadLines(blackboxPath))
            {
                cancellationToken.ThrowIfCancellationRequested();
                if (string.IsNullOrWhiteSpace(line))
                {
                    continue;
                }

                if (tail.Count == 200)
                {
                    tail.Dequeue();
                }

                try
                {
                    using var jDoc = JsonDocument.Parse(line);
                    var root = jDoc.RootElement;
                    var idx = root.GetProperty("i").GetInt32();
                    var tMs = root.GetProperty("t_ms").GetDouble();
                    var tid = root.GetProperty("tid").GetUInt32();
                    var typeName = root.GetProperty("type_name").GetString() ?? "?";
                    var detail = root.TryGetProperty("detail", out var detProp) ? detProp.GetString() : null;

                    string formatted;
                    if (!string.IsNullOrEmpty(detail))
                    {
                        formatted = $"[{idx}] t={tMs:F0}ms tid={tid} {typeName} | {detail}";
                    }
                    else
                    {
                        var a = root.GetProperty("a").GetUInt64();
                        var b = root.GetProperty("b").GetUInt64();
                        formatted = $"[{idx}] t={tMs:F0}ms tid={tid} {typeName} a={a} b={b}";
                    }
                    tail.Enqueue(formatted);
                }
                catch (Exception ex)
                {
                    Debug.WriteLine($"Blackbox line parse failed: {ex.GetType().Name}: {ex.Message}");
                    tail.Enqueue(line);  // fallback: raw line
                }
            }
            data.EventLines.AddRange(tail);
            data.EventCount = data.EventLines.Count;
        }

        var reportPath = NativeAnalyzerBridge.ResolveReportPath(dumpPath, outDir);
        data.ReportText = File.Exists(reportPath)
            ? File.ReadAllText(reportPath)
            : missingReportText;

        var wctPath = NativeAnalyzerBridge.ResolveWctPath(dumpPath, outDir);
        if (File.Exists(wctPath))
        {
            var raw = File.ReadAllText(wctPath);
            try
            {
                using var doc = JsonDocument.Parse(raw);
                data.WctText = JsonSerializer.Serialize(doc.RootElement, new JsonSerializerOptions
                {
                    WriteIndented = true,
                });
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"WCT JSON pretty-print failed: {ex.GetType().Name}: {ex.Message}");
                data.WctText = raw;
            }
        }
        else
        {
            data.WctText = missingWctText;
        }
        return data;
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

    private string BuildEspRefReason(CrashLoggerRefItem espRef)
    {
        var parts = new List<string>();
        if (!string.IsNullOrWhiteSpace(espRef.ObjectType))
            parts.Add(espRef.ObjectType);
        if (!string.IsNullOrWhiteSpace(espRef.ObjectName))
            parts.Add($"\"{espRef.ObjectName}\"");
        if (!string.IsNullOrWhiteSpace(espRef.Location))
            parts.Add(_isKorean ? $"{espRef.Location}에서 발견" : $"found in {espRef.Location}");
        if (espRef.RefCount > 1)
            parts.Add(_isKorean ? $"참조 {espRef.RefCount}건" : $"{espRef.RefCount} refs");
        return parts.Count == 0
            ? T("ESP/ESM object reference", "ESP/ESM 오브젝트 참조")
            : string.Join(" — ", parts);
    }

    private string MapRelevanceToConfidence(int score)
    {
        if (score >= 16) return T("ESP ref (high)", "ESP 참조 (높음)");
        if (score >= 10) return T("ESP ref", "ESP 참조");
        return T("ESP ref (low)", "ESP 참조 (낮음)");
    }

    private string T(string en, string ko)
    {
        return _isKorean ? ko : en;
    }
}
