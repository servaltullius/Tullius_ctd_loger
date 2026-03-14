using System.Linq;

namespace SkyrimDiagDumpToolWinUI;

internal sealed partial class MainWindowViewModel
{
    public string? BuildSummaryClipboardText()
    {
        var summary = CurrentSummary;
        if (summary is null)
        {
            return null;
        }

        var lines = new List<string>
        {
            _isKorean ? "SkyrimDiag 리포트" : "SkyrimDiag report",
        };

        if (!string.IsNullOrWhiteSpace(CurrentDumpPath))
        {
            lines.Add((_isKorean ? "덤프: " : "Dump: ") + CurrentDumpPath);
        }

        if (!string.IsNullOrWhiteSpace(summary.SummarySentence))
        {
            lines.Add((_isKorean ? "결론: " : "Conclusion: ") + summary.SummarySentence);
        }

        if (!string.IsNullOrWhiteSpace(summary.CrashBucketKey))
        {
            lines.Add((_isKorean ? "크래시 버킷 키: " : "Crash bucket key: ") + summary.CrashBucketKey);
        }

        if (!string.IsNullOrWhiteSpace(summary.ModulePlusOffset))
        {
            lines.Add((_isKorean ? "Module+Offset: " : "Module+Offset: ") + summary.ModulePlusOffset);
        }

        if (!string.IsNullOrWhiteSpace(summary.InferredModName))
        {
            lines.Add((_isKorean ? "추정 모드: " : "Inferred mod: ") + summary.InferredModName);
        }

        if (summary.ActionableCandidates.Count > 0)
        {
            var primaryCandidate = summary.ActionableCandidates[0];
            lines.Add((_isKorean ? "행동 우선 후보: " : "Actionable candidate: ") + BuildPrimaryCandidateValue(summary));
            lines.Add((_isKorean ? "근거 합의: " : "Evidence agreement: ") + BuildAgreementSummary(summary));
            if ((primaryCandidate.StatusId == "conflicting" || primaryCandidate.HasConflict) &&
                summary.ActionableCandidates.Count > 1)
            {
                lines.Add((_isKorean ? "충돌 세부: " : "Conflict detail: ") + BuildConflictCandidateLine(primaryCandidate));
                lines.Add((_isKorean ? "충돌 세부: " : "Conflict detail: ") + BuildConflictCandidateLine(summary.ActionableCandidates[1]));
            }
        }

        if (summary.CrashLoggerRefs.Count > 0)
        {
            var espDescs = string.Join(", ", summary.CrashLoggerRefs.Select(r =>
                !string.IsNullOrWhiteSpace(r.FormId) ? $"{r.EspName} [{r.FormId}]" : r.EspName));
            lines.Add((_isKorean ? "CrashLogger 참조 모드: " : "CrashLogger referenced mods: ") + espDescs);
        }

        return string.Join(Environment.NewLine, lines);
    }

    public string? BuildCommunityShareText()
    {
        var summary = CurrentSummary;
        if (summary is null)
        {
            return null;
        }

        var lines = new List<string>();

        static bool HasAnyPrefix(IEnumerable<string> values, params string[] prefixes)
        {
            foreach (var v in values)
            {
                if (string.IsNullOrWhiteSpace(v))
                {
                    continue;
                }

                foreach (var p in prefixes)
                {
                    if (v.StartsWith(p, StringComparison.OrdinalIgnoreCase))
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        var recs = summary.Recommendations;
        var looksSnapshotByText = summary.SummarySentence.Contains("snapshot", StringComparison.OrdinalIgnoreCase) ||
                                  summary.SummarySentence.Contains("스냅샷", StringComparison.Ordinal);
        var looksHangByText = summary.SummarySentence.Contains("freeze", StringComparison.OrdinalIgnoreCase) ||
                              summary.SummarySentence.Contains("infinite loading", StringComparison.OrdinalIgnoreCase) ||
                              summary.SummarySentence.Contains("프리징", StringComparison.Ordinal) ||
                              summary.SummarySentence.Contains("무한로딩", StringComparison.Ordinal);

        var isSnapshotLike = summary.IsSnapshotLike ||
                             looksSnapshotByText ||
                             HasAnyPrefix(recs, "[Snapshot]", "[정상/스냅샷]", "[Manual]", "[수동]");
        var isHangLike = !isSnapshotLike &&
                         (summary.IsHangLike || looksHangByText || HasAnyPrefix(recs, "[Hang]", "[프리징]"));
        lines.Add(isSnapshotLike
            ? (_isKorean ? "🟡 Skyrim 상태 스냅샷 리포트 — SkyrimDiag" : "🟡 Skyrim Snapshot Report — SkyrimDiag")
            : isHangLike
                ? (_isKorean ? "🟠 Skyrim 프리징/무한로딩 리포트 — SkyrimDiag" : "🟠 Skyrim Freeze/ILS Report — SkyrimDiag")
                : (_isKorean ? "🔴 Skyrim CTD 리포트 — SkyrimDiag" : "🔴 Skyrim CTD Report — SkyrimDiag"));

        if (summary.ActionableCandidates.Count > 0)
        {
            var primaryCandidate = summary.ActionableCandidates[0];
            lines.Add($"📌 {DescribeActionableCandidateLabel(primaryCandidate)}: {BuildPrimaryCandidateValue(summary)}");
            lines.Add($"🧭 {(_isKorean ? "근거 합의" : "Evidence agreement")}: {BuildAgreementSummary(summary)}");
            if ((primaryCandidate.StatusId == "conflicting" || primaryCandidate.HasConflict) &&
                summary.ActionableCandidates.Count > 1)
            {
                lines.Add($"⚖️ {BuildConflictCandidateLine(primaryCandidate)}");
                lines.Add($"⚖️ {BuildConflictCandidateLine(summary.ActionableCandidates[1])}");
            }
        }
        else if (summary.CrashLoggerRefs.Count > 0)
        {
            var topEspRef = summary.CrashLoggerRefs[0];
            var espLabel = !string.IsNullOrWhiteSpace(topEspRef.FormId)
                ? $"{topEspRef.EspName} [{topEspRef.FormId}]"
                : topEspRef.EspName;
            lines.Add($"📌 {(_isKorean ? "참조 모드 (ESP)" : "Referenced mod (ESP)")}: {espLabel}");
            if (summary.Suspects.Count > 0)
            {
                var topSuspect = summary.Suspects[0];
                var conf = !string.IsNullOrWhiteSpace(topSuspect.Confidence) ? topSuspect.Confidence : "?";
                lines.Add($"🔧 {(_isKorean ? "DLL 후보" : "DLL suspect")}: {topSuspect.Module} ({conf})");
            }
        }
        else if (summary.Suspects.Count > 0)
        {
            var top = summary.Suspects[0];
            var conf = !string.IsNullOrWhiteSpace(top.Confidence) ? top.Confidence : "?";
            lines.Add($"📌 {(_isKorean ? (isSnapshotLike ? "참고 후보" : "유력 원인") : (isSnapshotLike ? "Reference candidate" : "Primary suspect"))}: {top.Module} ({conf})");
        }

        if (!string.IsNullOrWhiteSpace(summary.CrashBucketKey))
        {
            var typeLabel = isSnapshotLike
                ? (_isKorean ? "분류" : "Category")
                : (_isKorean ? "유형" : "Type");
            var typeValue = isSnapshotLike
                ? "SNAPSHOT"
                : isHangLike
                    ? "HANG"
                    : summary.CrashBucketKey;
            lines.Add($"🔍 {typeLabel}: {typeValue}");
        }

        if (!string.IsNullOrWhiteSpace(summary.ModulePlusOffset))
        {
            lines.Add($"📍 Module+Offset: {summary.ModulePlusOffset}");
        }

        if (!string.IsNullOrWhiteSpace(summary.SummarySentence))
        {
            lines.Add($"💡 {(_isKorean ? "결론" : "Conclusion")}: {summary.SummarySentence}");
        }

        if (summary.Recommendations.Count > 0)
        {
            string firstAction;
            if (isSnapshotLike)
            {
                firstAction = summary.Recommendations.FirstOrDefault(r =>
                    r.StartsWith("[Snapshot]", StringComparison.OrdinalIgnoreCase) ||
                    r.StartsWith("[정상/스냅샷]", StringComparison.Ordinal) ||
                    r.StartsWith("[Manual]", StringComparison.OrdinalIgnoreCase) ||
                    r.StartsWith("[수동]", StringComparison.Ordinal)) ?? summary.Recommendations[0];
            }
            else if (isHangLike)
            {
                firstAction = summary.Recommendations.FirstOrDefault(r =>
                    r.StartsWith("[Hang]", StringComparison.OrdinalIgnoreCase) ||
                    r.StartsWith("[프리징]", StringComparison.Ordinal)) ?? summary.Recommendations[0];
            }
            else
            {
                firstAction = summary.Recommendations[0];
            }
            lines.Add($"🛠️ {(_isKorean ? "권장" : "Action")}: {StripRecommendationTag(firstAction)}");
        }

        lines.Add("— Tullius CTD Logger");

        return string.Join(Environment.NewLine, lines);
    }
}
