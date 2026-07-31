using System.Diagnostics;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace SkyrimDiagDumpToolWinUI;

internal static class SummaryTriageStore
{
    public static Task SaveAsync(
        string summaryPath,
        string dumpPath,
        string outDir,
        TriageReview review,
        CancellationToken cancellationToken)
    {
        return SaveAsync(
            summaryPath,
            dumpPath,
            outDir,
            review,
            cancellationToken,
            AtomicFile.WriteAllTextAsync);
    }

    internal static async Task SaveAsync(
        string summaryPath,
        string dumpPath,
        string outDir,
        TriageReview review,
        CancellationToken cancellationToken,
        Func<string, string, CancellationToken, Task> writeSummaryMirrorAsync)
    {
        ArgumentNullException.ThrowIfNull(writeSummaryMirrorAsync);

        await using var outputFamilyLock = await AcquireOutputFamilyLockAsync(
            outDir,
            dumpPath,
            cancellationToken);
        var jsonText = await File.ReadAllTextAsync(summaryPath, cancellationToken);
        using var identityDocument = JsonDocument.Parse(jsonText);
        var identity = DumpIdentityContract.FromJson(identityDocument.RootElement);
        if (!await identity.MatchesFileAsync(dumpPath, cancellationToken))
        {
            throw new InvalidDataException(
                "The summary belongs to a different dump. Reanalyze this dump before saving review feedback.");
        }

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
        var stateNode = new JsonObject
        {
            ["schema"] = "skydiag.triage_state.v1",
            ["dump_identity"] = identity.ToJsonObject(),
            ["triage"] = triageNode.DeepClone(),
        };
        var statePath = identity.ResolveTriageStatePath(outDir);
        var previousStateExists = File.Exists(statePath);
        var previousStateText = previousStateExists
            ? await File.ReadAllTextAsync(statePath, cancellationToken)
            : null;
        var stateText = stateNode.ToJsonString(options) + Environment.NewLine;

        // The identity-bound state remains the analyzer's load authority. The
        // UI reports the operation as successful only after its summary mirror
        // is refreshed, so compensate the authoritative write if that mirror
        // fails instead of leaving a save that the UI reports as rejected.
        await AtomicFile.WriteAllTextAsync(
            statePath,
            stateText,
            cancellationToken);
        try
        {
            await writeSummaryMirrorAsync(
                summaryPath,
                rootNode.ToJsonString(options) + Environment.NewLine,
                cancellationToken);
        }
        catch (Exception mirrorException)
        {
            try
            {
                await RestoreAuthoritativeStateAsync(
                    statePath,
                    stateText,
                    previousStateExists,
                    previousStateText);
            }
            catch (Exception rollbackException)
            {
                throw new IOException(
                    "The summary JSON mirror could not be updated, and the authoritative " +
                    "triage state could not be rolled back. The review may already be saved; " +
                    "reanalyze the dump before editing review feedback again.",
                    new AggregateException(mirrorException, rollbackException));
            }
            throw;
        }
    }

    private static async Task RestoreAuthoritativeStateAsync(
        string statePath,
        string committedStateText,
        bool previousStateExists,
        string? previousStateText)
    {
        if (File.Exists(statePath))
        {
            var currentStateText = await File.ReadAllTextAsync(
                statePath,
                CancellationToken.None);
            if (!string.Equals(
                    currentStateText,
                    committedStateText,
                    StringComparison.Ordinal))
            {
                throw new IOException(
                    "The authoritative triage state changed concurrently; rollback was not attempted.");
            }
        }
        else if (!previousStateExists)
        {
            return;
        }

        if (previousStateExists)
        {
            await AtomicFile.WriteAllTextAsync(
                statePath,
                previousStateText
                    ?? throw new InvalidDataException("The previous triage state snapshot is missing."),
                CancellationToken.None);
            return;
        }

        File.Delete(statePath);
    }

    private static async Task<FileStream> AcquireOutputFamilyLockAsync(
        string outDir,
        string dumpPath,
        CancellationToken cancellationToken)
    {
        var lockDirectory = Path.Combine(outDir, ".skydiag-locks");
        Directory.CreateDirectory(lockDirectory);
        var dumpStem = Path.GetFileNameWithoutExtension(dumpPath);
        if (string.IsNullOrWhiteSpace(dumpStem))
        {
            throw new InvalidDataException("The dump path has no filename stem.");
        }

        var lockPath = Path.Combine(lockDirectory, dumpStem + ".lock");
        var elapsed = Stopwatch.StartNew();
        IOException? lastContention = null;
        while (elapsed.Elapsed < TimeSpan.FromSeconds(30))
        {
            cancellationToken.ThrowIfCancellationRequested();
            try
            {
                return new FileStream(
                    lockPath,
                    FileMode.OpenOrCreate,
                    FileAccess.ReadWrite,
                    FileShare.None,
                    1,
                    FileOptions.Asynchronous);
            }
            catch (IOException ex) when (IsLockContention(ex))
            {
                lastContention = ex;
                await Task.Delay(TimeSpan.FromMilliseconds(25), cancellationToken);
            }
        }

        throw new IOException(
            "Timed out waiting for another analysis to publish this dump output family.",
            lastContention);
    }

    private static bool IsLockContention(IOException exception)
    {
        var windowsError = exception.HResult & 0xffff;
        return windowsError is 32 or 33;
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
