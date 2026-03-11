using System.Linq;
using System.Text.Json;

namespace SkyrimDiagDumpToolWinUI;

internal sealed class AnalysisSummary
{
    public required string SummarySentence { get; init; }
    public required string CrashBucketKey { get; init; }
    public required string ModulePlusOffset { get; init; }
    public required string InferredModName { get; init; }
    public required bool IsCrashLike { get; init; }
    public required bool IsHangLike { get; init; }
    public required bool IsSnapshotLike { get; init; }
    public required bool IsManualCapture { get; init; }
    public required IReadOnlyList<SuspectItem> Suspects { get; init; }
    public required IReadOnlyList<string> Recommendations { get; init; }
    public required IReadOnlyList<string> CallstackFrames { get; init; }
    public required IReadOnlyList<EvidenceViewItem> EvidenceItems { get; init; }
    public required IReadOnlyList<ResourceViewItem> ResourceItems { get; init; }
    public required IReadOnlyList<ActionableCandidateItem> ActionableCandidates { get; init; }
    public required TriageReview Triage { get; init; }
    public int HistoryCorrelationCount { get; init; }
    public string TroubleshootingTitle { get; init; } = string.Empty;
    public IReadOnlyList<string> TroubleshootingSteps { get; init; } = Array.Empty<string>();
    public required IReadOnlyList<CrashLoggerRefItem> CrashLoggerRefs { get; init; }
    public IReadOnlyList<string> Diagnostics { get; init; } = Array.Empty<string>();

    public static AnalysisSummary LoadFromSummaryFile(string summaryPath)
    {
        using var stream = File.OpenRead(summaryPath);
        using var doc = JsonDocument.Parse(stream);
        var root = doc.RootElement;

        var exception = root.TryGetProperty("exception", out var exNode) ? exNode : default;
        var analysis = root.TryGetProperty("analysis", out var analysisNode) ? analysisNode : default;

        var suspects = ParseObjectArray(root, "suspects", item => new SuspectItem(
            ReadString(item, "confidence"),
            FirstNonEmpty(
                ReadString(item, "module_filename"),
                ReadString(item, "module_path"),
                ReadString(item, "inferred_mod_name")),
            ReadString(item, "reason")));

        var crashLoggerNode = root.TryGetProperty("crash_logger", out var clNode) ? clNode : default;
        var crashLoggerRefs = ParseObjectArray(crashLoggerNode, "object_refs", item => new CrashLoggerRefItem(
            ReadString(item, "esp_name"),
            ReadString(item, "best_object_type"),
            ReadString(item, "best_location"),
            ReadString(item, "object_name"),
            ReadString(item, "form_id"),
            ReadInt32(item, "ref_count"),
            ReadInt32(item, "relevance_score")));

        var recommendations = ParseStringArray(root, "recommendations");
        var callstackFrames = ParseStringArray(root, "callstack.frames");

        var evidenceItems = ParseObjectArray(root, "evidence", item => new EvidenceViewItem(
            ReadString(item, "confidence"),
            ReadString(item, "title"),
            ReadString(item, "details")));

        var actionableCandidates = ParseObjectArray(root, "actionable_candidates", item => new ActionableCandidateItem(
            ReadString(item, "status_id"),
            ReadString(item, "confidence"),
            ReadString(item, "display_name"),
            ReadString(item, "plugin_name"),
            ReadString(item, "mod_name"),
            ReadString(item, "module_filename"),
            ReadString(item, "explanation"),
            ReadInt32(item, "family_count"),
            ReadInt32(item, "score"),
            ReadBool(item, "cross_validated"),
            ReadBool(item, "has_conflict"),
            ParseStringArray(item, "supporting_families"),
            ParseStringArray(item, "conflicting_families")));

        var resourceItems = ParseObjectArray(root, "resources", item =>
        {
            var providers = ParseStringArray(item, "providers");
            var kind = ReadString(item, "kind");
            return new ResourceViewItem(
                string.IsNullOrWhiteSpace(kind) ? "resource" : kind,
                ReadString(item, "path"),
                providers.Count == 0 ? "-" : string.Join(", ", providers),
                ReadBool(item, "is_conflict") ? "conflict" : "");
        });

        var tsElement = root.TryGetProperty("troubleshooting_steps", out var tsTemp)
            && tsTemp.ValueKind == JsonValueKind.Object ? tsTemp : default;
        var triageElement = root.TryGetProperty("triage", out var triageTemp)
            && triageTemp.ValueKind == JsonValueKind.Object ? triageTemp : default;

        return new AnalysisSummary
        {
            SummarySentence = ReadString(root, "summary_sentence"),
            CrashBucketKey = ReadString(root, "crash_bucket_key"),
            ModulePlusOffset = ReadString(exception, "module_plus_offset"),
            InferredModName = FirstNonEmpty(
                crashLoggerRefs.FirstOrDefault()?.EspName ?? string.Empty,
                ReadString(exception, "inferred_mod_name"),
                suspects.FirstOrDefault()?.Module ?? string.Empty),
            IsCrashLike = ReadBool(analysis, "is_crash_like"),
            IsHangLike = ReadBool(analysis, "is_hang_like"),
            IsSnapshotLike = ReadBool(analysis, "is_snapshot_like"),
            IsManualCapture = ReadBool(analysis, "is_manual_capture"),
            Suspects = suspects,
            Recommendations = recommendations,
            CallstackFrames = callstackFrames,
            EvidenceItems = evidenceItems,
            ActionableCandidates = actionableCandidates,
            CrashLoggerRefs = crashLoggerRefs,
            ResourceItems = resourceItems,
            Triage = ParseTriage(triageElement),
            HistoryCorrelationCount = root.TryGetProperty("history_correlation", out var histCorr)
                && histCorr.ValueKind == JsonValueKind.Object
                && histCorr.TryGetProperty("count", out var countNode)
                && countNode.TryGetInt32(out var hcCount) ? hcCount : 0,
            TroubleshootingTitle = tsElement.ValueKind != JsonValueKind.Undefined
                ? ReadString(tsElement, "title") : string.Empty,
            TroubleshootingSteps = tsElement.ValueKind != JsonValueKind.Undefined
                ? ParseStringArray(tsElement, "steps")
                : new List<string>(),
            Diagnostics = ParseStringArray(root, "diagnostics"),
        };
    }

    private static TriageReview ParseTriage(JsonElement triage)
    {
        var parsed = new TriageReview
        {
            ReviewStatus = SummaryTriageStore.NormalizeReviewStatus(ReadString(triage, "review_status")),
            Reviewed = ReadBool(triage, "reviewed"),
            Verdict = ReadString(triage, "verdict"),
            ActualCause = ReadString(triage, "actual_cause"),
            GroundTruthCause = ReadString(triage, "ground_truth_cause"),
            GroundTruthMod = ReadString(triage, "ground_truth_mod"),
            Reviewer = ReadString(triage, "reviewer"),
            ReviewedAtUtc = ReadString(triage, "reviewed_at_utc"),
            Notes = ReadString(triage, "notes"),
            SignatureMatched = ReadBool(triage, "signature_matched"),
        };

        var reviewStatus = parsed.ReviewStatus;
        if (reviewStatus == TriageReview.UnreviewedStatus && SummaryTriageStore.IsReviewed(parsed))
        {
            reviewStatus = "reviewed";
        }

        return parsed with
        {
            ReviewStatus = reviewStatus,
            Reviewed = SummaryTriageStore.IsReviewed(parsed),
        };
    }

    private static List<string> ParseStringArray(JsonElement root, string propertyPath)
    {
        var result = new List<string>();
        var current = root;
        foreach (var segment in propertyPath.Split('.'))
        {
            if (current.ValueKind != JsonValueKind.Object &&
                current.ValueKind != JsonValueKind.Array)
                return result;
            if (!current.TryGetProperty(segment, out current))
                return result;
        }
        if (current.ValueKind != JsonValueKind.Array) return result;
        foreach (var item in current.EnumerateArray())
        {
            if (item.ValueKind == JsonValueKind.String)
            {
                var s = item.GetString();
                if (!string.IsNullOrWhiteSpace(s)) result.Add(s.Trim());
            }
        }
        return result;
    }

    private static List<T> ParseObjectArray<T>(JsonElement root, string propertyName,
        Func<JsonElement, T> mapper)
    {
        var result = new List<T>();
        if (root.ValueKind != JsonValueKind.Object ||
            !root.TryGetProperty(propertyName, out var node) ||
            node.ValueKind != JsonValueKind.Array)
            return result;
        foreach (var item in node.EnumerateArray())
            result.Add(mapper(item));
        return result;
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

    private static int ReadInt32(JsonElement node, string name)
    {
        if (node.ValueKind == JsonValueKind.Undefined ||
            node.ValueKind == JsonValueKind.Null ||
            !node.TryGetProperty(name, out var child))
            return 0;
        return child.TryGetInt32(out var n) ? n : 0;
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
public sealed record ActionableCandidateItem(
    string StatusId,
    string Confidence,
    string DisplayName,
    string PluginName,
    string ModName,
    string ModuleFilename,
    string Explanation,
    int FamilyCount,
    int Score,
    bool CrossValidated,
    bool HasConflict,
    IReadOnlyList<string> SupportingFamilies,
    IReadOnlyList<string> ConflictingFamilies);
public sealed record TriageReview
{
    public const string UnreviewedStatus = "unreviewed";

    public string ReviewStatus { get; init; } = UnreviewedStatus;
    public bool Reviewed { get; init; }
    public string Verdict { get; init; } = string.Empty;
    public string ActualCause { get; init; } = string.Empty;
    public string GroundTruthCause { get; init; } = string.Empty;
    public string GroundTruthMod { get; init; } = string.Empty;
    public bool SignatureMatched { get; init; }
    public string Reviewer { get; init; } = string.Empty;
    public string ReviewedAtUtc { get; init; } = string.Empty;
    public string Notes { get; init; } = string.Empty;
}
public sealed record CrashLoggerRefItem(
    string EspName, string ObjectType, string Location,
    string ObjectName, string FormId, int RefCount, int RelevanceScore);
