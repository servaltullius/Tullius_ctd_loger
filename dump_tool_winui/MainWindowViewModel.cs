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
    public ObservableCollection<string> CallstackFrames { get; } = new();
    public ObservableCollection<EvidenceViewItem> EvidenceItems { get; } = new();
    public ObservableCollection<ResourceViewItem> ResourceItems { get; } = new();
    public ObservableCollection<string> EventItems { get; } = new();

    // ── Summary fields for UI ─────────────────────────────────
    public string SummarySentence { get; private set; } = string.Empty;
    public string BucketText { get; private set; } = string.Empty;
    public string ModuleText { get; private set; } = string.Empty;
    public string ModNameText { get; private set; } = string.Empty;
    public bool ShowCorrelationBadge { get; private set; }
    public string CorrelationBadgeText { get; private set; } = string.Empty;
    public string QuickPrimaryLabel { get; private set; } = string.Empty;
    public string QuickPrimaryValue { get; private set; } = string.Empty;
    public string QuickConfidenceValue { get; private set; } = string.Empty;
    public string QuickActionsValue { get; private set; } = string.Empty;
    public string QuickEventsValue { get; private set; } = string.Empty;
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
        PopulateTroubleshooting(summary);
        PopulateCallstack(summary);
        PopulateEvidence(summary);
        PopulateResources(summary);
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

        if (summary.ActionableCandidates.Count > 0)
        {
            var primaryCandidate = summary.ActionableCandidates[0];
            QuickPrimaryValue = BuildPrimaryCandidateValue(summary);
            QuickConfidenceValue = DescribeEvidenceAgreement(primaryCandidate);
            QuickPrimaryLabel = DescribeActionableCandidateLabel(primaryCandidate);
        }
        else
        {
            var primarySuspect = Suspects.FirstOrDefault();
            QuickPrimaryValue = primarySuspect is null
                ? T("Unknown", "알 수 없음")
                : primarySuspect.Module;
            QuickConfidenceValue = primarySuspect is null || string.IsNullOrWhiteSpace(primarySuspect.Confidence)
                ? T("Unrated", "미평가")
                : primarySuspect.Confidence;

            QuickPrimaryLabel = summary.CrashLoggerRefs.Count > 0
                ? T("Referenced mod (ESP)", "참조 모드 (ESP)")
                : T("Actionable candidate", "행동 우선 후보");
        }
    }

    private void PopulateRecommendations(AnalysisSummary summary)
    {
        Recommendations.Clear();
        foreach (var recommendation in summary.Recommendations.Take(12))
        {
            Recommendations.Add(recommendation);
        }
        var recommendationCount = Recommendations.Count;
        if (Recommendations.Count == 0)
        {
            Recommendations.Add(T("No recommendation text was generated.", "권장 조치 문구가 생성되지 않았습니다."));
        }
        QuickActionsValue = recommendationCount == 0
            ? T("None", "없음")
            : $"{recommendationCount} {T("items", "개 항목")}";
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
            lines.Add((_isKorean ? "근거 합의: " : "Evidence agreement: ") + DescribeEvidenceAgreement(primaryCandidate));
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
            lines.Add($"🧭 {(_isKorean ? "근거 합의" : "Evidence agreement")}: {DescribeEvidenceAgreement(primaryCandidate)}");
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
            lines.Add($"🛠️ {(_isKorean ? "권장" : "Action")}: {firstAction}");
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
