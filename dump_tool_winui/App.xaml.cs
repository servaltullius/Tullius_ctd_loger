using Microsoft.UI.Xaml;

namespace SkyrimDiagDumpToolWinUI;

public partial class App : Application
{
    private Window? _window;

    public App()
    {
        InitializeComponent();
    }

    protected override async void OnLaunched(LaunchActivatedEventArgs args)
    {
        var options = DumpToolInvocationOptions.Parse(Environment.GetCommandLineArgs().Skip(1).ToArray());

        if (options.Headless)
        {
            var exitCode = await LegacyAnalyzerRunner.RunHeadlessAsync(options, CancellationToken.None);
            Environment.Exit(exitCode);
            return;
        }

        string? startupWarning = null;
        if (options.ForceAdvancedUi && !string.IsNullOrWhiteSpace(options.DumpPath))
        {
            if (LegacyAnalyzerRunner.TryLaunchAdvancedViewer(options, out startupWarning))
            {
                Environment.Exit(0);
                return;
            }
        }

        if (LegacyAnalyzerRunner.ResolveLegacyAnalyzerPath() is null)
        {
            startupWarning = "SkyrimDiagDumpTool.exe was not found next to SkyrimDiagDumpToolWinUI.exe.";
        }

        _window = new MainWindow(options, startupWarning);
        _window.Activate();
    }
}
