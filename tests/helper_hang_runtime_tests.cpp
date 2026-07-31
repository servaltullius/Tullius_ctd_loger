#include <Windows.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>

#include "HangCapture.h"
#include "HangCaptureInternal.h"
#include "HelperLog.h"
#include "HelperRuntimeTestUtils.h"
#include "RetentionWorker.h"
#include "SkyrimDiagHelper/LoadStats.h"

using skydiag::helper::HangDecision;
using skydiag::helper::LoadStats;
using skydiag::helper::internal::ClearLog;
using skydiag::helper::internal::ExecuteConfirmedHangCapture;
using skydiag::helper::internal::HandleHangTick;
using skydiag::helper::internal::HangCaptureState;
using skydiag::helper::internal::HangTickResult;
using skydiag::helper::internal::ShutdownRetentionWorker;
using skydiag::tests::runtime::AssertContains;
using skydiag::tests::runtime::CloseAttachedProcess;
using skydiag::tests::runtime::FindSingleFileByPrefix;
using skydiag::tests::runtime::FindSingleFileWithExt;
using skydiag::tests::runtime::MakeSharedLayout;
using skydiag::tests::runtime::MakeSelfAttachedProcess;
using skydiag::tests::runtime::MakeTempDir;
using skydiag::tests::runtime::MakeTestConfig;
using skydiag::tests::runtime::ReadAllTextUtf8;
using skydiag::tests::runtime::Require;

namespace {

void TestLoadStatsAtomicSaveFailurePreservesExistingState()
{
  const auto outBase = MakeTempDir(L"skydiag_load_stats_atomic_failure");
  const auto statsPath = outBase / L"SkyrimDiag_LoadStats.json";
  constexpr const char* kPriorState =
    "{\n  \"version\": 1,\n  \"loadingSeconds\": [17]\n}\n";
  {
    std::ofstream prior(statsPath, std::ios::binary | std::ios::trunc);
    Require(prior.is_open(), "Failed to create prior load-stats state");
    prior << kPriorState;
    prior.flush();
    Require(prior.good(), "Failed to flush prior load-stats state");
  }

  HANDLE locked = CreateFileW(
    statsPath.c_str(),
    GENERIC_READ,
    FILE_SHARE_READ,
    nullptr,
    OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  Require(locked != INVALID_HANDLE_VALUE, "Failed to lock load-stats state against replacement");

  LoadStats replacement;
  replacement.AddLoadingSampleSeconds(99u);
  const bool saved = replacement.SaveToFile(statsPath);
  CloseHandle(locked);

  Require(!saved, "LoadStats::SaveToFile must propagate atomic replacement failure");
  Require(
    ReadAllTextUtf8(statsPath) == kPriorState,
    "Failed load-stats replacement must preserve the complete prior JSON");

  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(outBase, ec)) {
    Require(!ec, "Failed to enumerate load-stats output directory");
    Require(
      entry.path().filename().wstring().find(L".tmp.") == std::wstring::npos,
      "Failed load-stats replacement must clean its temporary file");
  }

  std::filesystem::remove_all(outBase);
}

void TestHandleHangTick_ReportsStatsPersistenceFailureAndUsesMemorySample()
{
  const auto outBase = MakeTempDir(L"skydiag_load_stats_tick_failure");
  ClearLog(outBase);
  const auto statsPath = outBase / L"SkyrimDiag_LoadStats.json";
  constexpr const char* kPriorState =
    "{\n  \"version\": 1,\n  \"loadingSeconds\": [17]\n}\n";
  {
    std::ofstream prior(statsPath, std::ios::binary | std::ios::trunc);
    Require(prior.is_open(), "Failed to create prior tick load-stats state");
    prior << kPriorState;
    prior.flush();
    Require(prior.good(), "Failed to flush prior tick load-stats state");
  }

  HANDLE locked = CreateFileW(
    statsPath.c_str(),
    GENERIC_READ,
    FILE_SHARE_READ,
    nullptr,
    OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  Require(locked != INVALID_HANDLE_VALUE, "Failed to lock tick load-stats state");

  auto shared = MakeSharedLayout();
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  shared->header.qpc_freq = 10'000'000ull;
  shared->header.last_heartbeat_qpc = static_cast<std::uint64_t>(now.QuadPart);
  shared->header.state_flags = skydiag::kState_None;

  auto proc = MakeSelfAttachedProcess(shared.get());
  auto cfg = MakeTestConfig();
  cfg.adaptiveLoadingMinSec = 1u;
  cfg.adaptiveLoadingMinExtraSec = 5u;
  cfg.adaptiveLoadingMaxSec = 1000u;

  LoadStats loadStats;
  std::uint32_t adaptiveLoadingThresholdSec = cfg.hangThresholdLoadingSec;
  std::wstring pendingHangViewerDumpPath;
  HangCaptureState state{};
  state.wasLoading = true;
  state.loadStartQpc =
    static_cast<std::uint64_t>(now.QuadPart) - (20ull * shared->header.qpc_freq);

  const auto result = HandleHangTick(
    cfg,
    proc,
    outBase,
    &loadStats,
    statsPath,
    &adaptiveLoadingThresholdSec,
    static_cast<std::uint64_t>(now.QuadPart),
    &pendingHangViewerDumpPath,
    &state);
  CloseHandle(locked);

  Require(result == HangTickResult::kContinue, "Stats persistence failure must not stop hang monitoring");
  Require(loadStats.HasSamples(), "Valid loading observation must remain usable in memory");
  Require(
    adaptiveLoadingThresholdSec == 30u,
    "Current helper session must derive its threshold from the valid in-memory sample");
  Require(
    ReadAllTextUtf8(statsPath) == kPriorState,
    "Tick persistence failure must preserve the prior on-disk statistics");

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(
    log,
    "failed to persist adaptive loading statistics",
    "Tick persistence failure must be visible in the helper log");
  AssertContains(
    log,
    "using the new sample in memory",
    "Helper log must state the intentional current-session memory behavior");

  CloseAttachedProcess(&proc);
  std::filesystem::remove_all(outBase);
}

void TestHandleHangTick_SkipsWhenHeartbeatNotInitialized()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_hang_runtime_skip");
  ClearLog(outBase);

  auto shared = MakeSharedLayout();
  shared->header.last_heartbeat_qpc = 0;
  shared->header.qpc_freq = 10'000'000ull;

  auto proc = MakeSelfAttachedProcess(shared.get());
  LoadStats loadStats;
  std::uint32_t adaptiveLoadingThresholdSec = 600u;
  std::wstring pendingHangViewerDumpPath;
  HangCaptureState state{};

  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  const std::uint64_t attachNowQpc =
    static_cast<std::uint64_t>(now.QuadPart) - (11ull * shared->header.qpc_freq);

  const auto result = HandleHangTick(
    MakeTestConfig(),
    proc,
    outBase,
    &loadStats,
    outBase / "load_stats.json",
    &adaptiveLoadingThresholdSec,
    attachNowQpc,
    &pendingHangViewerDumpPath,
    &state);

  Require(result == HangTickResult::kContinue, "Heartbeat-uninitialized path must continue without capture");
  Require(!state.hangCapturedThisEpisode, "Heartbeat-uninitialized path must not mark capture");

  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(outBase, ec)) {
    Require(!ec, "Failed to enumerate output directory");
    Require(entry.path().extension() != L".dmp", "Heartbeat-uninitialized path must not write a dump");
  }

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "heartbeat not initialized", "Heartbeat-uninitialized hang path must log suppression");

  CloseAttachedProcess(&proc);
  std::filesystem::remove_all(outBase);
}

void TestExecuteConfirmedHangCapture_WritesArtifacts()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_hang_runtime_capture");
  ClearLog(outBase);

  auto shared = MakeSharedLayout();
  shared->header.last_heartbeat_qpc = 1;
  shared->header.qpc_freq = 10'000'000ull;

  auto proc = MakeSelfAttachedProcess(shared.get());
  HangDecision decision{};
  decision.isHang = true;
  decision.secondsSinceHeartbeat = 15.0;
  decision.thresholdSec = 10;
  decision.isLoading = false;

  std::wstring pendingHangViewerDumpPath;
  HangCaptureState state{};
  const auto result = ExecuteConfirmedHangCapture(
    MakeTestConfig(),
    proc,
    outBase,
    decision,
    /*stateFlags=*/0u,
    &pendingHangViewerDumpPath,
    &state);

  Require(result == HangTickResult::kContinue, "Confirmed hang capture should continue helper loop");
  Require(state.hangCapturedThisEpisode, "Confirmed hang capture must mark episode captured");
  Require(pendingHangViewerDumpPath.empty(), "Viewer queue should remain empty when auto-open is disabled");

  Require(std::filesystem::exists(FindSingleFileWithExt(outBase, L".dmp")), "Hang capture must write a dump");
  Require(
    std::filesystem::exists(FindSingleFileByPrefix(outBase, L"SkyrimDiag_WCT_", L".json")),
    "Hang capture must write WCT json");
  Require(
    std::filesystem::exists(FindSingleFileByPrefix(outBase, L"SkyrimDiag_Incident_Hang_", L".json")),
    "Hang capture must write incident manifest");

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "Hang dump written", "Confirmed hang capture must log dump creation");
  AssertContains(log, "Incident manifest written", "Confirmed hang capture must log manifest creation");

  ShutdownRetentionWorker();
  CloseAttachedProcess(&proc);
  std::filesystem::remove_all(outBase);
}

}  // namespace

int main()
{
  try {
    TestLoadStatsAtomicSaveFailurePreservesExistingState();
    TestHandleHangTick_ReportsStatsPersistenceFailureAndUsesMemorySample();
    TestHandleHangTick_SkipsWhenHeartbeatNotInitialized();
    TestExecuteConfirmedHangCapture_WritesArtifacts();
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
