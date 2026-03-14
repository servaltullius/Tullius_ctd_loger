namespace SkyrimDiagDumpToolWinUI;

public static class Program
{
    [STAThread]
    public static int Main(string[] args)
    {
        var options = DumpToolInvocationOptions.Parse(args);
        if (options.Headless)
        {
            return HeadlessEntryPoint.Run(options);
        }

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
