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
internal sealed partial class MainWindowViewModel
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
