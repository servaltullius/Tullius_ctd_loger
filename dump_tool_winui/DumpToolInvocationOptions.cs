namespace SkyrimDiagDumpToolWinUI;

internal sealed class DumpToolInvocationOptions
{
    public string? DumpPath { get; set; }
    public string? OutDir { get; set; }
    public string? Language { get; set; }
    public bool Headless { get; set; }
    public bool ForceSimpleUi { get; set; }
    public bool ForceAdvancedUi { get; set; }

    public static DumpToolInvocationOptions Parse(IReadOnlyList<string> args)
    {
        var options = new DumpToolInvocationOptions();

        for (var i = 0; i < args.Count; i++)
        {
            var a = args[i];

            if (string.Equals(a, "--out-dir", StringComparison.OrdinalIgnoreCase) && i + 1 < args.Count)
            {
                options.OutDir = args[++i];
                continue;
            }

            if ((string.Equals(a, "--lang", StringComparison.OrdinalIgnoreCase) ||
                 string.Equals(a, "--language", StringComparison.OrdinalIgnoreCase)) && i + 1 < args.Count)
            {
                options.Language = args[++i];
                continue;
            }

            if (string.Equals(a, "--headless", StringComparison.OrdinalIgnoreCase))
            {
                options.Headless = true;
                continue;
            }

            if (string.Equals(a, "--simple-ui", StringComparison.OrdinalIgnoreCase))
            {
                options.ForceSimpleUi = true;
                options.ForceAdvancedUi = false;
                continue;
            }

            if (string.Equals(a, "--advanced-ui", StringComparison.OrdinalIgnoreCase))
            {
                options.ForceAdvancedUi = true;
                options.ForceSimpleUi = false;
                continue;
            }

            if (string.Equals(a, "--help", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(a, "-h", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(a, "/?", StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            if (a.StartsWith("-", StringComparison.Ordinal))
            {
                continue;
            }

            if (string.IsNullOrWhiteSpace(options.DumpPath))
            {
                options.DumpPath = a;
            }
        }

        return options;
    }
}
