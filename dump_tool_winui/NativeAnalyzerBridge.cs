using System.Runtime.InteropServices;
using System.Text;

namespace SkyrimDiagDumpToolWinUI;

internal static class NativeAnalyzerBridge
{
    private const string NativeDllName = "SkyrimDiagDumpToolNative.dll";

    [DllImport(NativeDllName, CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Unicode, EntryPoint = "SkyrimDiagAnalyzeDumpW")]
    private static extern int SkyrimDiagAnalyzeDumpW(
        string dumpPath,
        string outDir,
        string? languageToken,
        int debug,
        StringBuilder errorBuf,
        int errorBufChars);

    public static string? ResolveNativeAnalyzerPath()
    {
        var baseDir = AppContext.BaseDirectory;
        var candidates = new[]
        {
            Path.Combine(baseDir, NativeDllName),
            Path.Combine(baseDir, "..", NativeDllName),
        };

        foreach (var candidate in candidates)
        {
            var full = Path.GetFullPath(candidate);
            if (File.Exists(full))
            {
                return full;
            }
        }
        return null;
    }

    public static string ResolveOutputDirectory(string dumpPath, string? preferredOutDir)
    {
        if (!string.IsNullOrWhiteSpace(preferredOutDir))
        {
            return Path.GetFullPath(preferredOutDir);
        }

        var parent = Path.GetDirectoryName(dumpPath);
        return string.IsNullOrWhiteSpace(parent) ? AppContext.BaseDirectory : parent;
    }

    public static string ResolveSummaryPath(string dumpPath, string outDir)
    {
        var stem = Path.GetFileNameWithoutExtension(dumpPath);
        return Path.Combine(outDir, stem + "_SkyrimDiagSummary.json");
    }

    public static string ResolveReportPath(string dumpPath, string outDir)
    {
        var stem = Path.GetFileNameWithoutExtension(dumpPath);
        return Path.Combine(outDir, stem + "_SkyrimDiagReport.txt");
    }

    public static string ResolveBlackboxPath(string dumpPath, string outDir)
    {
        var stem = Path.GetFileNameWithoutExtension(dumpPath);
        return Path.Combine(outDir, stem + "_SkyrimDiagBlackbox.jsonl");
    }

    public static string ResolveWctPath(string dumpPath, string outDir)
    {
        var stem = Path.GetFileNameWithoutExtension(dumpPath);
        return Path.Combine(outDir, stem + "_SkyrimDiagWct.json");
    }

    public static async Task<(int exitCode, string error)> RunAnalyzeAsync(
        DumpToolInvocationOptions options,
        CancellationToken cancellationToken)
    {
        if (string.IsNullOrWhiteSpace(options.DumpPath))
        {
            return (2, "dump path is empty");
        }

        var dumpPath = Path.GetFullPath(options.DumpPath);
        if (!File.Exists(dumpPath))
        {
            return (2, "dump file not found: " + dumpPath);
        }

        var outDir = ResolveOutputDirectory(dumpPath, options.OutDir);
        Directory.CreateDirectory(outDir);

        try
        {
            return await Task.Run(() =>
            {
                var err = new StringBuilder(4096);
                var previous = Environment.GetEnvironmentVariable("SKYRIMDIAG_ALLOW_ONLINE_SYMBOLS");
                Environment.SetEnvironmentVariable(
                    "SKYRIMDIAG_ALLOW_ONLINE_SYMBOLS",
                    options.AllowOnlineSymbols ? "1" : "0");
                try
                {
                    var rc = SkyrimDiagAnalyzeDumpW(
                        dumpPath,
                        outDir,
                        options.Language,
                        options.Debug ? 1 : 0,
                        err,
                        err.Capacity);
                    return (rc, err.ToString());
                }
                finally
                {
                    Environment.SetEnvironmentVariable("SKYRIMDIAG_ALLOW_ONLINE_SYMBOLS", previous);
                }
            }, cancellationToken);
        }
        catch (DllNotFoundException ex)
        {
            return (5, NativeDllName + " was not found: " + ex.Message);
        }
        catch (EntryPointNotFoundException ex)
        {
            return (5, "Native entry point SkyrimDiagAnalyzeDumpW was not found: " + ex.Message);
        }
        catch (Exception ex)
        {
            return (6, ex.Message);
        }
    }
}
