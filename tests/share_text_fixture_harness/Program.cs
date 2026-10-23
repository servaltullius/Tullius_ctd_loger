using SkyrimDiagDumpToolWinUI;

internal static class Program
{
    private static int Main(string[] args)
    {
        if (args.Length != 2)
        {
            Console.Error.WriteLine("usage: ShareTextFixtureHarness <summary.json> <community|clipboard|cleared|groups>");
            return 2;
        }

        var summaryPath = Path.GetFullPath(args[0]);
        var mode = args[1].Trim().ToLowerInvariant();

        var summary = AnalysisSummary.LoadFromSummaryFile(summaryPath);
        var vm = new MainWindowViewModel(isKorean: false)
        {
            CurrentDumpPath = "/tmp/" + Path.GetFileNameWithoutExtension(summaryPath) + ".dmp",
        };
        vm.PopulateSummary(summary);

        var output = mode switch
        {
            "community" => vm.BuildCommunityShareText(),
            "clipboard" => vm.BuildSummaryClipboardText(),
            "cleared" => ClearedShareText(vm),
            "groups" => RecommendationGroupsText(vm),
            _ => throw new InvalidOperationException("unsupported mode: " + mode),
        };

        if (!string.IsNullOrEmpty(output))
        {
            Console.Write(output.Replace("\r\n", "\n"));
        }

        return 0;
    }

    // The recommendation sections and the next action as the viewer shows them.
    private static string RecommendationGroupsText(MainWindowViewModel vm)
    {
        var lines = new List<string>();
        lines.AddRange(vm.ImmediateRecommendations.Select(item => "immediate: " + item));
        lines.AddRange(vm.VerificationRecommendations.Select(item => "verify: " + item));
        lines.AddRange(vm.RecaptureRecommendations.Select(item => "recapture: " + item));
        lines.Add("next: " + vm.QuickActionsValue);
        return string.Join("\n", lines) + "\n";
    }

    // A newly selected dump drops the rendered summary; neither copy action
    // may still produce text from the previous dump.
    private static string ClearedShareText(MainWindowViewModel vm)
    {
        vm.CurrentDumpPath = "/tmp/OtherDump.dmp";
        vm.ClearSummary();
        return (vm.BuildSummaryClipboardText() ?? string.Empty) +
               (vm.BuildCommunityShareText() ?? string.Empty);
    }
}
