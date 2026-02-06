using System.Diagnostics;

namespace SkyrimDiagDumpToolWinUI;

internal static class LegacyAnalyzerRunner
{
    private const string LegacyExeName = "SkyrimDiagDumpTool.exe";

    public static string? ResolveLegacyAnalyzerPath()
    {
        var baseDir = AppContext.BaseDirectory;
        var candidates = new[]
        {
            Path.Combine(baseDir, LegacyExeName),
            Path.Combine(baseDir, "..", LegacyExeName),
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

    public static bool TryLaunchAdvancedViewer(DumpToolInvocationOptions options, out string? err)
    {
        err = null;

        if (string.IsNullOrWhiteSpace(options.DumpPath))
        {
            err = "Dump path is empty.";
            return false;
        }

        var dumpPath = Path.GetFullPath(options.DumpPath);
        if (!File.Exists(dumpPath))
        {
            err = "Dump file was not found: " + dumpPath;
            return false;
        }

        var exe = ResolveLegacyAnalyzerPath();
        if (string.IsNullOrWhiteSpace(exe))
        {
            err = LegacyExeName + " was not found next to SkyrimDiagDumpToolWinUI.exe.";
            return false;
        }

        var outDir = ResolveOutputDirectory(dumpPath, options.OutDir);
        Directory.CreateDirectory(outDir);

        var psi = new ProcessStartInfo(exe)
        {
            UseShellExecute = false,
            CreateNoWindow = false,
            WindowStyle = ProcessWindowStyle.Normal,
        };
        psi.ArgumentList.Add(dumpPath);
        psi.ArgumentList.Add("--out-dir");
        psi.ArgumentList.Add(outDir);
        psi.ArgumentList.Add("--advanced-ui");
        if (!string.IsNullOrWhiteSpace(options.Language))
        {
            psi.ArgumentList.Add("--lang");
            psi.ArgumentList.Add(options.Language!);
        }

        try
        {
            var process = Process.Start(psi);
            if (process is null)
            {
                err = "Failed to start advanced viewer process.";
                return false;
            }
            return true;
        }
        catch (Exception ex)
        {
            err = ex.Message;
            return false;
        }
    }

    public static async Task<int> RunHeadlessAsync(DumpToolInvocationOptions options, CancellationToken cancellationToken)
    {
        if (string.IsNullOrWhiteSpace(options.DumpPath))
        {
            return 2;
        }

        var dumpPath = Path.GetFullPath(options.DumpPath);
        if (!File.Exists(dumpPath))
        {
            return 2;
        }

        var exe = ResolveLegacyAnalyzerPath();
        if (string.IsNullOrWhiteSpace(exe))
        {
            return 5;
        }

        var outDir = ResolveOutputDirectory(dumpPath, options.OutDir);
        Directory.CreateDirectory(outDir);

        var psi = new ProcessStartInfo(exe)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            WindowStyle = ProcessWindowStyle.Hidden,
        };
        psi.ArgumentList.Add(dumpPath);
        psi.ArgumentList.Add("--out-dir");
        psi.ArgumentList.Add(outDir);
        psi.ArgumentList.Add("--headless");

        if (!string.IsNullOrWhiteSpace(options.Language))
        {
            psi.ArgumentList.Add("--lang");
            psi.ArgumentList.Add(options.Language!);
        }

        try
        {
            using var process = Process.Start(psi);
            if (process is null)
            {
                return 6;
            }
            await process.WaitForExitAsync(cancellationToken);
            return process.ExitCode;
        }
        catch
        {
            return 6;
        }
    }
}
