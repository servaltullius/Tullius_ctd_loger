using System.Linq;
using System.IO;

using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace SkyrimDiagDumpToolWinUI;

public sealed partial class MainWindow
{
    private async void SaveTriageButton_Click(object sender, RoutedEventArgs e)
    {
        var summaryPath = ResolveCurrentSummaryPath();
        if (string.IsNullOrWhiteSpace(summaryPath) || !File.Exists(summaryPath))
        {
            StatusText.Text = T(
                "Summary JSON is not available yet. Analyze a dump first.",
                "요약 JSON이 아직 없습니다. 먼저 덤프를 분석하세요.");
            return;
        }

        try
        {
            SaveTriageButton.IsEnabled = false;
            StatusText.Text = T("Saving review feedback...", "검토 피드백을 저장하는 중입니다...");

            await SummaryTriageStore.SaveAsync(summaryPath, BuildTriageReviewFromEditor(), CancellationToken.None);

            var summary = AnalysisSummary.LoadFromSummaryFile(summaryPath);
            RenderSummary(summary);
            StatusText.Text = T(
                "Review feedback saved to the summary JSON.",
                "검토 피드백을 요약 JSON에 저장했습니다.");
        }
        catch (Exception ex)
        {
            StatusText.Text = T("Failed to save review: ", "검토 저장 실패: ") + ex.Message;
        }
        finally
        {
            SaveTriageButton.IsEnabled = _vm.CurrentSummary is not null;
        }
    }

    private void PopulateTriageEditor(AnalysisSummary summary)
    {
        SelectReviewStatus(summary.Triage.ReviewStatus);
        VerdictBox.Text = summary.Triage.Verdict;
        ActualCauseBox.Text = summary.Triage.ActualCause;
        GroundTruthModBox.Text = summary.Triage.GroundTruthMod;
        ReviewNotesBox.Text = summary.Triage.Notes;
        TriageMetaText.Text = FormatTriageMetadata(summary.Triage);
        SetTriageEditorEnabled(true);
    }

    private void SetTriageEditorEnabled(bool isEnabled)
    {
        ReviewStatusComboBox.IsEnabled = isEnabled;
        VerdictBox.IsEnabled = isEnabled;
        ActualCauseBox.IsEnabled = isEnabled;
        GroundTruthModBox.IsEnabled = isEnabled;
        ReviewNotesBox.IsEnabled = isEnabled;
        SaveTriageButton.IsEnabled = isEnabled;
    }

    private void SelectReviewStatus(string reviewStatus)
    {
        var normalized = SummaryTriageStore.NormalizeReviewStatus(reviewStatus);
        foreach (var item in ReviewStatusComboBox.Items.OfType<ComboBoxItem>())
        {
            if (string.Equals(item.Tag as string, normalized, StringComparison.OrdinalIgnoreCase))
            {
                ReviewStatusComboBox.SelectedItem = item;
                return;
            }
        }

        ReviewStatusComboBox.SelectedIndex = 0;
    }

    private TriageReview BuildTriageReviewFromEditor()
    {
        var selectedStatus = ReviewStatusComboBox.SelectedItem as ComboBoxItem;
        var reviewStatus = selectedStatus?.Tag as string ?? TriageReview.UnreviewedStatus;
        var normalizedStatus = SummaryTriageStore.NormalizeReviewStatus(reviewStatus);
        var draftReview = new TriageReview
        {
            ReviewStatus = normalizedStatus,
            Reviewed = normalizedStatus != TriageReview.UnreviewedStatus,
            Verdict = VerdictBox.Text ?? string.Empty,
            ActualCause = ActualCauseBox.Text ?? string.Empty,
            GroundTruthMod = GroundTruthModBox.Text ?? string.Empty,
            Notes = ReviewNotesBox.Text ?? string.Empty,
        };
        var effectiveReviewed = SummaryTriageStore.IsReviewed(draftReview);
        if (normalizedStatus == TriageReview.UnreviewedStatus && effectiveReviewed)
        {
            normalizedStatus = "reviewed";
        }

        return draftReview with
        {
            ReviewStatus = normalizedStatus,
            Reviewed = effectiveReviewed,
        };
    }

    private string? ResolveCurrentSummaryPath()
    {
        if (string.IsNullOrWhiteSpace(_vm.CurrentDumpPath) || string.IsNullOrWhiteSpace(_vm.CurrentOutDir))
        {
            return null;
        }

        return NativeAnalyzerBridge.ResolveSummaryPath(_vm.CurrentDumpPath, _vm.CurrentOutDir);
    }

    private string FormatTriageMetadata(TriageReview triage)
    {
        var reviewState = DescribeReviewStatus(triage.ReviewStatus);

        if (string.IsNullOrWhiteSpace(triage.ReviewedAtUtc))
        {
            return T("Review status: ", "검토 상태: ") + reviewState;
        }

        return T("Review status: ", "검토 상태: ") + reviewState + " | "
            + T("Last saved (UTC): ", "마지막 저장 (UTC): ") + triage.ReviewedAtUtc;
    }

    private string DescribeReviewStatus(string reviewStatus)
    {
        return SummaryTriageStore.NormalizeReviewStatus(reviewStatus) switch
        {
            "reviewed" => T("Reviewed", "검토 완료"),
            "confirmed" => T("Confirmed", "확인됨"),
            "triaged" => T("Triaged", "분류됨"),
            "done" => T("Done", "완료"),
            _ => T("Unreviewed", "미검토"),
        };
    }
}
