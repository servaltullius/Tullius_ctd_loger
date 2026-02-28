using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace SkyrimDiagDumpToolWinUI;

internal static class NativeAnalyzerBridge
{
    private const string NativeDllName = "SkyrimDiagDumpToolNative.dll";
    public const int UserCanceledExitCode = 1223;

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

        if (cancellationToken.IsCancellationRequested)
        {
            return (UserCanceledExitCode, "analysis canceled");
        }

        if (!options.Headless)
        {
            return await RunAnalyzeOutOfProcessAsync(options, dumpPath, outDir, cancellationToken);
        }

        return await RunAnalyzeInProcessAsync(options, dumpPath, outDir, cancellationToken);
    }

    private static async Task<(int exitCode, string error)> RunAnalyzeOutOfProcessAsync(
        DumpToolInvocationOptions options,
        string dumpPath,
        string outDir,
        CancellationToken cancellationToken)
    {
        var hostPath = ResolveHeadlessHostPath();
        if (string.IsNullOrWhiteSpace(hostPath) || !File.Exists(hostPath))
        {
            return await RunAnalyzeInProcessAsync(options, dumpPath, outDir, cancellationToken);
        }

        var startInfo = new ProcessStartInfo
        {
            FileName = hostPath,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            WorkingDirectory = Path.GetDirectoryName(hostPath) ?? AppContext.BaseDirectory,
        };

        startInfo.ArgumentList.Add("--headless");
        startInfo.ArgumentList.Add("--out-dir");
        startInfo.ArgumentList.Add(outDir);

        if (!string.IsNullOrWhiteSpace(options.Language))
        {
            startInfo.ArgumentList.Add("--lang");
            startInfo.ArgumentList.Add(options.Language!);
        }
        if (options.Debug)
        {
            startInfo.ArgumentList.Add("--debug");
        }
        startInfo.ArgumentList.Add(options.AllowOnlineSymbols ? "--allow-online-symbols" : "--no-online-symbols");
        startInfo.ArgumentList.Add(dumpPath);

        using var process = new Process
        {
            StartInfo = startInfo,
        };

        try
        {
            if (!process.Start())
            {
                return (6, "Failed to start headless analysis host.");
            }

            var stderrTask = process.StandardError.ReadToEndAsync();
            var stdoutTask = process.StandardOutput.ReadToEndAsync();

            using var registration = cancellationToken.Register(() =>
            {
                try
                {
                    if (!process.HasExited)
                    {
                        process.Kill(entireProcessTree: true);
                    }
                }
                catch (Exception ex)
                {
                    Debug.WriteLine($"Process kill on cancel failed: {ex.GetType().Name}: {ex.Message}");
                }
            });

            try
            {
                await process.WaitForExitAsync(cancellationToken);
            }
            catch (OperationCanceledException)
            {
                try
                {
                    if (!process.HasExited)
                    {
                        process.Kill(entireProcessTree: true);
                    }
                }
                catch (Exception ex)
                {
                    Debug.WriteLine($"Process kill after cancel failed: {ex.GetType().Name}: {ex.Message}");
                }
                await process.WaitForExitAsync(CancellationToken.None);
                _ = await stderrTask;
                _ = await stdoutTask;
                return (UserCanceledExitCode, "analysis canceled");
            }

            var stderr = (await stderrTask).Trim();
            var stdout = (await stdoutTask).Trim();
            var message = !string.IsNullOrWhiteSpace(stderr) ? stderr : stdout;
            return (process.ExitCode, message);
        }
        catch (OperationCanceledException)
        {
            return (UserCanceledExitCode, "analysis canceled");
        }
        catch (Exception ex)
        {
            return (6, "Headless process execution failed: " + ex.Message);
        }
    }

    private static async Task<(int exitCode, string error)> RunAnalyzeInProcessAsync(
        DumpToolInvocationOptions options,
        string dumpPath,
        string outDir,
        CancellationToken cancellationToken)
    {
        try
        {
            return await Task.Run(() =>
            {
                cancellationToken.ThrowIfCancellationRequested();
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
        catch (OperationCanceledException)
        {
            return (UserCanceledExitCode, "analysis canceled");
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
            var logPath = Path.Combine(
                outDir,
                Path.GetFileNameWithoutExtension(dumpPath) + "_SkyrimDiagNativeException.log");
            try
            {
                File.WriteAllText(logPath, ex.ToString());
            }
            catch (Exception writeEx)
            {
                Debug.WriteLine($"Best-effort native exception log write failed: {writeEx.GetType().Name}: {writeEx.Message}");
            }

            var details = $"{ex.GetType().FullName} (0x{ex.HResult:X8}): {ex.Message}";
            if (!string.IsNullOrWhiteSpace(logPath))
            {
                details += "\nLog: " + logPath;
            }
            return (6, details);
        }
    }

    private static string? ResolveHeadlessHostPath()
    {
        var processPath = Environment.ProcessPath;
        if (!string.IsNullOrWhiteSpace(processPath))
        {
            return processPath;
        }

        var candidate = Path.Combine(AppContext.BaseDirectory, "SkyrimDiagDumpToolWinUI.exe");
        return File.Exists(candidate) ? candidate : null;
    }
}
