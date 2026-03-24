using System.Linq;

namespace SkyrimDiagDumpToolWinUI;

internal sealed partial class MainWindowViewModel
{
    private void PopulateConflictComparison(AnalysisSummary summary)
    {
        ConflictComparisonRows.Clear();
        foreach (var row in BuildConflictComparisonRows(summary))
        {
            ConflictComparisonRows.Add(row);
        }
    }

    private IReadOnlyList<ConflictComparisonRow> BuildConflictComparisonRows(AnalysisSummary summary)
    {
        var rows = new List<ConflictComparisonRow>();
        if (summary.ActionableCandidates.Count < 2)
        {
            return rows;
        }

        var first = summary.ActionableCandidates[0];
        if (first.StatusId != "conflicting" && !first.HasConflict)
        {
            return rows;
        }

        foreach (var candidate in summary.ActionableCandidates.Take(2))
        {
            rows.Add(new ConflictComparisonRow(
                BuildCandidateDisplayName(candidate),
                BuildFamilySummary(candidate),
                BuildConflictComparisonDetail(candidate)));
        }
        return rows;
    }

    private string DescribeActionableCandidateLabel(ActionableCandidateItem candidate) => candidate.StatusId switch
    {
        "cross_validated" => T("Cross-validated candidate", "교차검증된 후보"),
        "conflicting" => T("Conflicting candidates", "복수 후보"),
        "reference_clue" => T("Processing at crash", "사고 당시 처리 대상"),
        _ => T("Actionable candidate", "행동 우선 후보"),
    };

    private string DescribeEvidenceAgreement(ActionableCandidateItem candidate)
    {
        if (candidate.StatusId == "conflicting" || candidate.HasConflict)
        {
            return T("Signals disagree", "신호 충돌");
        }

        if (HasFamily(candidate, "crash_logger_frame") && HasFamily(candidate, "first_chance_context"))
        {
            return T("Crash Logger frame + first-chance", "Crash Logger frame + first-chance");
        }

        if (HasFamily(candidate, "crash_logger_frame") && HasFamily(candidate, "history_repeat"))
        {
            return T("Crash Logger frame + history", "Crash Logger frame + history");
        }

        if (HasFamily(candidate, "crash_logger_frame") && candidate.FamilyCount <= 1)
        {
            return T("Crash Logger frame first", "Crash Logger frame 우선");
        }

        if (candidate.StatusId == "reference_clue")
        {
            return T("Object ref only", "오브젝트 참조 단독");
        }

        if (candidate.FamilyCount > 0)
        {
            return _isKorean
                ? $"{candidate.FamilyCount}개 계열 합의"
                : $"{candidate.FamilyCount} signal families agree";
        }

        return string.IsNullOrWhiteSpace(candidate.Confidence)
            ? T("Limited evidence", "제한된 근거")
            : candidate.Confidence;
    }

    private string BuildAgreementSummary(AnalysisSummary summary)
    {
        var primaryCandidate = summary.ActionableCandidates[0];
        if ((primaryCandidate.StatusId == "conflicting" || primaryCandidate.HasConflict) &&
            summary.ActionableCandidates.Count > 1)
        {
            return BuildConflictAgreementSummary(summary.ActionableCandidates[0], summary.ActionableCandidates[1]);
        }

        return DescribeEvidenceAgreement(primaryCandidate);
    }

    private string BuildConflictAgreementSummary(ActionableCandidateItem first, ActionableCandidateItem second)
    {
        return _isKorean
            ? $"{BuildFamilySummary(first)} vs {BuildFamilySummary(second)}"
            : $"{BuildFamilySummary(first)} vs {BuildFamilySummary(second)}";
    }

    private string BuildConflictCandidateLine(ActionableCandidateItem candidate)
    {
        return $"{BuildCandidateDisplayName(candidate)} <- {BuildFamilySummary(candidate)}";
    }

    private string BuildCrashLoggerContextLabel(AnalysisSummary summary)
    {
        if (HasCrashLoggerFrameSignal(summary))
        {
            return T("Crash Logger frame", "Crash Logger 프레임");
        }

        if (summary.CrashLoggerRefs.Count > 0)
        {
            if (summary.ActionableCandidates.Count > 0 &&
                summary.ActionableCandidates[0].StatusId == "reference_clue")
            {
                return T("Processing at crash", "사고 당시 처리 대상");
            }

            return T("CrashLogger reference", "CrashLogger 참조");
        }

        return T("Actionable candidate", "행동 우선 후보");
    }

    private string BuildCrashLoggerContextSummary(AnalysisSummary summary)
    {
        const string dllGuidance = "DLL guidance";

        if (!string.IsNullOrWhiteSpace(summary.CrashLoggerDirectFaultModule))
        {
            return _isKorean
                ? $"{summary.CrashLoggerDirectFaultModule} — Crash Logger frame first (direct DLL fault) — {dllGuidance}"
                : $"{summary.CrashLoggerDirectFaultModule} — Crash Logger frame first (direct DLL fault) — {dllGuidance}";
        }

        if (!string.IsNullOrWhiteSpace(summary.CrashLoggerFirstActionableProbableModule))
        {
            return _isKorean
                ? $"{summary.CrashLoggerFirstActionableProbableModule} — Crash Logger frame first probable DLL frame — {dllGuidance}"
                : $"{summary.CrashLoggerFirstActionableProbableModule} — Crash Logger frame first probable DLL frame — {dllGuidance}";
        }

        if (!string.IsNullOrWhiteSpace(summary.CrashLoggerProbableStreakModule) &&
            summary.CrashLoggerProbableStreakLength > 0)
        {
            return _isKorean
                ? $"{summary.CrashLoggerProbableStreakModule} — Crash Logger frame first probable frame streak x{summary.CrashLoggerProbableStreakLength} — {dllGuidance}"
                : $"{summary.CrashLoggerProbableStreakModule} — Crash Logger frame first probable frame streak x{summary.CrashLoggerProbableStreakLength} — {dllGuidance}";
        }

        if (summary.CrashLoggerRefs.Count > 0)
        {
            var topRef = summary.CrashLoggerRefs[0];
            var espName = FirstNonEmpty(
                topRef.EspName,
                summary.InferredModName,
                T("CrashLogger object ref", "CrashLogger 오브젝트 참조"));
            var anchor = !string.IsNullOrWhiteSpace(topRef.FormId)
                ? $"{espName} [{topRef.FormId}]"
                : espName;

            var details = new List<string>();
            if (!string.IsNullOrWhiteSpace(topRef.ObjectName))
            {
                details.Add($"\"{topRef.ObjectName}\"");
            }
            else if (!string.IsNullOrWhiteSpace(topRef.ObjectType))
            {
                details.Add(topRef.ObjectType);
            }

            if (!string.IsNullOrWhiteSpace(topRef.Location))
            {
                details.Add(topRef.Location);
            }

            return details.Count == 0
                ? anchor
                : $"{anchor} — {string.Join(" — ", details.Take(2))}";
        }

        if (summary.ActionableCandidates.Count > 0)
        {
            return BuildPrimaryCandidateValue(summary);
        }

        if (!string.IsNullOrWhiteSpace(summary.InferredModName))
        {
            return summary.InferredModName;
        }

        return T("No CrashLogger object ref was extracted.", "CrashLogger 오브젝트 참조를 추출하지 못했습니다.");
    }

    private string BuildCrashContextSummary(AnalysisSummary summary)
    {
        if (!string.IsNullOrWhiteSpace(summary.ModulePlusOffset) || !string.IsNullOrWhiteSpace(summary.CrashBucketKey))
        {
            return T(
                "Fault module and crash bucket stay here as context, not first blame.",
                "오류 모듈과 크래시 버킷은 1차 범인 지목이 아니라 참고용 컨텍스트입니다.");
        }

        return T(
            "This capture has limited crash-context details.",
            "이번 캡처에는 크래시 컨텍스트 정보가 제한적입니다.");
    }

    private string BuildFamilySummary(ActionableCandidateItem candidate)
    {
        var families = candidate.SupportingFamilies
            .Select(DescribeFamily)
            .Where(family => !string.IsNullOrWhiteSpace(family))
            .Take(2)
            .ToList();
        if (families.Count == 0)
        {
            return T("limited evidence", "제한된 근거");
        }
        return string.Join(" + ", families);
    }

    private string BuildConflictComparisonDetail(ActionableCandidateItem candidate)
    {
        if (!string.IsNullOrWhiteSpace(candidate.Explanation))
        {
            return candidate.Explanation;
        }

        return BuildCandidateReason(candidate);
    }

    private string BuildPrimaryCandidateValue(AnalysisSummary summary)
    {
        var primaryCandidate = summary.ActionableCandidates[0];
        if ((primaryCandidate.StatusId == "conflicting" || primaryCandidate.HasConflict) &&
            summary.ActionableCandidates.Count > 1)
        {
            return $"{BuildCandidateDisplayName(primaryCandidate)} / {BuildCandidateDisplayName(summary.ActionableCandidates[1])}";
        }

        return BuildCandidateDisplayName(primaryCandidate);
    }

    private string BuildCandidateDisplayName(ActionableCandidateItem candidate)
    {
        return FirstNonEmpty(
            candidate.PrimaryIdentifier,
            candidate.DisplayName,
            candidate.PluginName,
            candidate.ModName,
            candidate.ModuleFilename);
    }

    private string BuildCandidateReason(ActionableCandidateItem candidate)
    {
        var families = candidate.SupportingFamilies
            .Select(DescribeFamily)
            .Where(family => !string.IsNullOrWhiteSpace(family))
            .ToList();

        var parts = new List<string>();
        if (families.Count > 0)
        {
            parts.Add(string.Join(" + ", families));
        }

        if (!string.IsNullOrWhiteSpace(candidate.SecondaryLabel))
        {
            parts.Add(_isKorean
                ? $"표시명: {candidate.SecondaryLabel}"
                : $"Label: {candidate.SecondaryLabel}");
        }

        if (!string.IsNullOrWhiteSpace(candidate.Explanation))
        {
            parts.Add(candidate.Explanation);
        }

        if ((candidate.StatusId == "conflicting" || candidate.HasConflict) && candidate.ConflictingFamilies.Count > 0)
        {
            var conflicts = string.Join(", ", candidate.ConflictingFamilies.Select(DescribeFamily));
            parts.Add(_isKorean ? $"충돌 신호: {conflicts}" : $"Conflicting signal: {conflicts}");
        }

        return parts.Count == 0
            ? T("No consensus detail available.", "합의 세부 정보가 없습니다.")
            : string.Join(" — ", parts);
    }

    private string DescribeFamily(string familyId) => familyId switch
    {
        "crash_logger_frame" => T("Crash Logger frame", "Crash Logger 프레임"),
        "crash_logger_object_ref" => T("CrashLogger object ref", "CrashLogger 오브젝트 참조"),
        "actionable_stack" => T("actionable stack", "실행 가능한 스택"),
        "resource_provider" => T("near resource provider", "인접 리소스 provider"),
        "history_repeat" => T("history repeat", "반복 기록"),
        _ => familyId,
    };

    public string BuildEspRefReason(CrashLoggerRefItem espRef)
    {
        var parts = new List<string>();
        if (!string.IsNullOrWhiteSpace(espRef.ObjectType))
            parts.Add(espRef.ObjectType);
        if (!string.IsNullOrWhiteSpace(espRef.FormId))
            parts.Add($"[{espRef.FormId}]");
        if (!string.IsNullOrWhiteSpace(espRef.ObjectName))
            parts.Add($"\"{espRef.ObjectName}\"");
        if (!string.IsNullOrWhiteSpace(espRef.Location))
            parts.Add(_isKorean ? $"{espRef.Location}에서 발견" : $"found in {espRef.Location}");
        if (espRef.RefCount > 1)
            parts.Add(_isKorean ? $"참조 {espRef.RefCount}건" : $"{espRef.RefCount} refs");
        return parts.Count == 0
            ? T("ESP/ESM object reference", "ESP/ESM 오브젝트 참조")
            : string.Join(" — ", parts);
    }

    public string MapRelevanceToConfidence(int score)
    {
        if (score >= 16) return T("ESP ref (high)", "ESP 참조 (높음)");
        if (score >= 10) return T("ESP ref", "ESP 참조");
        return T("ESP ref (low)", "ESP 참조 (낮음)");
    }

    private static bool HasFamily(ActionableCandidateItem candidate, string familyId)
    {
        return candidate.SupportingFamilies.Contains(familyId, StringComparer.Ordinal);
    }

    private bool HasCrashLoggerFrameSignal(AnalysisSummary summary)
    {
        if (summary.CrashLoggerFrameSignalStrength > 0)
        {
            return true;
        }

        if (!string.IsNullOrWhiteSpace(summary.CrashLoggerDirectFaultModule) ||
            !string.IsNullOrWhiteSpace(summary.CrashLoggerFirstActionableProbableModule) ||
            !string.IsNullOrWhiteSpace(summary.CrashLoggerProbableStreakModule))
        {
            return true;
        }

        if (summary.ActionableCandidates.Count == 0)
        {
            return false;
        }

        var topCandidate = summary.ActionableCandidates[0];
        return HasFamily(topCandidate, "crash_logger_frame");
    }
}
