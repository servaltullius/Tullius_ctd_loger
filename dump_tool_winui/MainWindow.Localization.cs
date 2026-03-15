namespace SkyrimDiagDumpToolWinUI;

public sealed partial class MainWindow
{
    private void ApplyLocalizedStaticText()
    {
        Title = T("Tullius CTD Logger", "툴리우스 CTD 로거");

        NavAnalyze.Content = T("Dashboard", "대시보드");
        NavTriage.Content = T("Triage", "분석 결과");
        NavRawData.Content = T("Raw Data", "원시 데이터");

        HeaderSubtitleText.Text = T(
            "Skyrim SE detected. Ready for dump triage.",
            "Skyrim SE가 감지되었습니다. 덤프 원인 분석을 시작할 수 있습니다.");
        HeaderBadgeText.Text = T("STATUS READY", "상태 준비됨");

        AnalyzeSectionTitleText.Text = T("Direct dump path", "직접 덤프 경로");
        RecentDumpsTitleText.Text = T("Recent discovered dumps", "최근 발견된 덤프");
        RecentDumpsHintText.Text = T(
            "We scan Tullius output locations and show the newest dumps first.",
            "툴리우스 출력 위치를 스캔하고 가장 최근 덤프를 먼저 보여줍니다.");
        RecentDumpsEmptyTitleText.Text = T("No dumps were found.", "덤프를 찾지 못했습니다.");
        RecentDumpsEmptyHintText.Text = T(
            "Add your MO2 overwrite folder or the folder referenced by OutputDir.",
            "MO2 overwrite 또는 OutputDir 폴더를 추가하세요.");
        DumpSearchLocationsTitleText.Text = T("Dump output locations", "덤프 출력 위치");
        DumpSearchLocationsHintText.Text = T(
            "Register your MO2 overwrite folder or OutputDir folder. Subfolders are searched automatically.",
            "MO2 overwrite 또는 OutputDir 폴더를 등록하면 하위 폴더까지 자동 검색합니다.");
        DumpSearchLocationsEmptyText.Text = T(
            "No dump output locations are saved yet.",
            "저장된 덤프 출력 위치가 아직 없습니다.");

        SnapshotSectionTitleText.Text = T("Crash Summary", "크래시 요약");
        NextStepsSectionTitleText.Text = T("Recommended Next Steps", "권장 다음 단계");
        SuspectsSectionTitleText.Text = T("Actionable Candidates", "행동 우선 후보");
        QuickPrimaryLabelText.Text = T("CrashLogger context", "CrashLogger 기준");
        QuickConfidenceLabelText.Text = T("Evidence agreement", "근거 합의");
        QuickActionsLabelText.Text = T("Next action", "다음 조치");
        QuickEventsLabelText.Text = T("Blackbox events", "블랙박스 이벤트");
        ConflictCandidatesTitleText.Text = T("Signal Comparison", "신호 비교");
        CrashContextTitleText.Text = T("Crash context", "크래시 컨텍스트");
        ImmediateRecommendationsTitleText.Text = T("Do This Now", "지금 바로");
        VerificationRecommendationsTitleText.Text = T("Verify Next", "추가 확인");
        RecaptureRecommendationsTitleText.Text = T("Recapture or Compare", "재수집 / 비교");
        RecaptureContextTitleText.Text = T("Recapture context", "재수집 문맥");
        RecaptureContextDetailsText.Text = "-";
        QuickPrimaryValueText.Text = "-";
        QuickConfidenceValueText.Text = "-";
        QuickActionsValueText.Text = "-";
        QuickEventsValueText.Text = "-";

        SummarySentenceText.Text = T("No analysis yet.", "아직 분석 결과가 없습니다.");
        CallstackLabelText.Text = T("Callstack", "콜스택");
        EvidenceLabelText.Text = T("Evidence", "근거");
        ResourcesLabelText.Text = T("Recent Resources", "최근 리소스");
        EventsLabelText.Text = T("Events (Blackbox)", "이벤트 (블랙박스)");
        WctLabelText.Text = T("WCT JSON", "WCT JSON");
        ReportLabelText.Text = T("Report", "리포트");

        DumpPathBox.PlaceholderText = T("Select a .dmp file", ".dmp 파일을 선택하세요");
        OutputDirBox.PlaceholderText = T("Optional output directory (empty = dump folder)", "선택 출력 폴더 (비우면 덤프 폴더)");

        RescanDumpsButton.Content = T("Rescan", "다시 스캔");
        ManageDumpFoldersButton.Content = T("Manage folders", "폴더 관리");
        EmptyStateManageFoldersButton.Content = T("Manage folders", "폴더 관리");
        DirectSelectDumpButton.Content = T("Direct select", "직접 선택");
        EmptyStateDirectSelectButton.Content = T("Direct select", "직접 선택");
        AddDumpSearchLocationButton.Content = T("Add location", "위치 추가");
        RemoveDumpSearchLocationButton.Content = T("Remove selected", "선택 제거");
        BrowseDumpButton.Content = T("Select dump", "덤프 선택");
        BrowseOutputButton.Content = T("Select folder", "폴더 선택");
        AnalyzeButton.Content = T("ANALYZE NOW", "지금 분석");
        CancelAnalyzeButton.Content = T("Cancel analysis", "분석 취소");
        OpenOutputButton.Content = T("Open report folder", "리포트 폴더 열기");
        CopySummaryButton.Content = T("Copy summary", "요약 복사");
        CopyShareButton.Content = T("Share", "공유");
        TriageEditorTitleText.Text = T("Review Feedback", "검토 피드백");
        ReviewStatusLabelText.Text = T("Review status", "검토 상태");
        ReviewStatusUnreviewedItem.Content = T("Unreviewed", "미검토");
        ReviewStatusReviewedItem.Content = T("Reviewed", "검토 완료");
        ReviewStatusConfirmedItem.Content = T("Confirmed", "확인됨");
        ReviewStatusTriagedItem.Content = T("Triaged", "분류됨");
        ReviewStatusDoneItem.Content = T("Done", "완료");
        VerdictLabelText.Text = T("Verdict", "판정");
        VerdictBox.PlaceholderText = T("Top conclusion or short verdict", "최종 결론 또는 짧은 판정");
        ActualCauseLabelText.Text = T("Actual cause", "실제 원인");
        ActualCauseBox.PlaceholderText = T("What actually caused the CTD", "CTD의 실제 원인이 무엇이었는지");
        GroundTruthModLabelText.Text = T("Ground truth mod", "확정 모드");
        GroundTruthModBox.PlaceholderText = T("Confirmed mod/plugin if known", "확정된 모드/플러그인이 있으면 입력");
        ReviewNotesLabelText.Text = T("Review notes", "검토 메모");
        ReviewNotesBox.PlaceholderText = T("Why the top candidate was right or wrong", "상위 후보가 맞았는지 틀렸는지 이유를 기록");
        SaveTriageButton.Content = T("Save review", "검토 저장");
        TriageMetaText.Text = T(
            "Run analysis first, then save review feedback into the summary JSON.",
            "먼저 분석을 실행한 뒤, 검토 피드백을 요약 JSON에 저장하세요.");
    }

    private string T(string en, string ko) => _vm.T(en, ko);
}
