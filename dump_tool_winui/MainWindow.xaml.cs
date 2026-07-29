using System.Globalization;

using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace SkyrimDiagDumpToolWinUI;

public sealed partial class MainWindow : Window
{
    private readonly DumpToolInvocationOptions _startupOptions;
    private readonly MainWindowViewModel _vm;
    private DumpDiscoveryState _dumpDiscoveryState = DumpDiscoveryStore.Load();
    private enum LayoutTier { Wide, Compact, Narrow }
    private LayoutTier _currentLayoutTier = (LayoutTier)(-1);
    private CancellationTokenSource? _analysisCts;
    private string? _currentSummaryPath;

    internal MainWindow(DumpToolInvocationOptions startupOptions, string? startupWarning)
    {
        _startupOptions = startupOptions;
        var isKorean = string.Equals(_startupOptions.Language, "ko", StringComparison.OrdinalIgnoreCase) ||
                       (string.IsNullOrWhiteSpace(_startupOptions.Language) &&
                        string.Equals(CultureInfo.CurrentUICulture.TwoLetterISOLanguageName, "ko", StringComparison.OrdinalIgnoreCase));
        _vm = new MainWindowViewModel(isKorean);

        InitializeComponent();

        SystemBackdrop = new MicaBackdrop();

        ApplyLocalizedStaticText();
        HookWheelChainingForNestedControls();
        RootGrid.SizeChanged += RootGrid_SizeChanged;

        SuspectsList.ItemsSource = _vm.Suspects;
        ImmediateRecommendationsList.ItemsSource = _vm.ImmediateRecommendations;
        VerificationRecommendationsList.ItemsSource = _vm.VerificationRecommendations;
        RecaptureRecommendationsList.ItemsSource = _vm.RecaptureRecommendations;
        ConflictCandidatesList.ItemsSource = _vm.ConflictComparisonRows;
        CallstackList.ItemsSource = _vm.CallstackFrames;
        EvidenceList.ItemsSource = _vm.EvidenceItems;
        ResourcesList.ItemsSource = _vm.ResourceItems;
        EventsList.ItemsSource = _vm.EventItems;
        RecentDumpList.ItemsSource = _vm.RecentDumps;
        DumpSearchLocationsList.ItemsSource = _vm.DumpSearchLocations;

        CopySummaryButton.IsEnabled = false;
        CopyShareButton.IsEnabled = false;
        SetTriageEditorEnabled(false);
        SetAnalysisContentVisibility(false);
        SetRawDataContentVisibility(hasReport: false, hasWct: false);
        RecentDumpsEmptyState.Visibility = Visibility.Collapsed;
        RecentDumpList.Visibility = Visibility.Collapsed;
        DumpSearchLocationsPanel.Visibility = Visibility.Collapsed;

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
            StatusText.Text = string.Equals(startupWarning, App.MissingNativeAnalyzerWarning, StringComparison.Ordinal)
                ? T(
                    "SkyrimDiagDumpToolNative.dll was not found next to SkyrimDiagDumpToolWinUI.exe.",
                    "SkyrimDiagDumpToolNative.dll을 SkyrimDiagDumpToolWinUI.exe 옆에서 찾지 못했습니다.")
                : startupWarning;
        }

        DispatcherQueue.TryEnqueue(() =>
        {
            _ = RunUiEventAsync(
                async () =>
                {
                    await RefreshDiscoveredDumpsAsync();
                    if (!string.IsNullOrWhiteSpace(_startupOptions.DumpPath))
                    {
                        await AnalyzeAsync(preferExistingArtifacts: true);
                    }
                },
                "Initial dump loading failed: ",
                "초기 덤프 불러오기 실패: ");
        });

        ApplyAdaptiveLayout();
    }

    private async Task RunUiEventAsync(
        Func<Task> action,
        string englishFailurePrefix,
        string koreanFailurePrefix)
    {
        try
        {
            await action();
        }
        catch (OperationCanceledException)
        {
            StatusText.Text = T("Operation canceled.", "작업이 취소되었습니다.");
        }
        catch (Exception ex)
        {
            try
            {
                StatusText.Text = T(englishFailurePrefix, koreanFailurePrefix) + ex.Message;
            }
            catch (Exception statusEx)
            {
                System.Diagnostics.Debug.WriteLine(
                    $"UI event error reporting failed: {statusEx.GetType().Name}: {statusEx.Message}; " +
                    $"original={ex.GetType().Name}: {ex.Message}");
            }
        }
    }

    private void SetBusy(bool isBusy, string message)
    {
        BusyRing.IsActive = isBusy;
        AnalyzeButton.IsEnabled = !isBusy;
        CancelAnalyzeButton.IsEnabled = isBusy;
        BrowseDumpButton.IsEnabled = !isBusy;
        DirectSelectDumpButton.IsEnabled = !isBusy;
        EmptyStateDirectSelectButton.IsEnabled = !isBusy;
        RescanDumpsButton.IsEnabled = !isBusy;
        ManageDumpFoldersButton.IsEnabled = !isBusy;
        EmptyStateManageFoldersButton.IsEnabled = !isBusy;
        AddDumpSearchLocationButton.IsEnabled = !isBusy;
        DumpSearchLocationsList.IsEnabled = !isBusy;
        RecentDumpList.IsEnabled = !isBusy;
        BrowseOutputButton.IsEnabled = !isBusy;
        DumpPathBox.IsEnabled = !isBusy;
        OutputDirBox.IsEnabled = !isBusy;
        OpenOutputButton.IsEnabled = !isBusy;
        SetTriageEditorEnabled(!isBusy && _vm.CurrentSummary is not null);
        UpdateDumpSearchLocationSelectionState();
        StatusText.Text = message;
    }
}
