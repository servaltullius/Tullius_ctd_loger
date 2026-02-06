using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Text.Json;

using Microsoft.UI.Xaml;

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

    private string? _currentDumpPath;
    private string? _currentOutDir;

    internal MainWindow(DumpToolInvocationOptions startupOptions, string? startupWarning)
    {
        _startupOptions = startupOptions;
        _isKorean = string.Equals(_startupOptions.Language, "ko", StringComparison.OrdinalIgnoreCase);

        InitializeComponent();

        SuspectsList.ItemsSource = _suspects;
        RecommendationsList.ItemsSource = _recommendations;
        CallstackList.ItemsSource = _callstackFrames;
        EvidenceList.ItemsSource = _evidenceItems;
        ResourcesList.ItemsSource = _resourceItems;
        EventsList.ItemsSource = _eventItems;

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
        }
        catch (Exception ex)
        {
            SetBusy(false, T("Failed to read summary JSON: ", "요약 JSON을 읽지 못했습니다: ") + ex.Message);
        }
    }

    private void RenderSummary(AnalysisSummary summary)
    {
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

        _recommendations.Clear();
        foreach (var recommendation in summary.Recommendations.Take(12))
        {
            _recommendations.Add(recommendation);
        }
        if (_recommendations.Count == 0)
        {
            _recommendations.Add(T("No recommendation text was generated.", "권장 조치 문구가 생성되지 않았습니다."));
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
        if (_eventItems.Count == 0)
        {
            _eventItems.Add(T("No blackbox events were found.", "블랙박스 이벤트를 찾지 못했습니다."));
        }

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
            ForceAdvancedUi = _startupOptions.ForceAdvancedUi,
            ForceSimpleUi = _startupOptions.ForceSimpleUi,
        };
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
