#include <Windows.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>

#include "HelperLog.h"
#include "HelperMainInternal.h"
#include "HelperRuntimeTestUtils.h"

using skydiag::helper::HelperConfig;
using skydiag::helper::internal::ClearLog;
using skydiag::helper::internal::CleanupCrashArtifactsAfterZeroExit;
using skydiag::helper::internal::LaunchDeferredViewersAfterExit;
using skydiag::tests::runtime::AssertContains;
using skydiag::tests::runtime::FileExists;
using skydiag::tests::runtime::MakeTempDir;
using skydiag::tests::runtime::MakeTestConfig;
using skydiag::tests::runtime::ReadAllTextUtf8;
using skydiag::tests::runtime::Require;
using skydiag::tests::runtime::WriteAllTextUtf8;

namespace {

void TestCleanupCrashArtifactsAfterZeroExit_RemovesArtifactsOnWeakExit()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_false_positive_cleanup");
  ClearLog(outBase);

  const auto dumpPath = outBase / "SkyrimDiag_Crash_20260315_130000_001.dmp";
  const auto stem = dumpPath.stem().wstring();
  const auto etwPath = outBase / (stem + L".etl");
  const auto reportPath = outBase / (stem + L"_SkyrimDiagReport.txt");
  const auto summaryPath = outBase / (stem + L"_SkyrimDiagSummary.json");
  const auto blackboxPath = outBase / (stem + L"_SkyrimDiagBlackbox.jsonl");
  const auto manifestPath = outBase / "SkyrimDiag_Incident_Crash_20260315_130000_001.json";

  WriteAllTextUtf8(dumpPath, "dump");
  WriteAllTextUtf8(etwPath, "etl");
  WriteAllTextUtf8(reportPath, "report");
  WriteAllTextUtf8(summaryPath, "summary");
  WriteAllTextUtf8(blackboxPath, "blackbox");
  WriteAllTextUtf8(manifestPath, "manifest");

  HelperConfig cfg = MakeTestConfig();
  skydiag::helper::AttachedProcess proc{};
  skydiag::helper::internal::HelperLoopState state{};
  state.crashCaptured = true;
  state.capturedCrashDumpPath = dumpPath.wstring();
  state.pendingCrashViewerDumpPath = dumpPath.wstring();
  state.pendingCrashEtw.etwPath = etwPath;

  CleanupCrashArtifactsAfterZeroExit(
    cfg,
    proc,
    outBase,
    /*exitCode0StrongCrash=*/false,
    /*exceptionCode=*/0xE06D7363u,
    &state);

  Require(!FileExists(dumpPath), "Weak zero-exit crash must delete dump");
  Require(!FileExists(etwPath), "Weak zero-exit crash must delete ETW sidecar");
  Require(!FileExists(reportPath), "Weak zero-exit crash must delete report");
  Require(!FileExists(summaryPath), "Weak zero-exit crash must delete summary");
  Require(!FileExists(blackboxPath), "Weak zero-exit crash must delete blackbox");
  Require(!FileExists(manifestPath), "Weak zero-exit crash must delete incident manifest");
  Require(!state.crashCaptured, "Weak zero-exit crash must clear crashCaptured state");
  Require(state.capturedCrashDumpPath.empty(), "Weak zero-exit crash must clear captured dump path");
  Require(state.pendingCrashViewerDumpPath.empty(), "Weak zero-exit crash must clear deferred viewer path");

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "removed", "Weak zero-exit crash cleanup must log artifact removal");

  std::filesystem::remove_all(outBase);
}

void TestLaunchDeferredViewersAfterExit_SuppressesOnNormalExit()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_false_positive_viewer");
  ClearLog(outBase);

  HelperConfig cfg = MakeTestConfig();
  cfg.autoOpenViewerOnCrash = true;

  skydiag::helper::internal::HelperLoopState state{};
  state.pendingCrashViewerDumpPath = L"C:\\Temp\\SyntheticCrash.dmp";

  LaunchDeferredViewersAfterExit(
    cfg,
    outBase,
    /*exitCode=*/0,
    /*exitCode0StrongCrash=*/false,
    &state);

  Require(state.pendingCrashViewerDumpPath.empty(), "Normal exit must suppress deferred crash viewer");

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(
    log,
    "Suppressed deferred crash viewer launch on normal process exit",
    "Normal exit must log deferred crash viewer suppression");

  std::filesystem::remove_all(outBase);
}

}  // namespace

int main()
{
  try {
    TestCleanupCrashArtifactsAfterZeroExit_RemovesArtifactsOnWeakExit();
    TestLaunchDeferredViewersAfterExit_SuppressesOnNormalExit();
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
