using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;

namespace SkyrimDiagDumpToolWinUI;

public sealed partial class MainWindow
{
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

    private void NavView_SelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (args.SelectedItem is NavigationViewItem item && item.Tag is string tag)
        {
            AnalyzePanel.Visibility = tag == "analyze" ? Visibility.Visible : Visibility.Collapsed;
            TriagePanel.Visibility = tag == "triage" ? Visibility.Visible : Visibility.Collapsed;
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
        RecentDumpsTitleText.FontSize = compact ? 22 : 24;
        SnapshotSectionTitleText.FontSize = narrow ? 22 : (compact ? 26 : 30);

        if (narrow)
        {
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

        RecentDumpList.MaxHeight = compact ? 260 : 320;
        DumpSearchLocationsList.MaxHeight = compact ? 180 : 220;
        SuspectsList.MaxHeight = compact ? 240 : 320;
        CallstackList.MaxHeight = compact ? 320 : 500;
        EvidenceList.MaxHeight = compact ? 320 : 500;
        ResourcesList.MaxHeight = compact ? 320 : 500;
        EventsList.MaxHeight = compact ? 380 : 600;

        WctTextBox.MinHeight = compact ? 160 : 200;
        WctTextBox.MaxHeight = compact ? 300 : 400;
        ReportTextBox.MinHeight = compact ? 180 : 200;
        ReportTextBox.MaxHeight = compact ? 360 : 500;
    }
}
