namespace SkyrimDiagDumpToolWinUI;

internal static class HeadlessEntryPoint
{
    public static int Run(DumpToolInvocationOptions options)
    {
        HeadlessBootstrapLog.Write("headless.run.start");
        var (exitCode, error) = NativeAnalyzerBridge.RunAnalyzeAsync(options, CancellationToken.None).GetAwaiter().GetResult();
        HeadlessBootstrapLog.Write(
            "headless.run.result",
            $"code={exitCode} error={(string.IsNullOrWhiteSpace(error) ? "<empty>" : error.Replace(Environment.NewLine, " | "))}");
        if (exitCode != 0 && !string.IsNullOrWhiteSpace(error))
        {
            Console.Error.WriteLine(error);
        }

        return exitCode;
    }
}
