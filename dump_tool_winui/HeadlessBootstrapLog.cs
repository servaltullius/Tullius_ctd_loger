using System.Text;

namespace SkyrimDiagDumpToolWinUI;

internal static class HeadlessBootstrapLog
{
    private static readonly object Sync = new();

    public static string PathOnDisk =>
        Path.Combine(AppContext.BaseDirectory, "SkyrimDiagDumpToolWinUI_headless_bootstrap.log");

    public static void Write(string stage, string? details = null)
    {
        try
        {
            lock (Sync)
            {
                var sb = new StringBuilder();
                sb.Append(DateTime.UtcNow.ToString("O"));
                sb.Append(" [");
                sb.Append(stage);
                sb.Append(']');
                if (!string.IsNullOrWhiteSpace(details))
                {
                    sb.Append(' ');
                    sb.Append(details);
                }
                sb.AppendLine();
                File.AppendAllText(PathOnDisk, sb.ToString(), Encoding.UTF8);
            }
        }
        catch
        {
        }
    }
}
