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

            Require(
                !Directory.EnumerateFiles(root, "*.tmp.*", SearchOption.AllDirectories).Any(),
                "atomic writes left temporary files behind");

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

    private static void Require(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
