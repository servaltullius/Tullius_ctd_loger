using SkyrimDiagDumpToolWinUI;

internal static class Program
{
    private static int Main(string[] args)
    {
        if (args.Length != 2)
        {
            Console.Error.WriteLine("usage: ShareTextFixtureHarness <summary.json> <community|clipboard>");
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
            _ => throw new InvalidOperationException("unsupported mode: " + mode),
        };

        if (!string.IsNullOrEmpty(output))
        {
            Console.Write(output.Replace("\r\n", "\n"));
        }

        return 0;
    }
}
