using System.Diagnostics;
using Microsoft.UI.Xaml;
using System.Text;

namespace SkyrimDiagDumpToolWinUI;

public partial class App : Application
{
    private Window? _window;

    public App()
    {
        InitializeComponent();
        UnhandledException += OnAppUnhandledException;
        AppDomain.CurrentDomain.UnhandledException += OnDomainUnhandledException;
        TaskScheduler.UnobservedTaskException += OnUnobservedTaskException;
    }

    protected override async void OnLaunched(LaunchActivatedEventArgs args)
    {
        try
        {
            var options = DumpToolInvocationOptions.Parse(Environment.GetCommandLineArgs().Skip(1).ToArray());

            if (options.Headless)
            {
                var (exitCode, error) = await NativeAnalyzerBridge.RunAnalyzeAsync(options, CancellationToken.None);
                if (exitCode != 0 && !string.IsNullOrWhiteSpace(error))
                {
                    Console.Error.WriteLine(error);
                }
                Environment.Exit(exitCode);
                return;
            }

            string? startupWarning = null;

            if (NativeAnalyzerBridge.ResolveNativeAnalyzerPath() is null)
            {
                startupWarning = "SkyrimDiagDumpToolNative.dll was not found next to SkyrimDiagDumpToolWinUI.exe.";
            }

            _window = new MainWindow(options, startupWarning);
            _window.Activate();
        }
        catch (Exception ex)
        {
            WriteStartupCrashLog("OnLaunched", ex);
            throw;
        }
    }

    private void OnAppUnhandledException(object sender, Microsoft.UI.Xaml.UnhandledExceptionEventArgs e)
    {
        WriteStartupCrashLog("App.UnhandledException", e.Exception);
    }

    private void OnDomainUnhandledException(object? sender, System.UnhandledExceptionEventArgs e)
    {
        if (e.ExceptionObject is Exception ex)
        {
            WriteStartupCrashLog("AppDomain.CurrentDomain.UnhandledException", ex);
        }
        else
        {
            WriteStartupCrashLog("AppDomain.CurrentDomain.UnhandledException", null);
        }
    }

    private void OnUnobservedTaskException(object? sender, UnobservedTaskExceptionEventArgs e)
    {
        WriteStartupCrashLog("TaskScheduler.UnobservedTaskException", e.Exception);
    }

    private static void WriteStartupCrashLog(string source, Exception? ex)
    {
        try
        {
            var path = Path.Combine(AppContext.BaseDirectory, "SkyrimDiagDumpToolWinUI_startup_error.log");
            var sb = new StringBuilder();
            sb.AppendLine("==== Startup Crash Log ====");
            sb.AppendLine("TimeUtc=" + DateTime.UtcNow.ToString("O"));
            sb.AppendLine("Source=" + source);
            sb.AppendLine("ExeBase=" + AppContext.BaseDirectory);
            if (ex is not null)
            {
                sb.AppendLine("ExceptionType=" + ex.GetType().FullName);
                sb.AppendLine("Message=" + ex.Message);
                sb.AppendLine("HResult=0x" + ex.HResult.ToString("X8"));
                sb.AppendLine("StackTrace:");
                sb.AppendLine(ex.ToString());
            }
            sb.AppendLine();
            File.AppendAllText(path, sb.ToString(), Encoding.UTF8);
        }
        catch (Exception writeEx)
        {
            Debug.WriteLine($"Startup crash log write failed: {writeEx.GetType().Name}: {writeEx.Message}");
        }
    }
}
