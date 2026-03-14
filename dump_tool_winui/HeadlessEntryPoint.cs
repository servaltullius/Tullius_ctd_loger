namespace SkyrimDiagDumpToolWinUI;

internal static class HeadlessEntryPoint
{
    public static int Run(DumpToolInvocationOptions options)
    {
        var (exitCode, error) = NativeAnalyzerBridge.RunAnalyzeAsync(options, CancellationToken.None).GetAwaiter().GetResult();
        if (exitCode != 0 && !string.IsNullOrWhiteSpace(error))
        {
            Console.Error.WriteLine(error);
        }

        return exitCode;
    }
}
