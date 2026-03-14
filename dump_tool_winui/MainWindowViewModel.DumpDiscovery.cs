namespace SkyrimDiagDumpToolWinUI;

internal sealed partial class MainWindowViewModel
{
    public void PopulateDumpDiscovery(
        IReadOnlyList<DumpDiscoveryItem> recentDumps,
        IReadOnlyList<DumpSearchLocationItem> dumpSearchLocations,
        string statusText)
    {
        RecentDumps.Clear();
        foreach (var item in recentDumps)
        {
            RecentDumps.Add(item);
        }

        DumpSearchLocations.Clear();
        foreach (var item in dumpSearchLocations)
        {
            DumpSearchLocations.Add(item);
        }

        RecentDumpStatusText = statusText;
    }
}
