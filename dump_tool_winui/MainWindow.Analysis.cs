using System.Diagnostics;
using System.IO;

using Microsoft.UI.Xaml;

using Windows.ApplicationModel.DataTransfer;

namespace SkyrimDiagDumpToolWinUI;

public sealed partial class MainWindow
{
    private async void AnalyzeButton_Click(object sender, RoutedEventArgs e)
    {
        await AnalyzeAsync(preferExistingArtifacts: false);
    }

    private void CancelAnalyzeButton_Click(object sender, RoutedEventArgs e)
    {
        _analysisCts?.Cancel();
    }

    private async Task<bool> TryLoadExistingAnalysisAsync(string dumpPath, string outDir, CancellationToken cancellationToken)
    {
        var summaryPath = NativeAnalyzerBridge.ResolveSummaryPath(dumpPath, outDir);

        for (var i = 0; i < 15 && !File.Exists(summaryPath); i++)
        {
            cancellationToken.ThrowIfCancellationRequested();
            await Task.Delay(100, cancellationToken);
        }

        if (!File.Exists(summaryPath))
        {
            return false;
        }

        for (var attempt = 0; attempt < 5; attempt++)
        {
            try
            {
                cancellationToken.ThrowIfCancellationRequested();
                var summary = AnalysisSummary.LoadFromSummaryFile(summaryPath);
                RenderSummary(summary);
                await RenderAdvancedArtifactsAsync(dumpPath, outDir, cancellationToken);
                SetBusy(false, T(
                    "Loaded existing analysis artifacts. Click Analyze to refresh.",
                    "기존 분석 결과를 불러왔습니다. 다시 분석하려면 \"지금 분석\"을 누르세요."));
                NavView.SelectedItem = NavTriage;
                return true;
            }
            catch (OperationCanceledException)
            {
                throw;
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"TryLoadExistingAnalysisAsync attempt {attempt}: {ex.GetType().Name}: {ex.Message}");
                await Task.Delay(100, cancellationToken);
            }
        }

        return false;
    }

    private async Task AnalyzeAsync(bool preferExistingArtifacts)
    {
        var dumpPath = DumpPathBox.Text.Trim();
        if (string.IsNullOrWhiteSpace(dumpPath))
        {
            StatusText.Text = T("Select a .dmp file first.", "먼저 .dmp 파일을 선택하세요.");
            return;
        }

        dumpPath = Path.GetFullPath(dumpPath);
        if (!File.Exists(dumpPath))
        {
            StatusText.Text = T("Dump file not found: ", "덤프 파일을 찾을 수 없습니다: ") + dumpPath;
            return;
        }

        await PromoteLearnedDumpLocationAsync(dumpPath);
        await RefreshDiscoveredDumpsAsync();

        _analysisCts?.Cancel();
        _analysisCts?.Dispose();
        using var analysisCts = new CancellationTokenSource();
        _analysisCts = analysisCts;
        var cancellationToken = analysisCts.Token;

        var options = _vm.BuildInvocationOptions(
            dumpPath, OutputDirBox.Text.Trim(), _startupOptions.Language, false, _startupOptions);
        var outDir = NativeAnalyzerBridge.ResolveOutputDirectory(dumpPath, options.OutDir);
        _vm.CurrentDumpPath = dumpPath;
        _vm.CurrentOutDir = outDir;

        try
        {
            if (preferExistingArtifacts)
            {
                SetBusy(true, T(
                    "Checking for existing analysis artifacts...",
                    "기존 분석 결과를 확인 중입니다..."));
                if (await TryLoadExistingAnalysisAsync(dumpPath, outDir, cancellationToken))
                {
                    return;
                }
            }

            SetBusy(true, T("Analyzing dump with native engine...", "네이티브 엔진으로 덤프를 분석 중입니다..."));

            var (exitCode, nativeErr) = await NativeAnalyzerBridge.RunAnalyzeAsync(options, cancellationToken);
            cancellationToken.ThrowIfCancellationRequested();
            if (exitCode == NativeAnalyzerBridge.UserCanceledExitCode)
            {
                SetBusy(false, T("Analysis canceled.", "분석이 취소되었습니다."));
                return;
            }

            if (exitCode != 0)
            {
                var prefix = T("Analysis failed. Exit code: ", "분석 실패. 종료 코드: ");
                var msg = prefix + exitCode;
                if (!string.IsNullOrWhiteSpace(nativeErr))
                {
                    msg += "\n" + nativeErr;
                }
                SetBusy(false, msg);
                return;
            }

            var summaryPath = NativeAnalyzerBridge.ResolveSummaryPath(dumpPath, outDir);
            if (!File.Exists(summaryPath))
            {
                SetBusy(false, T("Analysis finished but summary file is missing: ", "분석은 끝났지만 요약 파일이 없습니다: ") + summaryPath);
                return;
            }

            var summary = AnalysisSummary.LoadFromSummaryFile(summaryPath);
            RenderSummary(summary);
            await RenderAdvancedArtifactsAsync(dumpPath, outDir, cancellationToken);
            SetBusy(false, T("Analysis complete. Review the candidates and checklist.", "분석 완료. 원인 후보와 체크리스트를 확인하세요."));
            NavView.SelectedItem = NavTriage;
        }
        catch (OperationCanceledException)
        {
            SetBusy(false, T("Analysis canceled.", "분석이 취소되었습니다."));
        }
        catch (Exception ex)
        {
            SetBusy(false, T("Failed to read summary JSON: ", "요약 JSON을 읽지 못했습니다: ") + ex.Message);
        }
        finally
        {
            if (ReferenceEquals(_analysisCts, analysisCts))
            {
                _analysisCts = null;
            }
        }
    }

    private void RenderSummary(AnalysisSummary summary)
    {
        _vm.PopulateSummary(summary);

        SummarySentenceText.Text = _vm.SummarySentence;
        BucketText.Text = _vm.BucketText;

        if (_vm.ShowCorrelationBadge)
        {
            CorrelationBadge.Text = _vm.CorrelationBadgeText;
            CorrelationBadge.Visibility = Visibility.Visible;
        }
        else
        {
            CorrelationBadge.Visibility = Visibility.Collapsed;
        }

        CrashContextSummaryText.Text = _vm.CrashContextSummary;
        ModuleText.Text = _vm.ModuleText;
        ModNameText.Text = _vm.ModNameText;

        CopySummaryButton.IsEnabled = true;
        CopyShareButton.IsEnabled = true;

        QuickPrimaryValueText.Text = _vm.QuickPrimaryValue;
        QuickConfidenceValueText.Text = _vm.QuickConfidenceValue;
        QuickPrimaryLabelText.Text = _vm.QuickPrimaryLabel;
        QuickActionsValueText.Text = _vm.QuickActionsValue;
        QuickEventsValueText.Text = _vm.QuickEventsValue;
        RecaptureContextTitleText.Text = _vm.RecaptureContextTitle;
        RecaptureContextDetailsText.Text = _vm.RecaptureContextDetails;
        RecaptureContextCard.Visibility = _vm.ShowRecaptureContext
            ? Visibility.Visible
            : Visibility.Collapsed;

        ConflictCandidatesPanel.Visibility = _vm.ConflictComparisonRows.Count > 0
            ? Visibility.Visible
            : Visibility.Collapsed;
        ImmediateRecommendationsSection.Visibility = _vm.ImmediateRecommendations.Count > 0
            ? Visibility.Visible
            : Visibility.Collapsed;
        VerificationRecommendationsSection.Visibility = _vm.VerificationRecommendations.Count > 0
            ? Visibility.Visible
            : Visibility.Collapsed;
        RecaptureRecommendationsSection.Visibility = _vm.RecaptureRecommendations.Count > 0
            ? Visibility.Visible
            : Visibility.Collapsed;

        if (_vm.ShowTroubleshooting)
        {
            TroubleshootingExpander.Header = _vm.TroubleshootingTitle;
            TroubleshootingCard.Visibility = Visibility.Visible;
            TroubleshootingList.ItemsSource = _vm.TroubleshootingSteps;
        }
        else
        {
            TroubleshootingCard.Visibility = Visibility.Collapsed;
        }

        PopulateTriageEditor(summary);
    }

    private void CopySummaryButton_Click(object sender, RoutedEventArgs e)
    {
        var text = _vm.BuildSummaryClipboardText();
        if (string.IsNullOrWhiteSpace(text))
        {
            StatusText.Text = T("No summary to copy yet.", "아직 복사할 요약이 없습니다.");
            return;
        }

        try
        {
            var dataPackage = new DataPackage();
            dataPackage.SetText(text);
            Clipboard.SetContent(dataPackage);
            Clipboard.Flush();
            StatusText.Text = T("Copied crash summary to clipboard.", "크래시 요약을 클립보드에 복사했습니다.");
        }
        catch (Exception ex)
        {
            StatusText.Text = T("Failed to copy to clipboard: ", "클립보드 복사 실패: ") + ex.Message;
        }
    }

    private void CopyShareButton_Click(object sender, RoutedEventArgs e)
    {
        var text = _vm.BuildCommunityShareText();
        if (string.IsNullOrWhiteSpace(text))
        {
            StatusText.Text = T("No summary to share yet.", "아직 공유할 요약이 없습니다.");
            return;
        }

        try
        {
            var dataPackage = new DataPackage();
            dataPackage.SetText(text);
            Clipboard.SetContent(dataPackage);
            Clipboard.Flush();
            StatusText.Text = T("Copied community share text to clipboard.", "커뮤니티 공유용 요약을 클립보드에 복사했습니다.");
        }
        catch (Exception ex)
        {
            StatusText.Text = T("Failed to copy to clipboard: ", "클립보드 복사 실패: ") + ex.Message;
        }
    }

    private async Task RenderAdvancedArtifactsAsync(string dumpPath, string outDir, CancellationToken cancellationToken)
    {
        var artifacts = await Task.Run(
            () => MainWindowViewModel.LoadAdvancedArtifacts(
                dumpPath,
                outDir,
                T("Report file not found.", "리포트 파일이 없습니다."),
                T("WCT file not found for this dump.", "이 덤프에 대한 WCT 파일이 없습니다."),
                cancellationToken),
            cancellationToken);
        cancellationToken.ThrowIfCancellationRequested();

        _vm.PopulateAdvancedArtifacts(artifacts);
        QuickEventsValueText.Text = _vm.QuickEventsValue;
        ReportTextBox.Text = artifacts.ReportText;
        WctTextBox.Text = artifacts.WctText;
    }

    private void OpenOutputButton_Click(object sender, RoutedEventArgs e)
    {
        var outDir = _vm.CurrentOutDir;
        if (string.IsNullOrWhiteSpace(outDir) || !Directory.Exists(outDir))
        {
            StatusText.Text = T("Output folder is not available yet.", "출력 폴더가 아직 없습니다.");
            return;
        }

        try
        {
            Process.Start(new ProcessStartInfo
            {
                FileName = outDir,
                UseShellExecute = true,
                Verb = "open",
            });
        }
        catch (Exception ex)
        {
            StatusText.Text = T("Failed to open output folder: ", "출력 폴더 열기 실패: ") + ex.Message;
        }
    }
}
