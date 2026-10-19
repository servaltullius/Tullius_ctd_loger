#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "CrashCapture.h"
#include "HangCapture.h"
#include "HangCaptureInternal.h"
#include "HelperLog.h"
#include "HelperMainInternal.h"
#include "HelperRuntimeTestUtils.h"
#include "ManualCapture.h"
#include "PssSnapshot.h"
#include "RetentionWorker.h"
#include "SkyrimDiagHelper/DumpProfile.h"
#include "SkyrimDiagHelper/DumpWriter.h"
#include "SkyrimDiagHelper/HangDetect.h"
#include "SkyrimDiagHelper/LoadStats.h"
#include "SkyrimDiagHelper/ModalDialogProbe.h"

using skydiag::helper::HangDecision;
using skydiag::helper::LoadStats;
using skydiag::helper::internal::ClearLog;
using skydiag::helper::internal::ExecuteConfirmedHangCapture;
using skydiag::helper::internal::HandleHangTick;
using skydiag::helper::internal::HandleProcessExitTick;
using skydiag::helper::internal::HangCaptureState;
using skydiag::helper::internal::HangTickResult;
using skydiag::helper::internal::HelperLoopState;
using skydiag::helper::internal::InitializeLoopState;
using skydiag::helper::internal::RunHelperLoop;
using skydiag::helper::internal::ShutdownRetentionWorker;
using skydiag::tests::runtime::AssertContains;
using skydiag::tests::runtime::CloseAttachedProcess;
using skydiag::tests::runtime::FindSingleFileByPrefix;
using skydiag::tests::runtime::FindSingleFileWithExt;
using skydiag::tests::runtime::LaunchSleepingChildProcess;
using skydiag::tests::runtime::MakeAttachedProcessForChild;
using skydiag::tests::runtime::MakeSharedLayout;
using skydiag::tests::runtime::MakeSelfAttachedProcess;
using skydiag::tests::runtime::MakeTempDir;
using skydiag::tests::runtime::MakeTestConfig;
using skydiag::tests::runtime::OpenSelfProcessHandle;
using skydiag::tests::runtime::ReadAllTextUtf8;
using skydiag::tests::runtime::Require;
using skydiag::tests::runtime::TerminateChildProcess;

namespace {

std::uint64_t QpcNow()
{
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  return static_cast<std::uint64_t>(now.QuadPart);
}

std::uint64_t QpcFrequency()
{
  LARGE_INTEGER freq{};
  QueryPerformanceFrequency(&freq);
  return static_cast<std::uint64_t>(freq.QuadPart);
}

std::size_t CountFilesWithExt(const std::filesystem::path& dir, std::wstring_view ext)
{
  std::size_t count = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    count += entry.path().extension() == ext ? 1u : 0u;
  }
  return count;
}

// A shared layout whose heartbeat stopped `staleSec` seconds ago, on the real
// QPC clock HandleHangTick reads.
std::unique_ptr<skydiag::SharedLayout> MakeStaleHeartbeatLayout(std::uint64_t staleSec)
{
  auto shared = MakeSharedLayout();
  shared->header.qpc_freq = QpcFrequency();
  shared->header.last_heartbeat_qpc = QpcNow() - staleSec * shared->header.qpc_freq;
  shared->header.state_flags = skydiag::kState_None;
  return shared;
}

// Hang tests run in the background with no game window: keep the
// not-foreground suppression out of the way.
skydiag::helper::HelperConfig MakeHangTestConfig()
{
  auto cfg = MakeTestConfig();
  cfg.suppressHangWhenNotForeground = false;
  cfg.enableManualCaptureHotkey = false;
  cfg.hangThresholdInGameSec = 10u;
  cfg.hangThresholdInMenuSec = 10u;
  return cfg;
}

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
  Require(!state.modalDialogSeenThisEpisode, "No dialog was on screen at this hang capture");
  Require(pendingHangViewerDumpPath.empty(), "Viewer queue should remain empty when auto-open is disabled");

  Require(std::filesystem::exists(FindSingleFileWithExt(outBase, L".dmp")), "Hang capture must write a dump");
  Require(
    std::filesystem::exists(FindSingleFileByPrefix(outBase, L"SkyrimDiag_WCT_", L".json")),
    "Hang capture must write WCT json");
  Require(
    std::filesystem::exists(FindSingleFileByPrefix(outBase, L"SkyrimDiag_Incident_Hang_", L".json")),
    "Hang capture must write incident manifest");

  const auto wctJson = ReadAllTextUtf8(FindSingleFileByPrefix(outBase, L"SkyrimDiag_WCT_", L".json"));
  AssertContains(wctJson, "\"modal_dialogs\"", "Hang WCT json must always record the visible-dialog probe result");

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "Hang dump written", "Confirmed hang capture must log dump creation");
  AssertContains(log, "Incident manifest written", "Confirmed hang capture must log manifest creation");

  ShutdownRetentionWorker();
  CloseAttachedProcess(&proc);
  std::filesystem::remove_all(outBase);
}

void TestCaptureModalDialogsFindsMessageBoxOwnerThread()
{
  constexpr wchar_t kTitle[] = L"SkyrimDiag modal probe test";
  DWORD dialogTid = 0;
  std::thread dialogThread([&]() {
    dialogTid = GetCurrentThreadId();
    MessageBoxW(nullptr, L"Plugin failed to load.\nSee C:\\Users\\Tester\\log.txt", kTitle, MB_OK | MB_SETFOREGROUND);
  });

  std::vector<skydiag::helper::ModalDialogWindow> dialogs;
  const skydiag::helper::ModalDialogWindow* found = nullptr;
  for (int attempt = 0; attempt < 100 && !found; ++attempt) {
    Sleep(50);
    dialogs = skydiag::helper::CaptureModalDialogs(GetCurrentProcessId());
    for (const auto& dialog : dialogs) {
      if (dialog.title == "SkyrimDiag modal probe test") {
        found = &dialog;
        break;
      }
    }
  }

  Require(skydiag::tests::runtime::CloseDialogAndJoin(dialogThread, kTitle), "Test MessageBox did not close");

  Require(found != nullptr, "Visible MessageBox owned by this process must be captured");
  Require(found->tid == dialogTid, "Captured dialog must report the thread that pumps it");
  Require(
    found->text.find("Plugin failed to load.") != std::string::npos,
    "Captured dialog must include the MessageBox body text");
  const auto json = skydiag::helper::ModalDialogsToJson(dialogs).dump();
  AssertContains(json, "\"tid\"", "Dialog json must include the owner thread id");
  AssertContains(json, "\"owner_disabled\"", "Dialog json must record whether the owner window is disabled");
}

}  // namespace

// Loading at attach is the game starting up (the plugin keeps the flag on
// until its data has loaded). Timing it from start_qpc put a 2-minute sample
// next to every 25 s save load; only a load screen seen start to end counts.
void TestStartupLoadingIsNotALoadingSample()
{
  const auto outBase = MakeTempDir(L"skydiag_startup_loading_sample");
  const auto statsPath = outBase / L"SkyrimDiag_LoadStats.json";

  auto shared = MakeSharedLayout();
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  shared->header.qpc_freq = 10'000'000ull;
  shared->header.start_qpc = static_cast<std::uint64_t>(now.QuadPart) - (120ull * shared->header.qpc_freq);
  shared->header.last_heartbeat_qpc = static_cast<std::uint64_t>(now.QuadPart);
  shared->header.state_flags = skydiag::kState_Loading;

  auto proc = MakeSelfAttachedProcess(shared.get());
  auto cfg = MakeTestConfig();
  HelperLoopState loop{};
  InitializeLoopState(proc, &loop);
  Require(loop.hangState.wasLoading, "attached while loading");

  // The startup load ends.
  shared->header.state_flags = skydiag::kState_None;
  LoadStats loadStats;
  std::uint32_t adaptiveLoadingThresholdSec = cfg.hangThresholdLoadingSec;
  std::wstring pendingHangViewerDumpPath;
  HandleHangTick(
    cfg,
    proc,
    outBase,
    &loadStats,
    statsPath,
    &adaptiveLoadingThresholdSec,
    static_cast<std::uint64_t>(now.QuadPart),
    &pendingHangViewerDumpPath,
    &loop.hangState);
  Require(!loadStats.HasSamples(), "the startup is no loading sample");
  Require(!std::filesystem::exists(statsPath), "nothing is written");
  CloseAttachedProcess(&proc);
  std::error_code ec;
  std::filesystem::remove_all(outBase, ec);
}

// The hang decision alone (nowQpc, lastHeartbeatQpc, qpcFreq, stateFlags,
// thresholdInGameSec, thresholdLoadingSec), at 100 ticks per second.
void TestEvaluateHangDecisions()
{
  using skydiag::helper::EvaluateHang;
  {
    const auto d = EvaluateHang(2'000u, 1'000u, 100u, skydiag::kState_None, 10u, 60u);
    Require(d.isHang && !d.isLoading && d.thresholdSec == 10u && d.secondsSinceHeartbeat == 10.0,
      "10 s without heartbeat in game is a hang at a 10 s threshold");
  }
  Require(!EvaluateHang(1'999u, 1'000u, 100u, skydiag::kState_None, 10u, 60u).isHang, "9.99 s is not");
  {
    const auto d = EvaluateHang(4'000u, 1'000u, 100u, skydiag::kState_Loading, 10u, 60u);
    Require(!d.isHang && d.isLoading && d.thresholdSec == 60u, "a load screen uses the loading threshold");
  }
  Require(EvaluateHang(7'000u, 1'000u, 100u, skydiag::kState_Loading, 10u, 60u).isHang, "a load stuck past it is a hang");
  {
    const auto d = EvaluateHang(2'000u, 1'000u, 0u, skydiag::kState_None, 10u, 60u);
    Require(!d.isHang && d.thresholdSec == 0u, "no QPC frequency: no decision");
  }
  {
    const auto d = EvaluateHang(1'000u, 2'000u, 100u, skydiag::kState_None, 10u, 60u);
    Require(!d.isHang && d.secondsSinceHeartbeat == 0.0, "a heartbeat newer than now is no hang");
  }
  Require(!EvaluateHang(100'000u, 1'000u, 100u, skydiag::kState_None, 0u, 60u).isHang, "a zero threshold never fires");
}

// A heartbeat stalled past the threshold is captured once per episode (dump
// and WCT after the grace check), and the episode ends when it comes back.
void TestHandleHangTick_CapturesAStaleHeartbeatOncePerEpisode()
{
  const auto outBase = MakeTempDir(L"skydiag_hang_auto_capture");
  ClearLog(outBase);
  auto shared = MakeStaleHeartbeatLayout(30u);
  auto proc = MakeSelfAttachedProcess(shared.get());
  const auto cfg = MakeHangTestConfig();
  LoadStats loadStats;
  std::uint32_t adaptiveLoadingThresholdSec = cfg.hangThresholdLoadingSec;
  std::wstring pendingHangViewerDumpPath;
  HangCaptureState state{};
  const auto tick = [&] {
    return HandleHangTick(
      cfg, proc, outBase, &loadStats, outBase / L"SkyrimDiag_LoadStats.json", &adaptiveLoadingThresholdSec,
      QpcNow(), &pendingHangViewerDumpPath, &state);
  };

  Require(tick() == HangTickResult::kContinue, "the capture keeps the loop going");
  Require(state.hangCapturedThisEpisode, "the episode is marked captured");
  Require(std::filesystem::exists(FindSingleFileWithExt(outBase, L".dmp")), "a hang dump is written");
  Require(
    std::filesystem::exists(FindSingleFileByPrefix(outBase, L"SkyrimDiag_WCT_", L".json")),
    "its wait chains are written");

  (void)tick();
  Require(CountFilesWithExt(outBase, L".dmp") == 1u, "the same episode is not captured again");

  shared->header.last_heartbeat_qpc = QpcNow();
  (void)tick();
  Require(!state.hangCapturedThisEpisode, "the heartbeat came back: the episode ended");

  AssertContains(ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log"), "Hang dump written", "the capture is logged");
  ShutdownRetentionWorker();
  CloseAttachedProcess(&proc);
  std::filesystem::remove_all(outBase);
}

// A game that exits during the hang grace wait is left to the exit tick
// (crash drain, exit classification, deferred viewers); it used to end the
// helper loop on the spot and skip all of that.
void TestHandleHangTick_LeavesAnExitDuringTheGraceWaitToTheExitTick()
{
  const auto outBase = MakeTempDir(L"skydiag_hang_grace_exit");
  ClearLog(outBase);
  auto shared = MakeStaleHeartbeatLayout(30u);
  auto child = LaunchSleepingChildProcess();
  auto proc = MakeAttachedProcessForChild(child, shared.get());
  Require(TerminateProcess(child.pi.hProcess, 1u) != FALSE, "TerminateProcess failed");
  Require(WaitForSingleObject(child.pi.hProcess, 5000) == WAIT_OBJECT_0, "the child did not exit");

  const auto cfg = MakeHangTestConfig();
  LoadStats loadStats;
  std::uint32_t adaptiveLoadingThresholdSec = cfg.hangThresholdLoadingSec;
  HelperLoopState loop{};
  const auto result = HandleHangTick(
    cfg, proc, outBase, &loadStats, outBase / L"SkyrimDiag_LoadStats.json", &adaptiveLoadingThresholdSec, QpcNow(),
    &loop.pendingHangViewerDumpPath, &loop.hangState);
  Require(result == HangTickResult::kContinue, "the exit is left to the loop's exit tick");
  Require(CountFilesWithExt(outBase, L".dmp") == 0u, "no hang dump of an exited game");
  Require(HandleProcessExitTick(cfg, proc, outBase, &loop), "the exit tick then handles the exit");

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "exited during grace period", "the skipped hang is logged");
  AssertContains(log, "Target process exited (exit_code=1)", "the exit is classified");
  TerminateChildProcess(&child);
  std::filesystem::remove_all(outBase);
}

// A crash published while a hang is being confirmed goes first: no hang dump,
// and the crash event stays signaled for the crash capture.
void TestHandleHangTick_YieldsToACrashEvent()
{
  const auto outBase = MakeTempDir(L"skydiag_hang_yields_to_crash");
  ClearLog(outBase);
  auto shared = MakeStaleHeartbeatLayout(30u);
  auto proc = MakeSelfAttachedProcess(shared.get());
  proc.crashEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
  Require(proc.crashEvent != nullptr, "CreateEventW failed");

  const auto cfg = MakeHangTestConfig();
  LoadStats loadStats;
  std::uint32_t adaptiveLoadingThresholdSec = cfg.hangThresholdLoadingSec;
  std::wstring pendingHangViewerDumpPath;
  HangCaptureState state{};
  const auto result = HandleHangTick(
    cfg, proc, outBase, &loadStats, outBase / L"SkyrimDiag_LoadStats.json", &adaptiveLoadingThresholdSec, QpcNow(),
    &pendingHangViewerDumpPath, &state);
  Require(result == HangTickResult::kContinue, "the loop goes on to the crash");
  Require(!state.hangCapturedThisEpisode && CountFilesWithExt(outBase, L".dmp") == 0u, "no hang dump");
  Require(WaitForSingleObject(proc.crashEvent, 0) == WAIT_OBJECT_0, "the crash event is left for the crash capture");
  AssertContains(
    ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log"), "crash event arrived during the grace period", "logged");
  CloseAttachedProcess(&proc);
  std::filesystem::remove_all(outBase);
}

// PSS_CAPTURE_VA_CLONE needs PROCESS_CREATE_PROCESS, which the helper's attach
// handle does not have; the opt-in freeze snapshot failed every time with it.
void TestPssSnapshotWorksFromTheAttachHandle()
{
  const auto outBase = MakeTempDir(L"skydiag_pss_snapshot");
  const HANDLE process = OpenSelfProcessHandle();  // the attach handle's rights
  auto snapshot = skydiag::helper::internal::TryCapturePssSnapshotForFreeze(true, process);
  if (!snapshot.used) {
    std::fwprintf(stderr, L"PSS status: %ls\n", snapshot.status.c_str());
  }
  Require(snapshot.used && snapshot.snapshotHandle != nullptr, "the freeze snapshot is captured");

  auto shared = MakeSharedLayout();
  std::wstring err;
  const auto dumpPath = (outBase / L"SkyrimDiag_Hang_pss.dmp").wstring();
  const bool written = skydiag::helper::WriteDumpWithStreams(
    snapshot.snapshotHandle,
    GetCurrentProcessId(),
    dumpPath,
    shared.get(),
    sizeof(skydiag::SharedLayout),
    "{}",
    {},
    /*isCrash=*/false,
    skydiag::helper::ResolveDumpProfile(skydiag::helper::DumpMode::kMini, skydiag::helper::CaptureKind::Hang),
    /*isProcessSnapshot=*/true,
    &err);
  skydiag::helper::internal::ReleasePssSnapshotForFreeze(process, snapshot.snapshotHandle);
  CloseHandle(process);
  if (!written) {
    std::fwprintf(stderr, L"snapshot dump: %ls\n", err.c_str());
  }
  Require(written && std::filesystem::file_size(dumpPath) > 0u, "the hang dump is written from the snapshot");
  std::filesystem::remove_all(outBase);
}

// The manual capture (hotkey) path: its dump and WCT names.
void TestManualCaptureWritesItsArtifacts()
{
  const auto outBase = MakeTempDir(L"skydiag_manual_capture");
  ClearLog(outBase);
  auto shared = MakeSharedLayout();
  shared->header.qpc_freq = QpcFrequency();
  shared->header.last_heartbeat_qpc = QpcNow();
  auto proc = MakeSelfAttachedProcess(shared.get());
  LoadStats loadStats;
  skydiag::helper::internal::DoManualCapture(MakeHangTestConfig(), proc, outBase, loadStats, 600u, L"test hotkey");

  Require(
    std::filesystem::exists(FindSingleFileByPrefix(outBase, L"SkyrimDiag_Manual_", L".dmp")),
    "a manual dump is written");
  Require(
    std::filesystem::exists(FindSingleFileByPrefix(outBase, L"SkyrimDiag_WCT_Manual_", L".json")),
    "its wait chains are written");
  AssertContains(
    ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log"), "Manual capture triggered via test hotkey", "the trigger is logged");
  ShutdownRetentionWorker();
  CloseAttachedProcess(&proc);
  std::filesystem::remove_all(outBase);
}

// A press within 250 ms of the end of the last manual capture starts none.
static_assert(!skydiag::helper::internal::ShouldDebounceManualCapture(1'000u, 0u), "no capture yet");
static_assert(skydiag::helper::internal::ShouldDebounceManualCapture(1'100u, 1'000u), "100 ms after one");
static_assert(!skydiag::helper::internal::ShouldDebounceManualCapture(1'250u, 1'000u), "250 ms after one");
static_assert(!skydiag::helper::internal::ShouldDebounceManualCapture(900u, 1'000u), "a clock behind the last one");
// Holding the chord down must not register a capture per key repeat.
static_assert(
  skydiag::helper::internal::ManualCaptureHotkeyRegistrationModifiers({ MOD_CONTROL | MOD_SHIFT, VK_BACK }) ==
  (MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT));

// The helper loop end to end: the game exits during the hang grace wait, and
// the loop still reaches its exit handling.
void TestRunHelperLoop_HandlesAnExitDuringTheHangGraceWait()
{
  const auto outBase = MakeTempDir(L"skydiag_loop_grace_exit");
  ClearLog(outBase);
  auto shared = MakeStaleHeartbeatLayout(30u);
  auto child = LaunchSleepingChildProcess();
  auto proc = MakeAttachedProcessForChild(child, shared.get());
  const auto cfg = MakeHangTestConfig();
  LoadStats loadStats;
  std::uint32_t adaptiveLoadingThresholdSec = cfg.hangThresholdLoadingSec;
  HelperLoopState loop{};
  InitializeLoopState(proc, &loop);

  // The first hang tick starts its 1.5 s grace wait about 250 ms in.
  std::thread killer([&] {
    Sleep(800);
    TerminateProcess(child.pi.hProcess, 3u);
  });
  RunHelperLoop(
    cfg, proc, outBase, &loadStats, outBase / L"SkyrimDiag_LoadStats.json", &adaptiveLoadingThresholdSec, QpcNow(),
    &loop);
  killer.join();

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "exited during grace period", "the exit came during the grace wait");
  AssertContains(log, "Target process exited (exit_code=3)", "the loop still handled the exit");
  Require(CountFilesWithExt(outBase, L".dmp") == 0u, "no hang dump");
  TerminateChildProcess(&child);
  std::filesystem::remove_all(outBase);
}

// While a captured crash keeps the game frozen (the heartbeat has not moved
// since the latch), the stalled heartbeat is that crash, not a hang: it used
// to get a second, hang dump of the crashing game and a freeze viewer.
void TestRunHelperLoop_HoldsHangDetectionWhileACapturedCrashFreezesTheGame()
{
  const auto outBase = MakeTempDir(L"skydiag_loop_crash_hold");
  ClearLog(outBase);
  auto shared = MakeStaleHeartbeatLayout(30u);
  shared->header.crash_seq = 2u;
  shared->header.crash.exception_code = 0xC0000005u;
  shared->header.state_flags = skydiag::kState_Frozen;
  auto child = LaunchSleepingChildProcess();
  auto proc = MakeAttachedProcessForChild(child, shared.get());
  const auto cfg = MakeHangTestConfig();
  LoadStats loadStats;
  std::uint32_t adaptiveLoadingThresholdSec = cfg.hangThresholdLoadingSec;
  HelperLoopState loop{};
  loop.crashCaptured.latched = true;
  loop.crashCaptured.latchedHeartbeatQpc = shared->header.last_heartbeat_qpc;
  loop.crashCaptured.capturedInfo =
    skydiag::helper::internal::BuildCrashEventInfo(0xC0000005u, 0x1234u, 4242u, skydiag::kState_Frozen);
  loop.crashCaptured.capturedInfo.crashSeq = 2u;

  // Long enough for a hang tick's grace wait and dump (about 2 s).
  std::thread killer([&] {
    Sleep(3000);
    TerminateProcess(child.pi.hProcess, 0xC0000005u);
  });
  RunHelperLoop(
    cfg, proc, outBase, &loadStats, outBase / L"SkyrimDiag_LoadStats.json", &adaptiveLoadingThresholdSec, QpcNow(),
    &loop);
  killer.join();

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  Require(CountFilesWithExt(outBase, L".dmp") == 0u, "no hang dump of the crashing game");
  Require(log.find("Hang detected") == std::string::npos, "no hang was detected");
  AssertContains(log, "Target process exited", "the loop ran to the exit");
  TerminateChildProcess(&child);
  std::filesystem::remove_all(outBase);
}

int main()
{
  try {
    TestEvaluateHangDecisions();
    TestHandleHangTick_CapturesAStaleHeartbeatOncePerEpisode();
    TestHandleHangTick_LeavesAnExitDuringTheGraceWaitToTheExitTick();
    TestHandleHangTick_YieldsToACrashEvent();
    TestPssSnapshotWorksFromTheAttachHandle();
    TestManualCaptureWritesItsArtifacts();
    TestRunHelperLoop_HandlesAnExitDuringTheHangGraceWait();
    TestRunHelperLoop_HoldsHangDetectionWhileACapturedCrashFreezesTheGame();
    TestLoadStatsAtomicSaveFailurePreservesExistingState();
    TestHandleHangTick_ReportsStatsPersistenceFailureAndUsesMemorySample();
    TestHandleHangTick_SkipsWhenHeartbeatNotInitialized();
    TestStartupLoadingIsNotALoadingSample();
    TestExecuteConfirmedHangCapture_WritesArtifacts();
    TestCaptureModalDialogsFindsMessageBoxOwnerThread();
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
