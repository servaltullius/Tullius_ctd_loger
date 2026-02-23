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
    public required IReadOnlyList<string> CallstackFrames { get; init; }
    public required IReadOnlyList<EvidenceViewItem> EvidenceItems { get; init; }
    public required IReadOnlyList<ResourceViewItem> ResourceItems { get; init; }
    public int HistoryCorrelationCount { get; init; }

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

        var callstackFrames = new List<string>();
        if (root.TryGetProperty("callstack", out var callstackNode) &&
            callstackNode.ValueKind == JsonValueKind.Object &&
            callstackNode.TryGetProperty("frames", out var frameNode) &&
            frameNode.ValueKind == JsonValueKind.Array)
        {
            foreach (var item in frameNode.EnumerateArray())
            {
                if (item.ValueKind == JsonValueKind.String)
                {
                    var frame = item.GetString();
                    if (!string.IsNullOrWhiteSpace(frame))
                    {
                        callstackFrames.Add(frame.Trim());
                    }
                }
            }
        }

        var evidenceItems = new List<EvidenceViewItem>();
        if (root.TryGetProperty("evidence", out var evidenceNode) &&
            evidenceNode.ValueKind == JsonValueKind.Array)
        {
            foreach (var item in evidenceNode.EnumerateArray())
            {
                evidenceItems.Add(new EvidenceViewItem(
                    ReadString(item, "confidence"),
                    ReadString(item, "title"),
                    ReadString(item, "details")));
            }
        }

        var resourceItems = new List<ResourceViewItem>();
        if (root.TryGetProperty("resources", out var resourcesNode) &&
            resourcesNode.ValueKind == JsonValueKind.Array)
        {
            foreach (var item in resourcesNode.EnumerateArray())
            {
                var providers = new List<string>();
                if (item.TryGetProperty("providers", out var providersNode) &&
                    providersNode.ValueKind == JsonValueKind.Array)
                {
                    foreach (var p in providersNode.EnumerateArray())
                    {
                        if (p.ValueKind == JsonValueKind.String)
                        {
                            var s = p.GetString();
                            if (!string.IsNullOrWhiteSpace(s))
                            {
                                providers.Add(s.Trim());
                            }
                        }
                    }
                }

                var kind = ReadString(item, "kind");
                var path = ReadString(item, "path");
                var conflict = ReadBool(item, "is_conflict");
                resourceItems.Add(new ResourceViewItem(
                    string.IsNullOrWhiteSpace(kind) ? "resource" : kind,
                    path,
                    providers.Count == 0 ? "-" : string.Join(", ", providers),
                    conflict ? "conflict" : ""));
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
            CallstackFrames = callstackFrames,
            EvidenceItems = evidenceItems,
            ResourceItems = resourceItems,
            HistoryCorrelationCount = root.TryGetProperty("history_correlation", out var histCorr)
                && histCorr.ValueKind == JsonValueKind.Object
                && histCorr.TryGetProperty("count", out var countNode)
                && countNode.TryGetInt32(out var hcCount) ? hcCount : 0,
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

    private static bool ReadBool(JsonElement node, string name)
    {
        if (node.ValueKind == JsonValueKind.Undefined ||
            node.ValueKind == JsonValueKind.Null ||
            !node.TryGetProperty(name, out var child))
        {
            return false;
        }

        return child.ValueKind == JsonValueKind.True ||
               (child.ValueKind == JsonValueKind.Number && child.TryGetInt32(out var n) && n != 0);
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

public sealed record SuspectItem(string Confidence, string Module, string Reason);
public sealed record EvidenceViewItem(string Confidence, string Title, string Details);
public sealed record ResourceViewItem(string Kind, string Path, string Providers, string Conflict);
