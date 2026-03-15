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
            StatusText.Text = startupWarning;
        }

        DispatcherQueue.TryEnqueue(async () =>
        {
            await RefreshDiscoveredDumpsAsync();
            if (!string.IsNullOrWhiteSpace(_startupOptions.DumpPath))
            {
                await AnalyzeAsync(preferExistingArtifacts: true);
            }
        });

        ApplyAdaptiveLayout();
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
