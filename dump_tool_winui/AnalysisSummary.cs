using System.Text.Json;

namespace SkyrimDiagDumpToolWinUI;

internal sealed class AnalysisSummary
{
    public required string SummarySentence { get; init; }
    public required string CrashBucketKey { get; init; }
    public required string ModulePlusOffset { get; init; }
    public required string InferredModName { get; init; }
    public required IReadOnlyList<SuspectItem> Suspects { get; init; }
    public required IReadOnlyList<string> Recommendations { get; init; }

    public static AnalysisSummary LoadFromSummaryFile(string summaryPath)
    {
        using var stream = File.OpenRead(summaryPath);
        using var doc = JsonDocument.Parse(stream);
        var root = doc.RootElement;

        var exception = root.TryGetProperty("exception", out var exNode) ? exNode : default;

        var suspects = new List<SuspectItem>();
        if (root.TryGetProperty("suspects", out var suspectsNode) &&
            suspectsNode.ValueKind == JsonValueKind.Array)
        {
            foreach (var item in suspectsNode.EnumerateArray())
            {
                var module = FirstNonEmpty(
                    ReadString(item, "module_filename"),
                    ReadString(item, "module_path"),
                    ReadString(item, "inferred_mod_name"));

                suspects.Add(new SuspectItem(
                    ReadString(item, "confidence"),
                    module,
                    ReadString(item, "reason")));
            }
        }

        var recommendations = new List<string>();
        if (root.TryGetProperty("recommendations", out var recNode) &&
            recNode.ValueKind == JsonValueKind.Array)
        {
            foreach (var item in recNode.EnumerateArray())
            {
                if (item.ValueKind == JsonValueKind.String)
                {
                    var line = item.GetString();
                    if (!string.IsNullOrWhiteSpace(line))
                    {
                        recommendations.Add(line.Trim());
                    }
                }
            }
        }

        return new AnalysisSummary
        {
            SummarySentence = ReadString(root, "summary_sentence"),
            CrashBucketKey = ReadString(root, "crash_bucket_key"),
            ModulePlusOffset = ReadString(exception, "module_plus_offset"),
            InferredModName = FirstNonEmpty(
                ReadString(exception, "inferred_mod_name"),
                suspects.FirstOrDefault()?.Module ?? string.Empty),
            Suspects = suspects,
            Recommendations = recommendations,
        };
    }

    private static string ReadString(JsonElement node, string name)
    {
        if (node.ValueKind == JsonValueKind.Undefined ||
            node.ValueKind == JsonValueKind.Null ||
            !node.TryGetProperty(name, out var child))
        {
            return string.Empty;
        }

        return child.ValueKind == JsonValueKind.String ? child.GetString() ?? string.Empty : string.Empty;
    }

    private static string FirstNonEmpty(params string[] values)
    {
        foreach (var value in values)
        {
            if (!string.IsNullOrWhiteSpace(value))
            {
                return value.Trim();
            }
        }
        return string.Empty;
    }
}

internal sealed record SuspectItem(string Confidence, string Module, string Reason);
