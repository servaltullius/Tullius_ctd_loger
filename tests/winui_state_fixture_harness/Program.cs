using System.Text.Json;
using System.Text.Json.Nodes;
using SkyrimDiagDumpToolWinUI;

internal static class Program
{
    private static async Task<int> Main()
    {
        var root = Path.Combine(
            Path.GetTempPath(),
            "SkyrimDiagWinUIStateTests-" + Guid.NewGuid().ToString("N"));
        try
        {
            var aDir = Path.Combine(root, "a");
            var bDir = Path.Combine(root, "b");
            var outDir = Path.Combine(root, "out");
            Directory.CreateDirectory(aDir);
            Directory.CreateDirectory(bDir);
            Directory.CreateDirectory(outDir);

            var dumpA = Path.Combine(aDir, "SameStem.dmp");
            var dumpB = Path.Combine(bDir, "SameStem.dmp");
            var dumpAWithDifferentFileTime = Path.Combine(bDir, "SameBytesDifferentFileTime.dmp");
            await File.WriteAllBytesAsync(dumpA, Enumerable.Repeat((byte)0x41, 4096).ToArray());
            await File.WriteAllBytesAsync(dumpB, Enumerable.Repeat((byte)0x42, 4096).ToArray());
            File.Copy(dumpA, dumpAWithDifferentFileTime);
            File.SetLastWriteTimeUtc(
                dumpAWithDifferentFileTime,
                File.GetLastWriteTimeUtc(dumpA).AddSeconds(2));

            var identityA = await DumpIdentityContract.ComputeAsync(dumpA, CancellationToken.None);
            var identityB = await DumpIdentityContract.ComputeAsync(dumpB, CancellationToken.None);
            var identityAWithDifferentFileTime =
                await DumpIdentityContract.ComputeAsync(dumpAWithDifferentFileTime, CancellationToken.None);
            Require(identityA.IsValid && identityB.IsValid, "identities must be valid");
            Require(identityA.Sha256 != identityB.Sha256, "same-stem dumps must have distinct hashes");
            Require(
                identityA.Sha256 == identityAWithDifferentFileTime.Sha256 &&
                identityA.LastWriteTimeUtc100ns != identityAWithDifferentFileTime.LastWriteTimeUtc100ns,
                "fixture must isolate identical bytes by FILETIME too");
            Require(
                identityA.ResolveArtifactDirectory(outDir) !=
                    identityAWithDifferentFileTime.ResolveArtifactDirectory(outDir),
                "artifact storage key must include SHA256, size, and FILETIME");
            Require(
                identityA.ResolveTriageStatePath(outDir) !=
                    identityAWithDifferentFileTime.ResolveTriageStatePath(outDir),
                "triage storage key must include SHA256, size, and FILETIME");

            Require(identityA.MatchesFileMetadata(dumpA), "hashed identity must match its unchanged dump metadata");
            Require(
                !identityA.MatchesFileMetadata(dumpAWithDifferentFileTime),
                "metadata re-check must reject a dump with another FILETIME");

            var summaryPathA = Path.Combine(identityA.ResolveArtifactDirectory(outDir), "Summary.json");
            var summaryPathB = Path.Combine(identityB.ResolveArtifactDirectory(outDir), "Summary.json");
            await AtomicFile.WriteAllTextAsync(
                summaryPathA,
                BuildSummary(identityA),
                CancellationToken.None);
            var reviewA = new TriageReview
            {
                ReviewStatus = "confirmed",
                Reviewed = true,
                Verdict = "A verdict",
                ActualCause = "A cause",
                GroundTruthMod = "A.esp",
                Notes = "A notes",
            };
            await SummaryTriageStore.SaveAsync(
                summaryPathA,
                dumpA,
                outDir,
                reviewA,
                CancellationToken.None);

            var stateA = identityA.ResolveTriageStatePath(outDir);
            Require(File.Exists(stateA), "identity A state must exist");
            Require((await File.ReadAllTextAsync(stateA)).Contains("A.esp"), "identity A state lost its review");
            Require(
                File.Exists(Path.Combine(outDir, ".skydiag-locks", "SameStem.lock")),
                "triage save must use the same per-stem output-family lock as the native writer");

            var summaryBeforeMirrorFailure = await File.ReadAllTextAsync(summaryPathA);
            var stateBeforeMirrorFailure = await File.ReadAllTextAsync(stateA);
            var replacementReviewA = new TriageReview
            {
                ReviewStatus = "confirmed",
                Reviewed = true,
                Verdict = "replacement verdict",
                ActualCause = "replacement cause",
                GroundTruthMod = "Replacement.esp",
                Notes = "replacement notes",
            };
            var mirrorWriteAttempted = false;
            var mirrorFailureObserved = false;
            try
            {
                await SummaryTriageStore.SaveAsync(
                    summaryPathA,
                    dumpA,
                    outDir,
                    replacementReviewA,
                    CancellationToken.None,
                    async (mirrorPath, _, cancellationToken) =>
                    {
                        mirrorWriteAttempted = true;
                        Require(mirrorPath == summaryPathA, "unexpected summary mirror path");
                        var committedState = await File.ReadAllTextAsync(
                            stateA,
                            cancellationToken);
                        Require(
                            committedState.Contains("Replacement.esp"),
                            "authoritative state write must precede the forced mirror failure");
                        throw new IOException("forced summary mirror write failure");
                    });
            }
            catch (IOException ex) when (ex.Message.Contains("forced summary mirror write failure"))
            {
                mirrorFailureObserved = true;
            }
            Require(mirrorWriteAttempted, "summary mirror writer was not invoked");
            Require(mirrorFailureObserved, "forced summary mirror failure was not reported");
            Require(
                await File.ReadAllTextAsync(stateA) == stateBeforeMirrorFailure,
                "failed summary mirror write left the new authoritative state committed");
            Require(
                await File.ReadAllTextAsync(summaryPathA) == summaryBeforeMirrorFailure,
                "failed summary mirror write changed the summary mirror");

            // Simulate analyzer publication for a different dump with the same
            // stem. Both authoritative summary families must coexist.
            await AtomicFile.WriteAllTextAsync(
                summaryPathB,
                BuildSummary(identityB),
                CancellationToken.None);
            var loadedA = AnalysisSummary.LoadFromSummaryFile(summaryPathA);
            var loadedB = AnalysisSummary.LoadFromSummaryFile(summaryPathB);
            Require(loadedA.DumpIdentity == identityA, "summary A was overwritten");
            Require(loadedA.Triage.GroundTruthMod == "A.esp", "summary A lost its review");
            Require(loadedB.DumpIdentity == identityB, "summary B must identify dump B");
            Require(!loadedB.Triage.Reviewed, "dump B must not inherit dump A review");
            Require(string.IsNullOrEmpty(loadedB.Triage.GroundTruthMod), "dump B inherited dump A ground truth");

            var mismatchRejected = false;
            try
            {
                await SummaryTriageStore.SaveAsync(
                    summaryPathB,
                    dumpA,
                    outDir,
                    reviewA,
                    CancellationToken.None);
            }
            catch (InvalidDataException)
            {
                mismatchRejected = true;
            }
            Require(mismatchRejected, "saving a review through a mismatched canonical summary must fail");

            var reviewB = new TriageReview
            {
                ReviewStatus = "reviewed",
                Reviewed = true,
                Verdict = "B verdict",
                ActualCause = "B cause",
                GroundTruthMod = "B.esp",
            };
            await SummaryTriageStore.SaveAsync(
                summaryPathB,
                dumpB,
                outDir,
                reviewB,
                CancellationToken.None);
            var stateB = identityB.ResolveTriageStatePath(outDir);
            Require(File.Exists(stateB), "identity B state must exist");
            Require(stateA != stateB, "triage state paths must be identity-isolated");
            Require((await File.ReadAllTextAsync(stateA)).Contains("A.esp"), "dump B overwrote dump A state");
            Require((await File.ReadAllTextAsync(stateB)).Contains("B.esp"), "dump B state was not saved");

            // The stem summary next to the dump output gets the review when it
            // describes the same dump, and is left alone when it does not.
            var cDir = Path.Combine(root, "c");
            Directory.CreateDirectory(cDir);
            var dumpC = Path.Combine(cDir, "Unique.dmp");
            await File.WriteAllBytesAsync(dumpC, Enumerable.Repeat((byte)0x43, 4096).ToArray());
            var identityC = await DumpIdentityContract.ComputeAsync(dumpC, CancellationToken.None);
            var summaryPathC = Path.Combine(identityC.ResolveArtifactDirectory(outDir), "Summary.json");
            var stemSummaryC = Path.Combine(outDir, "Unique_SkyrimDiagSummary.json");
            await AtomicFile.WriteAllTextAsync(summaryPathC, BuildSummary(identityC), CancellationToken.None);
            await AtomicFile.WriteAllTextAsync(stemSummaryC, BuildSummary(identityC), CancellationToken.None);
            await SummaryTriageStore.SaveAsync(
                summaryPathC,
                dumpC,
                outDir,
                new TriageReview { ReviewStatus = "confirmed", Reviewed = true, GroundTruthMod = "C.esp" },
                CancellationToken.None);
            Require(
                AnalysisSummary.LoadFromSummaryFile(stemSummaryC).Triage.GroundTruthMod == "C.esp",
                "the stem summary of the same dump must get the review");

            var stemSummaryOfB = Path.Combine(outDir, "SameStem_SkyrimDiagSummary.json");
            await AtomicFile.WriteAllTextAsync(stemSummaryOfB, BuildSummary(identityB), CancellationToken.None);
            var stemOfBBefore = await File.ReadAllTextAsync(stemSummaryOfB);
            await SummaryTriageStore.SaveAsync(summaryPathA, dumpA, outDir, reviewA, CancellationToken.None);
            Require(
                await File.ReadAllTextAsync(stemSummaryOfB) == stemOfBBefore,
                "a stem summary of another dump with the same name must be left alone");

            var discoveryPath = Path.Combine(root, "dump-discovery.json");
            var discoveryA = new DumpDiscoveryState
            {
                RegisteredRoots = new List<string> { aDir },
            };
            await DumpDiscoveryStore.SaveToPathAsync(
                discoveryA,
                discoveryPath,
                CancellationToken.None);
            var authoritativeDiscovery = await File.ReadAllTextAsync(discoveryPath);
            var discoveryB = new DumpDiscoveryState
            {
                RegisteredRoots = new List<string> { bDir },
            };
            using (var canceled = new CancellationTokenSource())
            {
                canceled.Cancel();
                var canceledWriteObserved = false;
                try
                {
                    await DumpDiscoveryStore.SaveToPathAsync(
                        discoveryB,
                        discoveryPath,
                        canceled.Token);
                }
                catch (OperationCanceledException)
                {
                    canceledWriteObserved = true;
                }
                Require(canceledWriteObserved, "canceled discovery-state write must fail");
            }
            Require(
                await File.ReadAllTextAsync(discoveryPath) == authoritativeDiscovery,
                "failed discovery-state write destroyed the previous authoritative file");
            // Two viewers start from the same snapshot. The second one's change
            // must be applied to the file as it is now, not to its snapshot.
            var staleSnapshot = DumpDiscoveryStore.LoadFromPath(discoveryPath);
            await DumpDiscoveryStore.UpdateAtPathAsync(
                discoveryPath,
                state => DumpDiscoveryStore.AddRegisteredRoot(state, bDir),
                CancellationToken.None);
            Require(
                staleSnapshot.RegisteredRoots.Count == 1,
                "fixture snapshot must predate the other viewer's change");
            var merged = await DumpDiscoveryStore.UpdateAtPathAsync(
                discoveryPath,
                state => DumpDiscoveryStore.PromoteLearnedRoot(state, cDir),
                CancellationToken.None);
            var reloaded = DumpDiscoveryStore.LoadFromPath(discoveryPath);
            Require(
                reloaded.RegisteredRoots.Contains(Path.GetFullPath(aDir), StringComparer.OrdinalIgnoreCase) &&
                reloaded.RegisteredRoots.Contains(Path.GetFullPath(bDir), StringComparer.OrdinalIgnoreCase),
                "a discovery update dropped a folder another viewer registered");
            Require(
                reloaded.LearnedRoots.Contains(Path.GetFullPath(cDir), StringComparer.OrdinalIgnoreCase),
                "a discovery update lost its own change");
            Require(
                merged.RegisteredRoots.SequenceEqual(reloaded.RegisteredRoots) &&
                merged.LearnedRoots.SequenceEqual(reloaded.LearnedRoots),
                "a discovery update must return the state it saved");

            // A state file that does not parse is not saved over with an
            // empty list: the update fails and the file keeps its folders.
            var corruptDiscovery = "{ \"RegisteredRoots\": [ " + Environment.NewLine;
            await File.WriteAllTextAsync(discoveryPath, corruptDiscovery);
            var corruptUpdateFailed = false;
            try
            {
                await DumpDiscoveryStore.UpdateAtPathAsync(
                    discoveryPath,
                    state => DumpDiscoveryStore.PromoteLearnedRoot(state, cDir),
                    CancellationToken.None);
            }
            catch (JsonException)
            {
                corruptUpdateFailed = true;
            }
            Require(corruptUpdateFailed, "a discovery update over an unreadable state file must fail");
            Require(
                await File.ReadAllTextAsync(discoveryPath) == corruptDiscovery,
                "a failed discovery update must leave the state file alone");

            // A missing file is an empty state.
            File.Delete(discoveryPath);
            var fromMissing = await DumpDiscoveryStore.UpdateAtPathAsync(
                discoveryPath,
                state => DumpDiscoveryStore.AddRegisteredRoot(state, aDir),
                CancellationToken.None);
            Require(
                fromMissing.RegisteredRoots.Count == 1 && fromMissing.LearnedRoots.Count == 0,
                "a discovery update without a state file starts from an empty state");

            // Drive roots (e.g. "C:\") must keep their trailing separator rather
            // than turning into drive-relative paths ("C:").
            var driveRoot = Path.GetPathRoot(root)!;
            var withDriveRoot = DumpDiscoveryStore.AddRegisteredRoot(new DumpDiscoveryState(), driveRoot);
            Require(
                withDriveRoot.RegisteredRoots.Count == 1 &&
                string.Equals(withDriveRoot.RegisteredRoots[0], driveRoot, StringComparison.OrdinalIgnoreCase) &&
                Path.EndsInDirectorySeparator(withDriveRoot.RegisteredRoots[0]),
                "NormalizeRoot must preserve the trailing separator on a drive root");

            Require(
                !Directory.EnumerateFiles(root, "*.tmp.*", SearchOption.AllDirectories).Any(),
                "atomic writes left temporary files behind");

            await CheckReanalysisByAnalyzerVersion(root, identityA);
            await CheckSaveLoadHistoryFingerprint(root, identityA);
            CheckIniTextDecoding();

            await CheckAdvancedArtifactIdentityAndTailParsing(root, dumpA, dumpB, identityA);
            await CheckCrashLoggerLogPathResolution(root);
            await CheckCrashLoggerLogPathExtensions(root);
            await CheckRecentDumpsDefaultCapIncludesMoreThanTwelve(root);

            Console.WriteLine("winui_state_fixture_harness: OK");
            return 0;
        }
        finally
        {
            try
            {
                Directory.Delete(root, recursive: true);
            }
            catch
            {
            }
        }
    }

    // A saved summary is reused only when the analyzer that wrote it is not
    // older than the viewer; one without analyzer_version or without a quick
    // answer is analyzed again.
    private static async Task CheckReanalysisByAnalyzerVersion(string root, DumpIdentityContract identity)
    {
        async Task<AnalysisSummary> Load(string? analyzerVersion, bool withQuickAnswer)
        {
            var node = JsonNode.Parse(BuildSummary(identity))!.AsObject();
            if (analyzerVersion is not null)
            {
                node["analyzer_version"] = analyzerVersion;
            }
            if (withQuickAnswer)
            {
                node["quick_answer"] = new JsonObject
                {
                    ["kind"] = "crash",
                    ["headline"] = "The game crashed to desktop (CTD).",
                };
            }
            var dir = Path.Combine(root, "reanalysis");
            Directory.CreateDirectory(dir);
            var path = Path.Combine(dir, Guid.NewGuid().ToString("N") + ".json");
            await AtomicFile.WriteAllTextAsync(path, node.ToJsonString(), CancellationToken.None);
            return AnalysisSummary.LoadFromSummaryFile(path);
        }

        var viewer = new Version(0, 2, 71, 0);  // AssemblyVersion is the product version + ".0"
        Require((await Load("0.2.71", true)).AnalyzerVersion == "0.2.71", "analyzer_version is read");
        Require(!(await Load("0.2.71", true)).NeedsReanalysisFor(viewer), "the same version is reused");
        Require(!(await Load("0.2.72", true)).NeedsReanalysisFor(viewer), "a newer analyzer's summary is reused");
        Require((await Load("0.2.70", true)).NeedsReanalysisFor(viewer), "an older analyzer's summary is analyzed again");
        Require((await Load("0.2.9", true)).NeedsReanalysisFor(new Version(0, 2, 10, 0)),
            "versions compare as numbers, not text");
        Require((await Load(null, true)).NeedsReanalysisFor(viewer), "no analyzer_version: analyzed again");
        Require((await Load("not a version", true)).NeedsReanalysisFor(viewer), "an unreadable version: analyzed again");
        Require((await Load("0.2.71", false)).NeedsReanalysisFor(viewer), "no quick answer: analyzed again");
        Require(!(await Load(null, true)).NeedsReanalysisFor(null), "no viewer version: only the quick answer decides");
    }

    // ADR-0025: the viewer's fingerprint of crash_history.json matches the
    // analyzer's (analysis_engine_runtime_tests, TestSaveLoadHistoryFingerprint,
    // reads the same file), and a summary keeps the one it was judged on.
    private static async Task CheckSaveLoadHistoryFingerprint(string root, DumpIdentityContract identity)
    {
        var fixture = Path.Combine(
            Directory.GetCurrentDirectory(), "tests", "data", "save_load_history", "crash_history.json");
        Require(File.Exists(fixture), "the shared crash_history.json fixture is found from the repository root");
        Require(
            SaveLoadHistoryFingerprint.Compute(fixture, "dddd") == "9509785e8fa8add1",
            "the viewer's fingerprint matches the analyzer's");
        Require(
            SaveLoadHistoryFingerprint.Compute(fixture, string.Empty) != "9509785e8fa8add1",
            "the rows of the dump being shown are left out");

        var dir = Path.Combine(root, "fingerprint");
        Directory.CreateDirectory(dir);
        Require(
            SaveLoadHistoryFingerprint.Compute(Path.Combine(dir, "missing.json"), "dddd") == "0000000000000000",
            "no history file is an empty history");
        var broken = Path.Combine(dir, "broken.json");
        await File.WriteAllTextAsync(broken, "{ not json");
        Require(SaveLoadHistoryFingerprint.Compute(broken, "dddd") is null, "an unreadable history is not a match");

        var node = JsonNode.Parse(BuildSummary(identity))!.AsObject();
        node["save_load"] = new JsonObject { ["key"] = "0646f9dee25d300b", ["history_fingerprint"] = "9509785e8fa8add1" };
        var path = Path.Combine(dir, "summary.json");
        await AtomicFile.WriteAllTextAsync(path, node.ToJsonString(), CancellationToken.None);
        Require(
            AnalysisSummary.LoadFromSummaryFile(path).SaveLoadHistoryFingerprint == "9509785e8fa8add1",
            "the summary's history fingerprint is read");
        await AtomicFile.WriteAllTextAsync(path, BuildSummary(identity), CancellationToken.None);
        Require(
            AnalysisSummary.LoadFromSummaryFile(path).SaveLoadHistoryFingerprint.Length == 0,
            "a summary without a save load has none");
    }

    private static string BuildSummary(DumpIdentityContract identity)
    {
        var root = new JsonObject
        {
            ["schema"] = new JsonObject
            {
                ["name"] = "SkyrimDiagSummary",
                ["version"] = 2,
            },
            ["dump_identity"] = identity.ToJsonObject(),
            ["summary_sentence"] = "fixture",
            ["crash_bucket_key"] = "fixture",
            ["analysis"] = new JsonObject
            {
                ["is_crash_like"] = true,
                ["is_hang_like"] = false,
                ["is_snapshot_like"] = false,
                ["is_manual_capture"] = false,
                ["is_filtered_clean_exit"] = false,
            },
            ["triage"] = new JsonObject
            {
                ["review_status"] = "unreviewed",
                ["reviewed"] = false,
                ["verdict"] = "",
                ["actual_cause"] = "",
                ["ground_truth_mod"] = "",
                ["notes"] = "",
            },
        };
        return root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + Environment.NewLine;
    }

    // SkyrimDiagHelper.ini decodes as the helper reads it (IniText.h): every
    // BOM, UTF-16LE without one, UTF-8, and bytes that are not UTF-8 in the
    // ANSI code page (a Korean OutputDir saved in CP949).
    private static void CheckIniTextDecoding()
    {
        const string text = "[SkyrimDiagHelper]\r\nOutputDir=D:\\스카이림 로그\r\n";
        byte[] WithPreamble(System.Text.Encoding encoding) =>
            encoding.GetPreamble().Concat(encoding.GetBytes(text)).ToArray();
        Require(IniTextDecoder.Decode(System.Text.Encoding.UTF8.GetBytes(text)) == text, "UTF-8 ini");
        Require(IniTextDecoder.Decode(WithPreamble(System.Text.Encoding.UTF8)) == text, "UTF-8 BOM ini");
        Require(IniTextDecoder.Decode(WithPreamble(System.Text.Encoding.Unicode)) == text, "UTF-16LE BOM ini");
        Require(IniTextDecoder.Decode(System.Text.Encoding.Unicode.GetBytes(text)) == text, "UTF-16LE ini without a BOM");
        Require(IniTextDecoder.Decode(WithPreamble(System.Text.Encoding.BigEndianUnicode)) == text, "UTF-16BE BOM ini");

        // CP949 given explicitly: the machine's own ANSI code page may be UTF-8
        // (the "worldwide language support" option), where no bytes reach the fallback.
        System.Text.Encoding.RegisterProvider(System.Text.CodePagesEncodingProvider.Instance);
        var cp949 = System.Text.Encoding.GetEncoding(949);
        const string sample = "[SkyrimDiagHelper]\r\nOutputDir=D:\\스카이림 로그\r\n";
        Require(IniTextDecoder.Decode(cp949.GetBytes(sample), cp949) == sample,
            "an ini that is not UTF-8 decodes with the ANSI code page");
        Require(IniTextDecoder.AnsiEncoding() is not null, "the system ANSI code page resolves");
        Require(IniTextDecoder.Decode(Array.Empty<byte>()) == string.Empty, "an empty ini");
    }

    private static async Task CheckAdvancedArtifactIdentityAndTailParsing(
        string root,
        string dumpA,
        string dumpB,
        DumpIdentityContract identityA)
    {
        var dir = Path.Combine(root, "compat_artifacts");
        Directory.CreateDirectory(dir);

        var reportPath = NativeAnalyzerBridge.ResolveReportPath(dumpB, dir);
        var wctPath = NativeAnalyzerBridge.ResolveWctPath(dumpB, dir);
        var blackboxPath = NativeAnalyzerBridge.ResolveBlackboxPath(dumpB, dir);

        await File.WriteAllTextAsync(reportPath, "compat report body");
        await File.WriteAllTextAsync(wctPath, "{\"threads\":[{\"tid\":42}]}");
        var bbLines = Enumerable.Range(0, 260)
            .Select(i => $"{{\"i\":{i},\"t_ms\":{i * 10},\"tid\":1,\"type_name\":\"evt\",\"detail\":\"d{i}\"}}");
        await File.WriteAllLinesAsync(blackboxPath, bbLines);

        // Only dump B has been analyzed. Its compatibility files share dump A's stem.
        var dataA = MainWindowViewModel.LoadAdvancedArtifacts(
            dumpA,
            dir,
            identityA,
            "missing report",
            "missing wct",
            CancellationToken.None);
        Require(!dataA.HasReport && dataA.ReportText == "missing report", "report must not come from another same-stem dump");
        Require(!dataA.HasWct && dataA.WctText == "missing wct", "WCT must not come from another same-stem dump");
        Require(dataA.EventCount == 0 && dataA.EventLines.Count == 0, "blackbox must not come from another same-stem dump");

        var data = MainWindowViewModel.LoadAdvancedArtifacts(
            dumpB,
            dir,
            DumpIdentityContract.Invalid,
            "missing report",
            "missing wct",
            CancellationToken.None);
        Require(data.HasReport && data.ReportText == "compat report body", "report must use the compatibility path without an identity");
        Require(data.HasWct && data.WctText.Contains("\"tid\": 42", StringComparison.Ordinal), "WCT must use the compatibility path without an identity");
        Require(data.EventCount == 260, "blackbox must count all 260 events");
        Require(data.EventLines.Count == 200, "blackbox must retain the final 200 events");
        Require(data.EventLines[0].StartsWith("[60]", StringComparison.Ordinal), "tail window must start at event 60");
        Require(data.EventLines[^1].StartsWith("[259]", StringComparison.Ordinal), "tail window must end at event 259");
    }

    private static async Task CheckCrashLoggerLogPathResolution(string root)
    {
        var dir = Path.Combine(root, "cl_resolve");
        Directory.CreateDirectory(dir);
        var directLog = Path.Combine(dir, "crash-2026-11-20-01-00-00.log");
        await File.WriteAllTextAsync(directLog, "Skyrim SSE v1.6.1170");

        Require(
            string.Equals(
                MainWindowViewModel.ResolveCrashLoggerLogPath(directLog, null, null),
                directLog,
                StringComparison.OrdinalIgnoreCase),
            "unredacted existing Crash Logger log path must resolve directly");
        Require(
            string.Equals(
                MainWindowViewModel.ResolveCrashLoggerLogPath("<redacted>\\crash-2026-11-20-01-00-00.log", null, dir),
                directLog,
                StringComparison.OrdinalIgnoreCase),
            "redacted Crash Logger log path must resolve by filename in outDir");
        Require(
            MainWindowViewModel.ResolveCrashLoggerLogPath("<redacted>\\missing-crash.log", null, dir) is null,
            "missing Crash Logger log must resolve to null");
        Require(
            MainWindowViewModel.ResolveCrashLoggerLogPath("<redacted>\\..", null, dir) is null,
            "traversal filename must resolve to null");
    }

    private static async Task CheckCrashLoggerLogPathExtensions(string root)
    {
        var dir = Path.Combine(root, "cl_extensions");
        Directory.CreateDirectory(dir);
        var summaryPath = Path.Combine(dir, "summary.json");
        var batchPath = Path.Combine(dir, "not-a-log.bat");
        var logPath = Path.Combine(dir, "crash.LOG");
        await File.WriteAllTextAsync(batchPath, "fixture");
        await File.WriteAllTextAsync(logPath, "Skyrim SSE v1.6.1170");

        async Task<string?> ResolveSummaryLogPath(string path)
        {
            var summary = new JsonObject
            {
                ["crash_logger"] = new JsonObject { ["log_path"] = path },
            };
            await File.WriteAllTextAsync(summaryPath, summary.ToJsonString());
            var loaded = AnalysisSummary.LoadFromSummaryFile(summaryPath);
            return MainWindowViewModel.ResolveCrashLoggerLogPath(loaded.CrashLoggerLogPath, null, dir);
        }

        Require(await ResolveSummaryLogPath(batchPath) is null, "summary must reject an existing .bat path");
        Require(
            await ResolveSummaryLogPath("<redacted>\\" + Path.GetFileName(batchPath)) is null,
            "summary must reject a matching .bat candidate in outDir");
        Require(await ResolveSummaryLogPath(logPath) == logPath, "summary must accept an existing uppercase .LOG path");
        Require(
            await ResolveSummaryLogPath("<redacted>\\" + Path.GetFileName(logPath)) == logPath,
            "summary must accept a matching uppercase .LOG candidate in outDir");
    }

    private static async Task CheckRecentDumpsDefaultCapIncludesMoreThanTwelve(string root)
    {
        var dir = Path.Combine(root, "many_dumps");
        Directory.CreateDirectory(dir);
        var baseTime = new DateTime(2026, 11, 20, 0, 0, 0, DateTimeKind.Utc);
        for (var i = 0; i < 18; i++)
        {
            var dump = Path.Combine(dir, $"SkyrimDiag_Crash_20261120_0000{i:D2}_000.dmp");
            await File.WriteAllBytesAsync(dump, new byte[] { 0x4D, 0x44, 0x4D, 0x50 });
            File.SetLastWriteTimeUtc(dump, baseTime.AddMinutes(i));
        }

        var state = new DumpDiscoveryState { RegisteredRoots = new List<string> { dir } };
        var dumps = DumpDiscoveryService.DiscoverRecentDumps(state, currentDumpPath: null, isKorean: false);
        Require(dumps.Count >= 18, $"default recent-dump discovery cap must show at least 18 dumps, got {dumps.Count}");
    }

    private static void Require(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
