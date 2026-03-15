namespace SkyrimDiagDumpToolWinUI;

internal sealed partial class MainWindowViewModel
{
    private void PopulateRecommendations(AnalysisSummary summary)
    {
        Recommendations.Clear();
        foreach (var recommendation in summary.Recommendations.Take(12))
        {
            Recommendations.Add(recommendation);
        }
        if (Recommendations.Count == 0)
        {
            Recommendations.Add(T("No recommendation text was generated.", "권장 조치 문구가 생성되지 않았습니다."));
        }
        PopulateRecommendationGroups();
        QuickActionsValue = BuildNextActionSummary(summary);
    }

    private void PopulateRecommendationGroups()
    {
        RecommendationGroups.Clear();
        ImmediateRecommendations.Clear();
        VerificationRecommendations.Clear();
        RecaptureRecommendations.Clear();

        var immediate = new List<string>();
        var verification = new List<string>();
        var recapture = new List<string>();

        foreach (var recommendation in Recommendations)
        {
            var plain = StripRecommendationTag(recommendation);
            if (string.IsNullOrWhiteSpace(plain))
            {
                continue;
            }

            if (IsRecaptureRecommendation(recommendation))
            {
                recapture.Add(plain);
            }
            else if (IsImmediateActionRecommendation(recommendation))
            {
                immediate.Add(plain);
            }
            else
            {
                verification.Add(plain);
            }
        }

        var groups = BuildRecommendationGroups(immediate, verification, recapture);
        foreach (var group in groups)
        {
            RecommendationGroups.Add(group);
        }

        foreach (var item in groups[0].Items)
        {
            ImmediateRecommendations.Add(item);
        }
        foreach (var item in groups[1].Items)
        {
            VerificationRecommendations.Add(item);
        }
        foreach (var item in groups[2].Items)
        {
            RecaptureRecommendations.Add(item);
        }
    }

    private void PopulateRecaptureContext(AnalysisSummary summary)
    {
        if (!summary.HasRecaptureEvaluation || !summary.RecaptureTriggered)
        {
            ShowRecaptureContext = false;
            RecaptureContextTitle = string.Empty;
            RecaptureContextDetails = string.Empty;
            return;
        }

        ShowRecaptureContext = true;
        RecaptureContextTitle = T("Recapture context", "재수집 문맥");

        var parts = new List<string>();
        if (!string.IsNullOrWhiteSpace(summary.RecaptureTargetProfile))
        {
            parts.Add(T("target_profile=", "target_profile=") + summary.RecaptureTargetProfile);
        }

        if (summary.RecaptureEscalationLevel > 0)
        {
            parts.Add(T("escalation_level=", "escalation_level=") + summary.RecaptureEscalationLevel);
        }

        if (summary.RecaptureReasons.Count > 0)
        {
            parts.Add(T("reasons=", "reasons=") + string.Join(", ", summary.RecaptureReasons));
        }

        if (!string.IsNullOrWhiteSpace(summary.RecaptureKind))
        {
            parts.Add(T("kind=", "kind=") + summary.RecaptureKind);
        }

        RecaptureContextDetails = parts.Count == 0
            ? T("Additional capture context is available.", "추가 캡처 문맥이 있습니다.")
            : string.Join(" | ", parts);
    }

    private IReadOnlyList<RecommendationGroupItem> BuildRecommendationGroups(
        IReadOnlyList<string> immediate,
        IReadOnlyList<string> verification,
        IReadOnlyList<string> recapture)
    {
        return new[]
        {
            new RecommendationGroupItem(T("Do This Now", "지금 바로"), immediate),
            new RecommendationGroupItem(T("Verify Next", "추가 확인"), verification),
            new RecommendationGroupItem(T("Recapture or Compare", "재수집 / 비교"), recapture),
        };
    }

    private string BuildNextActionSummary(AnalysisSummary summary)
    {
        var taggedAction = summary.Recommendations.FirstOrDefault(IsPriorityActionRecommendation);
        if (!string.IsNullOrWhiteSpace(taggedAction))
        {
            return StripRecommendationTag(taggedAction);
        }

        if (summary.ActionableCandidates.Count > 0)
        {
            var primaryCandidate = summary.ActionableCandidates[0];
            var candidateName = BuildPrimaryCandidateValue(summary);
            return primaryCandidate.StatusId switch
            {
                "cross_validated" => T($"Update or isolate {candidateName} first", $"{candidateName} 업데이트/격리부터 확인"),
                "related" => T($"Check {candidateName} before generic DLL triage", $"{candidateName} 쪽을 일반 DLL 점검보다 먼저 확인"),
                "reference_clue" => T("Capture another dump or compare a second signal", "다른 덤프를 한 번 더 모으거나 두 번째 신호를 비교"),
                "conflicting" => T($"Compare {candidateName} one side at a time", $"{candidateName} 후보를 한쪽씩 나눠 비교"),
                _ => T("Review the first recommendation", "첫 번째 권장 조치를 확인"),
            };
        }

        var firstRecommendation = summary.Recommendations.FirstOrDefault();
        return string.IsNullOrWhiteSpace(firstRecommendation)
            ? T("None", "없음")
            : StripRecommendationTag(firstRecommendation);
    }

    private static bool IsPriorityActionRecommendation(string recommendation)
    {
        return recommendation.StartsWith("[Actionable candidate]", StringComparison.OrdinalIgnoreCase) ||
               recommendation.StartsWith("[행동 우선 후보]", StringComparison.Ordinal) ||
               recommendation.StartsWith("[Object ref]", StringComparison.OrdinalIgnoreCase) ||
               recommendation.StartsWith("[오브젝트 참조]", StringComparison.Ordinal) ||
               recommendation.StartsWith("[Conflict]", StringComparison.OrdinalIgnoreCase) ||
               recommendation.StartsWith("[충돌]", StringComparison.Ordinal);
    }

    private static string StripRecommendationTag(string recommendation)
    {
        if (string.IsNullOrWhiteSpace(recommendation))
        {
            return string.Empty;
        }

        if (recommendation.StartsWith("[", StringComparison.Ordinal))
        {
            var end = recommendation.IndexOf(']');
            if (end >= 0 && end + 1 < recommendation.Length)
            {
                return recommendation[(end + 1)..].TrimStart();
            }
        }

        return recommendation.Trim();
    }

    private static bool IsImmediateActionRecommendation(string recommendation)
    {
        return recommendation.StartsWith("[Actionable candidate]", StringComparison.OrdinalIgnoreCase) ||
               recommendation.StartsWith("[행동 우선 후보]", StringComparison.Ordinal) ||
               recommendation.StartsWith("[Top suspect]", StringComparison.OrdinalIgnoreCase) ||
               recommendation.StartsWith("[유력 후보]", StringComparison.Ordinal) ||
               recommendation.StartsWith("[ESP/ESM]", StringComparison.OrdinalIgnoreCase) ||
               recommendation.StartsWith("[Object ref]", StringComparison.OrdinalIgnoreCase) ||
               recommendation.StartsWith("[오브젝트 참조]", StringComparison.Ordinal) ||
               recommendation.StartsWith("[Conflict]", StringComparison.OrdinalIgnoreCase) ||
               recommendation.StartsWith("[충돌]", StringComparison.Ordinal) ||
               recommendation.StartsWith("[Masters]", StringComparison.OrdinalIgnoreCase) ||
               recommendation.StartsWith("[마스터]", StringComparison.Ordinal) ||
               recommendation.StartsWith("[BEES]", StringComparison.OrdinalIgnoreCase) ||
               recommendation.StartsWith("[Check]", StringComparison.OrdinalIgnoreCase) ||
               recommendation.StartsWith("[점검]", StringComparison.Ordinal) ||
               recommendation.StartsWith("[Hook framework]", StringComparison.OrdinalIgnoreCase) ||
               recommendation.StartsWith("[훅 프레임워크]", StringComparison.Ordinal);
    }

    private static bool IsRecaptureRecommendation(string recommendation)
    {
        return recommendation.Contains("DumpMode=2", StringComparison.OrdinalIgnoreCase) ||
               recommendation.Contains("capture another", StringComparison.OrdinalIgnoreCase) ||
               recommendation.Contains("recapture", StringComparison.OrdinalIgnoreCase) ||
               recommendation.Contains("capture taken during", StringComparison.OrdinalIgnoreCase) ||
               recommendation.Contains("second signal", StringComparison.OrdinalIgnoreCase) ||
               recommendation.Contains("break the tie", StringComparison.OrdinalIgnoreCase) ||
               recommendation.Contains("다시 캡처", StringComparison.Ordinal) ||
               recommendation.Contains("재수집", StringComparison.Ordinal) ||
               recommendation.Contains("한 번 더", StringComparison.Ordinal) ||
               recommendation.Contains("두 번째 신호", StringComparison.Ordinal) ||
               recommendation.StartsWith("[Snapshot]", StringComparison.OrdinalIgnoreCase) ||
               recommendation.StartsWith("[정상/스냅샷]", StringComparison.Ordinal) ||
               recommendation.StartsWith("[Manual]", StringComparison.OrdinalIgnoreCase) ||
               recommendation.StartsWith("[수동]", StringComparison.Ordinal);
    }
}
