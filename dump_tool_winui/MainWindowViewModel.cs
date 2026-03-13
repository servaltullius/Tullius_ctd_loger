using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Linq;
using System.Text.Json;

namespace SkyrimDiagDumpToolWinUI;

/// <summary>
/// Presentation logic extracted from MainWindow code-behind.
/// Holds analysis state + ObservableCollections + text building methods.
/// No WinUI/XAML dependency — testable in isolation.
/// </summary>
internal sealed class MainWindowViewModel
{
    private readonly bool _isKorean;

    public MainWindowViewModel(bool isKorean)
    {
        _isKorean = isKorean;
    }

    // ── State ──────────────────────────────────────────────────
    public string? CurrentDumpPath { get; set; }
    public string? CurrentOutDir { get; set; }
    public AnalysisSummary? CurrentSummary { get; private set; }
    public bool IsKorean => _isKorean;

    // ── Observable collections (bound to UI) ──────────────────
    public ObservableCollection<SuspectItem> Suspects { get; } = new();
    public ObservableCollection<string> Recommendations { get; } = new();
    public ObservableCollection<RecommendationGroupItem> RecommendationGroups { get; } = new();
    public ObservableCollection<string> ImmediateRecommendations { get; } = new();
    public ObservableCollection<string> VerificationRecommendations { get; } = new();
    public ObservableCollection<string> RecaptureRecommendations { get; } = new();
    public ObservableCollection<ConflictComparisonRow> ConflictComparisonRows { get; } = new();
    public ObservableCollection<string> CallstackFrames { get; } = new();
    public ObservableCollection<EvidenceViewItem> EvidenceItems { get; } = new();
    public ObservableCollection<ResourceViewItem> ResourceItems { get; } = new();
    public ObservableCollection<string> EventItems { get; } = new();
    public ObservableCollection<DumpDiscoveryItem> RecentDumps { get; } = new();
    public ObservableCollection<DumpSearchLocationItem> DumpSearchLocations { get; } = new();

    // ── Summary fields for UI ─────────────────────────────────
    public string SummarySentence { get; private set; } = string.Empty;
    public string BucketText { get; private set; } = string.Empty;
    public string ModuleText { get; private set; } = string.Empty;
    public string ModNameText { get; private set; } = string.Empty;
    public string CrashLoggerContextSummary { get; private set; } = string.Empty;
    public string CrashContextSummary { get; private set; } = string.Empty;
    public bool ShowCorrelationBadge { get; private set; }
    public string CorrelationBadgeText { get; private set; } = string.Empty;
    public string QuickPrimaryLabel { get; private set; } = string.Empty;
    public string QuickPrimaryValue { get; private set; } = string.Empty;
    public string QuickConfidenceValue { get; private set; } = string.Empty;
    public string QuickActionsValue { get; private set; } = string.Empty;
    public string QuickEventsValue { get; private set; } = string.Empty;
    public string RecentDumpStatusText { get; private set; } = string.Empty;
    public bool ShowRecaptureContext { get; private set; }
    public string RecaptureContextTitle { get; private set; } = string.Empty;
    public string RecaptureContextDetails { get; private set; } = string.Empty;
    public string TroubleshootingTitle { get; private set; } = string.Empty;
    public IReadOnlyList<string> TroubleshootingSteps { get; private set; } = Array.Empty<string>();
    public bool ShowTroubleshooting { get; private set; }
    // ── Populate from analysis result ─────────────────────────

    public void PopulateSummary(AnalysisSummary summary)
    {
        CurrentSummary = summary;
        PopulateHeaderFields(summary);
        PopulateSuspects(summary);
        PopulateRecommendations(summary);
        PopulateRecaptureContext(summary);
        PopulateConflictComparison(summary);
        PopulateTroubleshooting(summary);
        PopulateCallstack(summary);
        PopulateEvidence(summary);
        PopulateResources(summary);
    }

    public void PopulateDumpDiscovery(
        IReadOnlyList<DumpDiscoveryItem> recentDumps,
        IReadOnlyList<DumpSearchLocationItem> dumpSearchLocations,
        string statusText)
    {
        RecentDumps.Clear();
        foreach (var item in recentDumps)
        {
            RecentDumps.Add(item);
        }

        DumpSearchLocations.Clear();
        foreach (var item in dumpSearchLocations)
        {
            DumpSearchLocations.Add(item);
        }

        RecentDumpStatusText = statusText;
    }

    private void PopulateHeaderFields(AnalysisSummary summary)
    {
        SummarySentence = string.IsNullOrWhiteSpace(summary.SummarySentence)
            ? T("No summary sentence produced.", "요약 문장이 생성되지 않았습니다.")
            : summary.SummarySentence;

        BucketText = string.IsNullOrWhiteSpace(summary.CrashBucketKey)
            ? T("Crash bucket: unavailable", "크래시 버킷: 없음")
            : T("Crash bucket: ", "크래시 버킷: ") + summary.CrashBucketKey;

        if (summary.HistoryCorrelationCount > 1)
        {
            CorrelationBadgeText = _isKorean
                ? $"\u26a0 동일 패턴 {summary.HistoryCorrelationCount}회 반복 발생"
                : $"\u26a0 Same pattern repeated {summary.HistoryCorrelationCount} times";
            ShowCorrelationBadge = true;
        }
        else
        {
            ShowCorrelationBadge = false;
        }

        ModuleText = string.IsNullOrWhiteSpace(summary.ModulePlusOffset)
            ? T("Fault module: unavailable", "오류 모듈: 없음")
            : T("Fault module: ", "오류 모듈: ") + summary.ModulePlusOffset;
        CrashLoggerContextSummary = BuildCrashLoggerContextSummary(summary);
        CrashContextSummary = BuildCrashContextSummary(summary);

        if (summary.ActionableCandidates.Count > 0)
        {
            ModNameText = DescribeActionableCandidateLabel(summary.ActionableCandidates[0]) + ": " + BuildPrimaryCandidateValue(summary);
        }
        else if (summary.CrashLoggerRefs.Count > 0 && !string.IsNullOrWhiteSpace(summary.InferredModName))
        {
            ModNameText = T("Referenced mod: ", "참조 모드: ") + summary.InferredModName;
        }
        else
        {
            ModNameText = string.IsNullOrWhiteSpace(summary.InferredModName)
                ? T("Inferred mod: unavailable", "추정 모드: 없음")
                : T("Inferred mod: ", "추정 모드: ") + summary.InferredModName;
        }

        QuickPrimaryLabel = BuildCrashLoggerContextLabel(summary);
        QuickPrimaryValue = CrashLoggerContextSummary;
        if (summary.ActionableCandidates.Count > 0)
        {
            QuickConfidenceValue = BuildAgreementSummary(summary);
        }
        else if (summary.CrashLoggerRefs.Count > 0)
        {
            QuickConfidenceValue = MapRelevanceToConfidence(summary.CrashLoggerRefs[0].RelevanceScore);
        }
        else
        {
            var primarySuspect = summary.Suspects.FirstOrDefault();
            QuickConfidenceValue = primarySuspect is null || string.IsNullOrWhiteSpace(primarySuspect.Confidence)
                ? T("Unrated", "미평가")
                : primarySuspect.Confidence;
        }
    }

    private void PopulateSuspects(AnalysisSummary summary)
    {
        Suspects.Clear();
        if (summary.ActionableCandidates.Count > 0)
        {
            foreach (var candidate in summary.ActionableCandidates.Take(5))
            {
                Suspects.Add(new SuspectItem(
                    DescribeEvidenceAgreement(candidate),
                    BuildCandidateDisplayName(candidate),
                    BuildCandidateReason(candidate)));
            }
        }
        else
        {
            foreach (var espRef in summary.CrashLoggerRefs.Take(3))
            {
                Suspects.Add(new SuspectItem(
                    MapRelevanceToConfidence(espRef.RelevanceScore),
                    espRef.EspName,
                    BuildEspRefReason(espRef)));
            }
            var dllSlots = Math.Max(0, 7 - Suspects.Count);
            foreach (var suspect in summary.Suspects.Take(dllSlots))
            {
                Suspects.Add(suspect);
            }
        }
        if (Suspects.Count == 0)
        {
            Suspects.Add(new SuspectItem(
                T("Unknown", "알 수 없음"),
                T("No strong suspect was extracted.", "강한 원인 후보를 추출하지 못했습니다."),
                T("Try sharing the dump + report for deeper analysis.", "덤프 + 리포트를 공유해 추가 분석을 진행하세요.")));
        }
    }

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

    private void PopulateTroubleshooting(AnalysisSummary summary)
    {
        if (summary.TroubleshootingSteps.Count > 0)
        {
            TroubleshootingTitle = string.IsNullOrWhiteSpace(summary.TroubleshootingTitle)
                ? T("Troubleshooting", "트러블슈팅 가이드")
                : summary.TroubleshootingTitle;
            TroubleshootingSteps = summary.TroubleshootingSteps
                .Select((step, i) => $"{i + 1}. {step}")
                .ToList();
            ShowTroubleshooting = true;
        }
        else
        {
            ShowTroubleshooting = false;
        }
    }

    private void PopulateCallstack(AnalysisSummary summary)
    {
        CallstackFrames.Clear();
        foreach (var frame in summary.CallstackFrames.Take(160))
        {
            CallstackFrames.Add(frame);
        }
        if (CallstackFrames.Count == 0)
        {
            CallstackFrames.Add(T("No callstack frames were extracted.", "콜스택 프레임을 추출하지 못했습니다."));
        }
    }

    private void PopulateEvidence(AnalysisSummary summary)
    {
        EvidenceItems.Clear();
        foreach (var evidence in summary.EvidenceItems.Take(80))
        {
            EvidenceItems.Add(evidence);
        }
        if (EvidenceItems.Count == 0)
        {
            EvidenceItems.Add(new EvidenceViewItem(
                T("Unknown", "알 수 없음"),
                T("No evidence list was generated.", "근거 목록이 생성되지 않았습니다."),
                ""));
        }
    }

    private void PopulateResources(AnalysisSummary summary)
    {
        ResourceItems.Clear();
        foreach (var resource in summary.ResourceItems.Take(120))
        {
            ResourceItems.Add(resource);
        }
        if (ResourceItems.Count == 0)
        {
            ResourceItems.Add(new ResourceViewItem(
                "resource",
                T("No resource traces were found.", "리소스 추적이 없습니다."),
                "-",
                ""));
        }
    }

    public void PopulateAdvancedArtifacts(AdvancedArtifactsData artifacts)
    {
        EventItems.Clear();
        foreach (var line in artifacts.EventLines)
        {
            EventItems.Add(line);
        }
        if (EventItems.Count == 0)
        {
            EventItems.Add(T("No blackbox events were found.", "블랙박스 이벤트를 찾지 못했습니다."));
        }
        QuickEventsValue = artifacts.EventCount == 0
            ? T("0 events", "0개")
            : $"{artifacts.EventCount} {T("events", "개")}";
    }

    // ── Text builders ─────────────────────────────────────────

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

    private string BuildConflictCandidateLine(ActionableCandidateItem candidate)
    {
        return $"{BuildCandidateDisplayName(candidate)} <- {BuildFamilySummary(candidate)}";
    }

    private string BuildCrashLoggerContextLabel(AnalysisSummary summary)
    {
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

    private string DescribeFamily(string familyId) => familyId switch
    {
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

    public DumpToolInvocationOptions BuildInvocationOptions(
        string dumpPath, string? outDirText, string? startupLanguage, bool headless,
        DumpToolInvocationOptions startupOptions)
    {
        var lang = string.IsNullOrWhiteSpace(startupLanguage)
            ? (_isKorean ? "ko" : "en")
            : startupLanguage;

        return new DumpToolInvocationOptions
        {
            DumpPath = dumpPath,
            OutDir = string.IsNullOrWhiteSpace(outDirText) ? null : outDirText,
            Language = lang,
            Headless = headless,
            Debug = startupOptions.Debug,
            AllowOnlineSymbols = startupOptions.AllowOnlineSymbols,
            ForceAdvancedUi = startupOptions.ForceAdvancedUi,
            ForceSimpleUi = startupOptions.ForceSimpleUi,
        };
    }

    public string T(string en, string ko) => _isKorean ? ko : en;

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

    // ── Advanced artifacts loading (static, no UI dependency) ─

    internal sealed class AdvancedArtifactsData
    {
        public List<string> EventLines { get; } = new();
        public int EventCount { get; set; }
        public string ReportText { get; set; } = string.Empty;
        public string WctText { get; set; } = string.Empty;
    }

    public static AdvancedArtifactsData LoadAdvancedArtifacts(
        string dumpPath,
        string outDir,
        string missingReportText,
        string missingWctText,
        CancellationToken cancellationToken)
    {
        var data = new AdvancedArtifactsData();

        var blackboxPath = NativeAnalyzerBridge.ResolveBlackboxPath(dumpPath, outDir);
        if (File.Exists(blackboxPath))
        {
            var tail = new Queue<string>(capacity: 200);
            foreach (var line in File.ReadLines(blackboxPath))
            {
                cancellationToken.ThrowIfCancellationRequested();
                if (string.IsNullOrWhiteSpace(line))
                {
                    continue;
                }

                if (tail.Count == 200)
                {
                    tail.Dequeue();
                }

                try
                {
                    using var jDoc = JsonDocument.Parse(line);
                    var root = jDoc.RootElement;
                    var idx = root.GetProperty("i").GetInt32();
                    var tMs = root.GetProperty("t_ms").GetDouble();
                    var tid = root.GetProperty("tid").GetUInt32();
                    var typeName = root.GetProperty("type_name").GetString() ?? "?";
                    var detail = root.TryGetProperty("detail", out var detProp) ? detProp.GetString() : null;

                    string formatted;
                    if (!string.IsNullOrEmpty(detail))
                    {
                        formatted = $"[{idx}] t={tMs:F0}ms tid={tid} {typeName} | {detail}";
                    }
                    else
                    {
                        var a = root.GetProperty("a").GetUInt64();
                        var b = root.GetProperty("b").GetUInt64();
                        formatted = $"[{idx}] t={tMs:F0}ms tid={tid} {typeName} a={a} b={b}";
                    }
                    tail.Enqueue(formatted);
                }
                catch (Exception ex)
                {
                    Debug.WriteLine($"Blackbox line parse failed: {ex.GetType().Name}: {ex.Message}");
                    tail.Enqueue(line);
                }
            }
            data.EventLines.AddRange(tail);
            data.EventCount = data.EventLines.Count;
        }

        var reportPath = NativeAnalyzerBridge.ResolveReportPath(dumpPath, outDir);
        data.ReportText = File.Exists(reportPath)
            ? File.ReadAllText(reportPath)
            : missingReportText;

        var wctPath = NativeAnalyzerBridge.ResolveWctPath(dumpPath, outDir);
        if (File.Exists(wctPath))
        {
            var raw = File.ReadAllText(wctPath);
            try
            {
                using var doc = JsonDocument.Parse(raw);
                data.WctText = JsonSerializer.Serialize(doc.RootElement, new JsonSerializerOptions
                {
                    WriteIndented = true,
                });
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"WCT JSON pretty-print failed: {ex.GetType().Name}: {ex.Message}");
                data.WctText = raw;
            }
        }
        else
        {
            data.WctText = missingWctText;
        }
        return data;
    }
}

public sealed record DumpDiscoveryItem(
    string FileName,
    string FullPath,
    string SourceLabel,
    string SourcePath,
    string UpdatedText,
    string SizeText,
    string AnalyzeLabel);

public sealed record DumpSearchLocationItem(string Path, string SourceLabel, bool IsRemovable);
