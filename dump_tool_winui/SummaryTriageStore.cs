using System.Text.Json;
using System.Text.Json.Nodes;

namespace SkyrimDiagDumpToolWinUI;

internal static class SummaryTriageStore
{
    public static async Task SaveAsync(string summaryPath, TriageReview review, CancellationToken cancellationToken)
    {
        var jsonText = await File.ReadAllTextAsync(summaryPath, cancellationToken);
        var rootNode = JsonNode.Parse(jsonText) as JsonObject
            ?? throw new InvalidDataException("Summary JSON root must be an object.");

        var triageNode = rootNode["triage"] as JsonObject ?? new JsonObject();
        rootNode["triage"] = triageNode;

        var normalizedStatus = NormalizeReviewStatus(review.ReviewStatus);
        var reviewed = IsReviewed(review);
        if (normalizedStatus == TriageReview.UnreviewedStatus && reviewed)
        {
            normalizedStatus = "reviewed";
        }

        triageNode["review_status"] = normalizedStatus;
        triageNode["reviewed"] = reviewed;
        triageNode["verdict"] = review.Verdict.Trim();
        triageNode["actual_cause"] = review.ActualCause.Trim();
        triageNode["ground_truth_mod"] = review.GroundTruthMod.Trim();
        triageNode["notes"] = review.Notes.Trim();
        triageNode["reviewed_at_utc"] = reviewed
            ? DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
            : string.Empty;

        var options = new JsonSerializerOptions
        {
            WriteIndented = true,
        };
        await File.WriteAllTextAsync(
            summaryPath,
            rootNode.ToJsonString(options) + Environment.NewLine,
            cancellationToken);
    }

    public static bool HasReviewContent(TriageReview review)
    {
        return !string.IsNullOrWhiteSpace(review.GroundTruthMod) ||
               !string.IsNullOrWhiteSpace(review.ActualCause) ||
               !string.IsNullOrWhiteSpace(review.Verdict) ||
               !string.IsNullOrWhiteSpace(review.Notes);
    }

    public static bool IsReviewed(TriageReview review)
    {
        return NormalizeReviewStatus(review.ReviewStatus) != TriageReview.UnreviewedStatus ||
               review.Reviewed ||
               HasReviewContent(review);
    }

    public static string NormalizeReviewStatus(string? reviewStatus)
    {
        var normalized = (reviewStatus ?? string.Empty).Trim().ToLowerInvariant();
        return normalized switch
        {
            "reviewed" => "reviewed",
            "confirmed" => "confirmed",
            "triaged" => "triaged",
            "done" => "done",
            _ => TriageReview.UnreviewedStatus,
        };
    }
}
