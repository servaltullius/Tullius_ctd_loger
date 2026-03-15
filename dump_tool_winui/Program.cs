namespace SkyrimDiagDumpToolWinUI;

public static class Program
{
    [STAThread]
    public static int Main(string[] args)
    {
        HeadlessBootstrapLog.Write("main.enter", $"argc={args.Length}");
        var options = DumpToolInvocationOptions.Parse(args);
        if (options.Headless)
        {
            HeadlessBootstrapLog.Write(
                "main.headless",
                $"dump={options.DumpPath ?? "<null>"} out={options.OutDir ?? "<null>"}");
            try
            {
                var exitCode = HeadlessEntryPoint.Run(options);
                HeadlessBootstrapLog.Write("main.headless.exit", $"code={exitCode}");
                return exitCode;
            }
            catch (Exception ex)
            {
                HeadlessBootstrapLog.Write("main.headless.exception", ex.ToString());
                throw;
            }
        }

        HeadlessBootstrapLog.Write("main.interactive");
        global::WinRT.ComWrappersSupport.InitializeComWrappers();
        global::Microsoft.UI.Xaml.Application.Start((p) =>
        {
            var context = new global::Microsoft.UI.Dispatching.DispatcherQueueSynchronizationContext(
                global::Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread());
            global::System.Threading.SynchronizationContext.SetSynchronizationContext(context);
            _ = new App();
        });
        return 0;
    }
}
