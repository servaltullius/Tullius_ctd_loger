using System.IO;

using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

using Windows.Storage.Pickers;

namespace SkyrimDiagDumpToolWinUI;

public sealed partial class MainWindow
{
    private async Task RefreshDiscoveredDumpsAsync()
    {
        var hintDumpPath = !string.IsNullOrWhiteSpace(DumpPathBox.Text)
            ? DumpPathBox.Text.Trim()
            : _startupOptions.DumpPath;

        var recentDumps = DumpDiscoveryService.DiscoverRecentDumps(_dumpDiscoveryState, hintDumpPath, _vm.IsKorean);
        var searchLocations = DumpDiscoveryService.BuildSearchLocationItems(_dumpDiscoveryState, hintDumpPath, _vm.IsKorean);
        _vm.PopulateDumpDiscovery(recentDumps, searchLocations, BuildDumpDiscoveryStatusText(recentDumps.Count, searchLocations.Count));

        RecentDumpsStatusText.Text = _vm.RecentDumpStatusText;
        RecentDumpList.Visibility = _vm.RecentDumps.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        RecentDumpsEmptyState.Visibility = _vm.RecentDumps.Count > 0 ? Visibility.Collapsed : Visibility.Visible;
        DumpSearchLocationsEmptyText.Visibility = _vm.DumpSearchLocations.Count > 0 ? Visibility.Collapsed : Visibility.Visible;
        UpdateDumpSearchLocationSelectionState();
        await Task.CompletedTask;
    }

    private string BuildDumpDiscoveryStatusText(int dumpCount, int searchLocationCount)
    {
        if (_vm.IsKorean)
        {
            return $"출력 위치 {searchLocationCount}곳, 최근 덤프 {dumpCount}개";
        }

        return $"{searchLocationCount} output locations, {dumpCount} recent dumps";
    }

    private async Task PromoteLearnedDumpLocationAsync(string dumpPath)
    {
        if (!DumpDiscoveryService.CanPromoteLearnedRoot(_dumpDiscoveryState, dumpPath))
        {
            return;
        }

        string? directoryPath;
        try
        {
            directoryPath = Path.GetDirectoryName(Path.GetFullPath(dumpPath));
        }
        catch
        {
            return;
        }

        if (string.IsNullOrWhiteSpace(directoryPath))
        {
            return;
        }

        _dumpDiscoveryState = DumpDiscoveryStore.PromoteLearnedRoot(_dumpDiscoveryState, directoryPath);
        await DumpDiscoveryStore.SaveAsync(_dumpDiscoveryState, CancellationToken.None);
    }

    private void UpdateDumpSearchLocationSelectionState()
    {
        RemoveDumpSearchLocationButton.IsEnabled =
            DumpSearchLocationsList.IsEnabled &&
            DumpSearchLocationsList.SelectedItem is DumpSearchLocationItem item &&
            item.IsRemovable;
    }

    private async void RescanDumpsButton_Click(object sender, RoutedEventArgs e)
    {
        await RefreshDiscoveredDumpsAsync();
        StatusText.Text = T("Rescanned known dump output locations.", "알려진 덤프 출력 위치를 다시 스캔했습니다.");
    }

    private void ManageDumpFoldersButton_Click(object sender, RoutedEventArgs e)
    {
        DumpSearchLocationsPanel.Visibility =
            DumpSearchLocationsPanel.Visibility == Visibility.Visible ? Visibility.Collapsed : Visibility.Visible;
        UpdateDumpSearchLocationSelectionState();
    }

    private async void AddDumpSearchLocation_Click(object sender, RoutedEventArgs e)
    {
        var picker = new FolderPicker();
        picker.FileTypeFilter.Add("*");
        picker.SuggestedStartLocation = PickerLocationId.DocumentsLibrary;
        WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(this));

        var folder = await picker.PickSingleFolderAsync();
        if (folder is null)
        {
            return;
        }

        _dumpDiscoveryState = DumpDiscoveryStore.AddRegisteredRoot(_dumpDiscoveryState, folder.Path);
        await DumpDiscoveryStore.SaveAsync(_dumpDiscoveryState, CancellationToken.None);
        DumpSearchLocationsPanel.Visibility = Visibility.Visible;
        await RefreshDiscoveredDumpsAsync();
        StatusText.Text = T("Dump output location saved.", "덤프 출력 위치를 저장했습니다.");
    }

    private async void RemoveDumpSearchLocation_Click(object sender, RoutedEventArgs e)
    {
        if (DumpSearchLocationsList.SelectedItem is not DumpSearchLocationItem item || !item.IsRemovable)
        {
            return;
        }

        _dumpDiscoveryState = DumpDiscoveryStore.RemoveRoot(_dumpDiscoveryState, item.Path);
        await DumpDiscoveryStore.SaveAsync(_dumpDiscoveryState, CancellationToken.None);
        await RefreshDiscoveredDumpsAsync();
        StatusText.Text = T("Removed dump output location.", "덤프 출력 위치를 제거했습니다.");
    }

    private void DumpSearchLocationsList_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        UpdateDumpSearchLocationSelectionState();
    }

    private async void AnalyzeRecentDump_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not Button button || button.Tag is not string dumpPath || string.IsNullOrWhiteSpace(dumpPath))
        {
            return;
        }

        DumpPathBox.Text = dumpPath;
        await PromoteLearnedDumpLocationAsync(dumpPath);
        await RefreshDiscoveredDumpsAsync();
        await AnalyzeAsync(preferExistingArtifacts: true);
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
            await PromoteLearnedDumpLocationAsync(file.Path);
            await RefreshDiscoveredDumpsAsync();
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
}
