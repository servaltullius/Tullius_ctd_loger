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
}
