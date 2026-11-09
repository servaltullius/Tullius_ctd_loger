#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "AddressLibraryFile.h"
#include "CrashCapture.h"
#include "FreezeCapture.h"
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
#include "SkyrimDiagHelper/WctCapture.h"

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
  Require(nlohmann::json::parse(wctJson)["capture"]["kind"] == "hang", "the capture block names a hang");
  const auto manifest = nlohmann::json::parse(
    ReadAllTextUtf8(FindSingleFileByPrefix(outBase, L"SkyrimDiag_Incident_Hang_", L".json")));
  Require(
    manifest["context"]["seconds_since_heartbeat"] == 15.0 && manifest["context"].contains("dump_transport") &&
      !manifest["context"].contains("trigger"),
    "the hang manifest context");

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "Hang dump written", "Confirmed hang capture must log dump creation");
  AssertContains(log, "Incident manifest written", "Confirmed hang capture must log manifest creation");

  ShutdownRetentionWorker();
  CloseAttachedProcess(&proc);
  std::filesystem::remove_all(outBase);
}

// A hang dump of a game that then exits, as `exitCode`, with the hang still
// open: what HeldHangDump leaves in the folder and the log.
struct HeldHangExit
{
  std::size_t dumps = 0;
  std::size_t jsonFiles = 0;
  bool heldCleared = false;
  bool viewerCleared = false;
  bool heldAtCapture = false;
  bool slowShutdownAtExit = false;
  std::string log;
};

// A process created suspended has only its first thread: the thread count a
// game in ExitProcess shows (all other threads ended, the DLLs detaching).
skydiag::tests::runtime::ChildProcess LaunchSuspendedChildProcess()
{
  skydiag::tests::runtime::ChildProcess child{};
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  std::wstring commandLine = L"cmd.exe /c exit 0";
  const auto applicationPath = skydiag::tests::runtime::GetCmdExePath();
  const BOOL ok = CreateProcessW(
    applicationPath.c_str(),
    commandLine.data(),
    nullptr,
    nullptr,
    FALSE,
    CREATE_SUSPENDED | CREATE_NO_WINDOW,
    nullptr,
    nullptr,
    &si,
    &child.pi);
  Require(ok != FALSE, "CreateProcessW(cmd.exe, suspended) failed");
  CloseHandle(child.pi.hThread);
  child.pi.hThread = nullptr;
  return child;
}

// What happens between the capture and the exit.
enum class BeforeExit
{
  kNothing,
  // The heartbeat comes back but no hang tick runs before the exit.
  kHeartbeatReturns,
  // A hang tick runs on the still-stalled heartbeat, with the pid of a
  // one-thread process: the closing check would skip it if it ran, and the
  // tick notes that the game has entered its exit.
  kHangTick,
  // The flags at the exit show a load (whose threshold is 600 s here).
  kLoadFlagAtExit,
  // A dialog window of the game was visible at the capture.
  kDialogSeen,
  // A load flag appears while the heartbeat stays stopped, and a hang tick
  // runs: under the 600 s loading threshold the stall is no hang any more.
  kLoadFlagThenHangTick,
};

HeldHangExit CaptureHangThenExit(
  skydiag::helper::HelperConfig cfg,
  DWORD exitCode,
  const wchar_t* dirPrefix,
  std::uint32_t stateFlags = skydiag::kState_InMenu,
  BeforeExit beforeExit = BeforeExit::kNothing)
{
  const auto outBase = MakeTempDir(dirPrefix);
  ClearLog(outBase);
  auto shared = MakeStaleHeartbeatLayout(32u);
  shared->header.state_flags = stateFlags;
  auto child = LaunchSleepingChildProcess();
  auto proc = MakeAttachedProcessForChild(child, shared.get());
  HelperLoopState loop{};
  InitializeLoopState(proc, outBase, &loop);
  HangDecision decision{};
  decision.isHang = true;
  decision.secondsSinceHeartbeat = 32.0;
  decision.thresholdSec = 30u;
  (void)ExecuteConfirmedHangCapture(
    cfg, proc, outBase, decision, stateFlags, &loop.pendingHangViewerDumpPath, &loop.hangState);
  Require(CountFilesWithExt(outBase, L".dmp") == 1u, "the hang dump is written");
  const bool heldAtCapture = !loop.hangState.heldDump.dumpPath.empty();
  if (beforeExit == BeforeExit::kHeartbeatReturns) {
    shared->header.last_heartbeat_qpc = QpcNow();
  } else if (beforeExit == BeforeExit::kHangTick) {
    auto oneThread = LaunchSuspendedChildProcess();
    const auto gamePid = proc.pid;
    proc.pid = oneThread.pi.dwProcessId;
    LoadStats loadStats;
    std::uint32_t adaptiveLoadingThresholdSec = cfg.hangThresholdLoadingSec;
    (void)HandleHangTick(
      cfg, proc, outBase, &loadStats, outBase / L"SkyrimDiag_LoadStats.json", &adaptiveLoadingThresholdSec,
      QpcNow(), &loop.pendingHangViewerDumpPath, &loop.hangState);
    proc.pid = gamePid;
    TerminateChildProcess(&oneThread);
    Require(
      loop.hangState.heldDump.dumpPath.empty() == !heldAtCapture, "a held dump stays held while the hang lasts");
    Require(!loop.hangState.hangSkippedAsClosingThisEpisode, "a captured episode is not checked as closing again");
    Require(loop.hangState.closingSeenAfterCaptureThisEpisode, "the tick notes that the game entered its exit");
  } else if (beforeExit == BeforeExit::kLoadFlagAtExit) {
    shared->header.state_flags = skydiag::kState_InMenu | skydiag::kState_Loading;
  } else if (beforeExit == BeforeExit::kDialogSeen) {
    loop.hangState.modalDialogSeenThisEpisode = true;
  } else if (beforeExit == BeforeExit::kLoadFlagThenHangTick) {
    shared->header.state_flags = skydiag::kState_InMenu | skydiag::kState_Loading;
    LoadStats loadStats;
    std::uint32_t adaptiveLoadingThresholdSec = cfg.hangThresholdLoadingSec;
    (void)HandleHangTick(
      cfg, proc, outBase, &loadStats, outBase / L"SkyrimDiag_LoadStats.json", &adaptiveLoadingThresholdSec,
      QpcNow(), &loop.pendingHangViewerDumpPath, &loop.hangState);
    Require(loop.hangState.hangCapturedThisEpisode, "the heartbeat has not moved: the episode is still open");
    Require(
      loop.hangState.heldDump.dumpPath.empty() == !heldAtCapture, "and its dump is not released as recovered");
  }

  Require(TerminateProcess(child.pi.hProcess, exitCode) != FALSE, "TerminateProcess failed");
  Require(WaitForSingleObject(child.pi.hProcess, 5000) == WAIT_OBJECT_0, "the child did not exit");
  Require(HandleProcessExitTick(cfg, proc, outBase, &loop), "the exit tick handles the exit");

  HeldHangExit result{};
  result.dumps = CountFilesWithExt(outBase, L".dmp");
  result.jsonFiles = CountFilesWithExt(outBase, L".json");
  result.heldCleared = loop.hangState.heldDump.dumpPath.empty();
  result.viewerCleared = loop.pendingHangViewerDumpPath.empty();
  result.heldAtCapture = heldAtCapture;
  result.slowShutdownAtExit = loop.slowShutdownAtExit;
  result.log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  ShutdownRetentionWorker();
  TerminateChildProcess(&child);
  std::filesystem::remove_all(outBase);
  return result;
}

// A freeze that lasts until a normal exit was a slow shutdown: its dump, WCT
// file and incident manifest go before anything analyzes it, and no viewer
// is queued for it any more.
void TestHeldHangDump_RemovedForASlowShutdown()
{
  auto cfg = MakeHangTestConfig();
  cfg.autoOpenViewerOnHang = true;
  cfg.autoOpenHangAfterProcessExit = true;
  cfg.autoOpenHangDelayMs = 0u;
  const auto r = CaptureHangThenExit(cfg, 0u, L"skydiag_held_hang_slow_shutdown");
  Require(r.heldAtCapture, "a capture in a menu that no viewer opened is held unanalyzed");
  Require(r.dumps == 0u, "the dump is removed");
  Require(r.jsonFiles == 0u, "its WCT file and manifest are removed");
  Require(r.heldCleared && r.viewerCleared, "nothing is left to analyze or open");
  AssertContains(r.log, "Removed the hang dump of a slow shutdown", "the removal is logged");
  Require(r.log.find("the hang dump is kept") == std::string::npos, "not also logged as kept");
}

// An exit with another code is a frozen game that was ended: the dump stays.
void TestHeldHangDump_KeptForAnEndedGame()
{
  const auto r = CaptureHangThenExit(MakeHangTestConfig(), 1u, L"skydiag_held_hang_ended");
  Require(r.dumps == 1u, "the dump stays");
  Require(r.heldCleared, "it is released for analysis");
  Require(r.log.find("Removed the hang dump") == std::string::npos, "nothing is removed");
}

// KeepSlowShutdownHangDumps=1 keeps the dump of a slow shutdown.
void TestHeldHangDump_KeptWhenAsked()
{
  auto cfg = MakeHangTestConfig();
  cfg.keepSlowShutdownHangDumps = true;
  const auto r = CaptureHangThenExit(cfg, 0u, L"skydiag_held_hang_keep");
  Require(r.dumps == 1u && r.heldCleared, "the dump stays and is released for analysis");
  Require(r.log.find("Removed the hang dump") == std::string::npos, "nothing is removed");
  Require(r.log.find("its dump is removed") == std::string::npos, "the hold line does not claim a removal");
  AssertContains(r.log, "kept, KeepSlowShutdownHangDumps=1", "the hold line says it is kept");
}

// A freeze during play is no slow shutdown, even when something ends the game
// with exit code 0 (a quit hotkey, a launcher): it is not held, so its report
// is written at once, and the exit leaves its dump.
void TestHeldHangDump_GameplayFreezeIsNotHeld()
{
  const auto r = CaptureHangThenExit(MakeHangTestConfig(), 0u, L"skydiag_held_hang_gameplay", skydiag::kState_None);
  Require(!r.heldAtCapture, "a capture during play is analyzed at once, not held");
  Require(r.dumps == 1u, "its dump stays at exit code 0");
  Require(!r.slowShutdownAtExit, "not a slow shutdown, so its exit viewer is not suppressed");
  Require(r.log.find("Removed the hang dump") == std::string::npos, "nothing is removed");
}

// A menu freeze that recovers just before a quick quit, before a hang tick
// saw the recovery: the heartbeat at the exit says it was no slow shutdown.
void TestHeldHangDump_KeptWhenTheGameRecoveredBeforeAQuickQuit()
{
  const auto r = CaptureHangThenExit(
    MakeHangTestConfig(), 0u, L"skydiag_held_hang_recovered_quit", skydiag::kState_InMenu,
    BeforeExit::kHeartbeatReturns);
  Require(r.heldAtCapture && !r.slowShutdownAtExit, "held, but the exit is no slow shutdown");
  Require(r.dumps == 1u && r.heldCleared, "the dump stays and is released for analysis");
  Require(r.log.find("Removed the hang dump") == std::string::npos, "nothing is removed");
}

// #267 and #268 together: a held menu capture is not checked again as a
// closing game (its episode is captured), and its normal exit removes it.
void TestHeldHangDump_HeldEpisodeIsNotSkippedAsClosing()
{
  const auto r = CaptureHangThenExit(
    MakeHangTestConfig(), 0u, L"skydiag_held_hang_then_tick", skydiag::kState_InMenu, BeforeExit::kHangTick);
  Require(r.slowShutdownAtExit && r.dumps == 0u, "still a slow shutdown at the exit, and removed");
}

// The stall at the exit is judged by the threshold the hang was captured at
// (30 s here), not by the flags left at the exit: a load flag there would
// switch to the 600 s loading threshold and hide the slow shutdown.
void TestHeldHangDump_StallJudgedByTheCaptureThreshold()
{
  auto cfg = MakeHangTestConfig();
  cfg.hangThresholdLoadingSec = 600u;
  const auto r = CaptureHangThenExit(
    cfg, 0u, L"skydiag_held_hang_load_flag", skydiag::kState_InMenu, BeforeExit::kLoadFlagAtExit);
  Require(r.slowShutdownAtExit && r.dumps == 0u, "a slow shutdown, removed");
}

// The thresholds follow the game's flags, which can change while the
// heartbeat stays stopped. A load flag before the exit made the stall "no
// hang" under the 600 s loading threshold: the episode was reset, the held
// dump released as recovered, and the slow shutdown's dump stayed.
void TestHeldHangDump_EpisodeEndsOnlyWhenTheHeartbeatMoves()
{
  auto cfg = MakeHangTestConfig();
  cfg.hangThresholdLoadingSec = 600u;
  const auto r = CaptureHangThenExit(
    cfg, 0u, L"skydiag_held_hang_flag_tick", skydiag::kState_InMenu, BeforeExit::kLoadFlagThenHangTick);
  Require(r.heldAtCapture && r.slowShutdownAtExit && r.dumps == 0u, "still a slow shutdown, removed");
  Require(r.log.find("the game recovered") == std::string::npos, "never told as recovered");
}

// A dialog window at the capture is no slow shutdown: a quitting game shows
// none, and the dump says whose dialog it was (a plugin's error message that
// ends the game with code 0 once it is dismissed).
void TestHeldHangDump_KeptWhenADialogWasVisible()
{
  const auto r = CaptureHangThenExit(
    MakeHangTestConfig(), 0u, L"skydiag_held_hang_dialog", skydiag::kState_InMenu, BeforeExit::kDialogSeen);
  Require(r.heldAtCapture && !r.slowShutdownAtExit, "held, but the exit is no slow shutdown");
  Require(r.dumps == 1u && r.heldCleared, "the dump stays and is released for analysis");
  Require(r.log.find("Removed the hang dump") == std::string::npos, "nothing is removed");
}

// A quit that starts outside a menu (Alt+F4 during play) and stalls is
// captured as a freeze during play: analyzed at once, its dump kept. The game
// is then seen in its exit, so its normal exit opens no freeze viewer.
void TestGameplayCaptureThenTheGamesExitIsASlowShutdown()
{
  auto cfg = MakeHangTestConfig();
  cfg.autoOpenViewerOnHang = true;
  cfg.autoOpenHangAfterProcessExit = true;
  cfg.autoOpenHangDelayMs = 0u;
  const auto r = CaptureHangThenExit(
    cfg, 0u, L"skydiag_hang_play_then_exit", skydiag::kState_None, BeforeExit::kHangTick);
  Require(!r.heldAtCapture && r.dumps == 1u, "a capture during play is not held, and its dump stays");
  Require(r.slowShutdownAtExit, "seen in its exit, then a normal exit: a slow shutdown");
  AssertContains(r.log, "The game entered its exit after the hang capture", "the exit is noted");
  AssertContains(r.log, "Suppressed hang viewer launch", "no freeze viewer for it");
}

static_assert(skydiag::helper::internal::MayBeSlowShutdown(skydiag::kState_InMenu, false), "a menu");
static_assert(!skydiag::helper::internal::MayBeSlowShutdown(skydiag::kState_None, false), "play");
static_assert(
  !skydiag::helper::internal::MayBeSlowShutdown(skydiag::kState_InMenu | skydiag::kState_Loading, false), "a load");
static_assert(!skydiag::helper::internal::MayBeSlowShutdown(skydiag::kState_InMenu, true), "a load by the decision");

// The exit's hang viewer and the held dump: a viewer that opened analyzes it;
// one that did not leaves its analysis to the helper.
void TestHeldHangDump_SettledByTheExitViewer()
{
  const auto outBase = MakeTempDir(L"skydiag_held_hang_exit_viewer");
  ClearLog(outBase);
  // AutoAnalyzeDump=0 here: the release logs, and starts no analyzer next to the test.
  const auto cfg = MakeHangTestConfig();
  const std::wstring dump = (outBase / L"SkyrimDiag_Hang_20261107_120000_000.dmp").wstring();
  HelperLoopState loop{};
  loop.pendingHangViewerDumpPath = dump;

  loop.hangState.heldDump = skydiag::helper::internal::HeldHangDump{ dump, true };
  skydiag::helper::internal::SettleHeldHangDumpAfterExitViewer(cfg, outBase, /*viewerLaunched=*/true, &loop);
  Require(loop.hangState.heldDump.dumpPath.empty(), "an opened viewer takes it");
  Require(
    !std::filesystem::exists(outBase / "SkyrimDiagHelper.log") ||
      ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log").find("Analyzing the held hang dump") == std::string::npos,
    "and the helper does not analyze it too");

  loop.hangState.heldDump = skydiag::helper::internal::HeldHangDump{ dump, true };
  skydiag::helper::internal::SettleHeldHangDumpAfterExitViewer(cfg, outBase, /*viewerLaunched=*/false, &loop);
  Require(loop.hangState.heldDump.dumpPath.empty(), "released");
  AssertContains(
    ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log"), "Analyzing the held hang dump (the hang viewer did not open)",
    "a viewer that did not open leaves the analysis to the helper");
  std::filesystem::remove_all(outBase);
}

// A helper that stops before the game's exit settled a held dump analyzes it.
void TestHeldHangDump_ReleasedWhenTheHelperStops()
{
  const auto outBase = MakeTempDir(L"skydiag_held_hang_helper_stops");
  ClearLog(outBase);
  auto shared = MakeStaleHeartbeatLayout(32u);
  auto proc = MakeSelfAttachedProcess(shared.get());
  const auto cfg = MakeHangTestConfig();
  HelperLoopState loop{};
  loop.hangState.heldDump = skydiag::helper::internal::HeldHangDump{
    (outBase / L"SkyrimDiag_Hang_20261107_120000_000.dmp").wstring(), true };
  skydiag::helper::internal::ShutdownLoopState(cfg, proc, outBase, &loop);
  Require(loop.hangState.heldDump.dumpPath.empty(), "released");
  AssertContains(
    ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log"), "Analyzing the held hang dump (the helper is stopping)",
    "its analysis is started");
  CloseAttachedProcess(&proc);
  std::filesystem::remove_all(outBase);
}

// A freeze the game comes back from is not a slow shutdown: the heartbeat's
// return releases its dump for analysis, so the exit cannot remove it.
void TestHeldHangDump_ReleasedWhenTheGameRecovers()
{
  const auto outBase = MakeTempDir(L"skydiag_held_hang_recovered");
  ClearLog(outBase);
  auto shared = MakeStaleHeartbeatLayout(32u);
  auto proc = MakeSelfAttachedProcess(shared.get());
  const auto cfg = MakeHangTestConfig();
  LoadStats loadStats;
  std::uint32_t adaptiveLoadingThresholdSec = cfg.hangThresholdLoadingSec;
  std::wstring pendingHangViewerDumpPath;
  HangCaptureState state{};
  state.heldDump = skydiag::helper::internal::HeldHangDump{ (outBase / L"SkyrimDiag_Hang_20261107_120000_000.dmp").wstring(), false };
  state.hangCapturedThisEpisode = true;
  shared->header.last_heartbeat_qpc = QpcNow();
  (void)HandleHangTick(
    cfg, proc, outBase, &loadStats, outBase / L"SkyrimDiag_LoadStats.json", &adaptiveLoadingThresholdSec, QpcNow(),
    &pendingHangViewerDumpPath, &state);
  Require(state.heldDump.dumpPath.empty() && !state.hangCapturedThisEpisode, "the episode ended and released it");
  CloseAttachedProcess(&proc);
  std::filesystem::remove_all(outBase);
}

// WM_GETTEXT messages the probe-text test dialog received.
std::atomic<int> g_probeTextGetTextCount{ 0 };

INT_PTR CALLBACK CountGetTextDialogProc(HWND, UINT message, WPARAM, LPARAM)
{
  if (message == WM_GETTEXT) {
    g_probeTextGetTextCount.fetch_add(1);
  }
  return message == WM_INITDIALOG ? TRUE : FALSE;
}

// The probe reads a dialog of its own process without WM_GETTEXT: a sent
// message would make the dialog thread handle it right before the dump the
// self-process tests take (the "Main-thread stack must match the MessageBox
// wait chain" flake). A real dialog (class #32770) from an in-memory template
// counts the WM_GETTEXT it gets while CaptureModalDialogs reads it.
void TestCaptureModalDialogsSendsNoMessageToOwnDialogs()
{
  constexpr wchar_t kTitle[] = L"SkyrimDiag probe text test";
  struct alignas(4) Template
  {
    DLGTEMPLATE dialog;
    WORD menu;
    WORD windowClass;
    wchar_t title[std::size(kTitle)];
  } tmpl{};
  tmpl.dialog.style = WS_POPUP | WS_CAPTION | WS_VISIBLE;
  // No taskbar button, whose title the shell reads with WM_GETTEXT.
  tmpl.dialog.dwExtendedStyle = WS_EX_TOOLWINDOW;
  tmpl.dialog.cx = 120;
  tmpl.dialog.cy = 40;
  std::copy(std::begin(kTitle), std::end(kTitle), tmpl.title);

  HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Require(ready != nullptr, "CreateEventW failed");
  DWORD dialogTid = 0;
  HWND dialog = nullptr;
  std::thread dialogThread([&]() {
    dialogTid = GetCurrentThreadId();
    dialog = CreateDialogIndirectParamW(
      GetModuleHandleW(nullptr), &tmpl.dialog, nullptr, CountGetTextDialogProc, 0);
    // The message queue exists before `ready`, so the WM_QUIT posted at the
    // end arrives even if the dialog could not be created.
    MSG peek{};
    PeekMessageW(&peek, nullptr, 0, 0, PM_NOREMOVE);
    SetEvent(ready);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      if (!dialog || !IsDialogMessageW(dialog, &msg)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
    }
    if (dialog) {
      DestroyWindow(dialog);
    }
  });
  Require(WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0, "the dialog thread did not start");
  CloseHandle(ready);

  bool found = false;
  int getTextDuringProbe = 0;
  if (dialog) {
    // Other programs can read a new window's title with WM_GETTEXT right
    // after it appears; wait until 100 ms pass without one.
    for (int attempt = 0; attempt < 50; ++attempt) {
      g_probeTextGetTextCount = 0;
      Sleep(100);
      if (g_probeTextGetTextCount.load() == 0) {
        break;
      }
    }
    g_probeTextGetTextCount = 0;
    for (const auto& probed : skydiag::helper::CaptureModalDialogs(GetCurrentProcessId())) {
      found = found || (probed.tid == dialogTid && probed.title == "SkyrimDiag probe text test");
    }
    getTextDuringProbe = g_probeTextGetTextCount.load();
  }
  PostThreadMessageW(dialogTid, WM_QUIT, 0, 0);
  dialogThread.join();

  Require(dialog != nullptr, "the test dialog was not created");
  Require(found, "the probe finds the dialog and reads its title");
  Require(getTextDuringProbe == 0, "reading a dialog of this process sends it no WM_GETTEXT");
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
  InitializeLoopState(proc, outBase, &loop);
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
// IsGameProcessClosing: exactly one thread left is a game in ExitProcess; none
// is a process already gone, however it went.
static_assert(skydiag::helper::IsGameProcessClosing(1u), "one thread left");
static_assert(!skydiag::helper::IsGameProcessClosing(0u), "no threads: already gone, not closing");
static_assert(!skydiag::helper::IsGameProcessClosing(2u), "a running process has more");
static_assert(!skydiag::helper::IsGameProcessClosing(std::nullopt), "an unknown count is not closing");


void TestCountProcessThreads()
{
  using skydiag::helper::internal::CountProcessThreads;
  auto child = LaunchSuspendedChildProcess();
  const auto childThreads = CountProcessThreads(child.pi.dwProcessId);
  Require(childThreads.has_value() && *childThreads == 1u, "a suspended new process has one thread");
  TerminateChildProcess(&child);

  HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Require(stop != nullptr, "CreateEventW failed");
  std::thread parked([stop] { WaitForSingleObject(stop, INFINITE); });
  const auto selfThreads = CountProcessThreads(GetCurrentProcessId());
  SetEvent(stop);
  parked.join();
  CloseHandle(stop);
  Require(selfThreads.has_value() && *selfThreads >= 2u, "this process has at least the parked thread too");

  const auto none = CountProcessThreads(0xFFFFFFF0u);
  Require(none.has_value() && *none == 0u, "no process with that id: no threads");
}

// 2026-11-01: a shutdown that took more than 30 s in a menu was confirmed as a
// hang with one game thread left, and its dump failed. A game that is closing
// gets no hang dump and no WCT file, once per episode.
void TestHandleHangTick_SkipsAGameThatIsClosing()
{
  const auto outBase = MakeTempDir(L"skydiag_hang_closing");
  ClearLog(outBase);
  auto shared = MakeStaleHeartbeatLayout(32u);
  shared->header.state_flags = skydiag::kState_InMenu;
  auto child = LaunchSuspendedChildProcess();
  auto proc = MakeAttachedProcessForChild(child, shared.get());
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

  Require(tick() == HangTickResult::kContinue, "the loop goes on to the exit");
  Require(state.hangSkippedAsClosingThisEpisode, "the episode is marked as a closing game");
  Require(!state.hangCapturedThisEpisode, "not a captured hang (the exit classification relies on it)");
  Require(CountFilesWithExt(outBase, L".dmp") == 0u, "no hang dump");
  Require(CountFilesWithExt(outBase, L".json") == 0u, "no WCT file");
  Require(pendingHangViewerDumpPath.empty(), "no viewer queued");

  (void)tick();
  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  const std::string line = "Hang detected while the game is closing (1 thread left";
  const auto first = log.find(line);
  Require(first != std::string::npos, "the skipped hang is logged");
  Require(log.find(line, first + 1u) == std::string::npos, "once per episode");

  shared->header.last_heartbeat_qpc = QpcNow();
  (void)tick();
  Require(!state.hangSkippedAsClosingThisEpisode, "a heartbeat ends the episode");

  TerminateChildProcess(&child);
  std::filesystem::remove_all(outBase);
}

// After a skipped closing hang, the exit tick says the game exited while it
// was closing, not that a game that stopped responding was ended.
void TestProcessExitAfterASkippedClosingHang()
{
  const auto outBase = MakeTempDir(L"skydiag_hang_closing_exit");
  ClearLog(outBase);
  auto shared = MakeStaleHeartbeatLayout(32u);
  shared->header.state_flags = skydiag::kState_InMenu;
  auto child = LaunchSuspendedChildProcess();
  auto proc = MakeAttachedProcessForChild(child, shared.get());
  const auto cfg = MakeHangTestConfig();
  LoadStats loadStats;
  std::uint32_t adaptiveLoadingThresholdSec = cfg.hangThresholdLoadingSec;
  HelperLoopState loop{};
  InitializeLoopState(proc, outBase, &loop);
  (void)HandleHangTick(
    cfg, proc, outBase, &loadStats, outBase / L"SkyrimDiag_LoadStats.json", &adaptiveLoadingThresholdSec, QpcNow(),
    &loop.pendingHangViewerDumpPath, &loop.hangState);
  Require(loop.hangState.hangSkippedAsClosingThisEpisode, "the hang is skipped as a closing game");

  // Ended during its stalled exit, e.g. from Task Manager.
  Require(TerminateProcess(child.pi.hProcess, 1u) != FALSE, "TerminateProcess failed");
  Require(WaitForSingleObject(child.pi.hProcess, 5000) == WAIT_OBJECT_0, "the child did not exit");
  Require(HandleProcessExitTick(cfg, proc, outBase, &loop), "the exit tick handles the exit");

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "The game exited while it was still closing (exit_code=1", "the exit is told as a closing game");
  Require(log.find("The game stopped responding and then exited") == std::string::npos, "not as a frozen game");
  Require(CountFilesWithExt(outBase, L".dmp") == 0u, "no dump");
  ShutdownRetentionWorker();
  TerminateChildProcess(&child);
  std::filesystem::remove_all(outBase);
}

// The closing check right before the capture can miss an ExitProcess that is
// still ending the other threads; a dump that then fails is told as the exit.
void TestExecuteConfirmedHangCapture_TellsAFailedDumpOfAClosingGame()
{
  const auto outBase = MakeTempDir(L"skydiag_hang_closing_capture");
  ClearLog(outBase);
  auto shared = MakeStaleHeartbeatLayout(32u);
  // One thread, and a dump of it fails (ERROR_PARTIAL_COPY), like the 11/01 exit.
  auto child = LaunchSuspendedChildProcess();
  auto proc = MakeAttachedProcessForChild(child, shared.get());
  HangDecision decision{};
  decision.isHang = true;
  decision.secondsSinceHeartbeat = 32.0;
  decision.thresholdSec = 30u;
  std::wstring pendingViewer;
  HangCaptureState state{};
  const auto result = ExecuteConfirmedHangCapture(
    MakeHangTestConfig(), proc, outBase, decision, skydiag::kState_InMenu, &pendingViewer, &state);
  Require(result == HangTickResult::kContinue, "the loop goes on");
  Require(CountFilesWithExt(outBase, L".dmp") == 0u, "the dump of the closing game failed");
  Require(state.hangSkippedAsClosingThisEpisode && !state.hangCapturedThisEpisode, "marked as a closing game");
  Require(CountFilesWithExt(outBase, L".json") == 0u, "no WCT file or incident manifest left");
  AssertContains(
    ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log"), "The game was closing by the end of the hang capture",
    "the failed dump is told as the exit");
  TerminateChildProcess(&child);
  std::filesystem::remove_all(outBase);
}

// A frozen game ended (Task Manager) while its hang dump is written leaves no
// threads. That is a game that stopped responding and was ended, not one that
// was closing.
void TestExecuteConfirmedHangCapture_AGameEndedDuringTheCaptureIsNotClosing()
{
  const auto outBase = MakeTempDir(L"skydiag_hang_ended_capture");
  ClearLog(outBase);
  auto shared = MakeStaleHeartbeatLayout(32u);
  auto child = LaunchSuspendedChildProcess();
  auto proc = MakeAttachedProcessForChild(child, shared.get());
  const auto cfg = MakeHangTestConfig();
  HelperLoopState loop{};
  InitializeLoopState(proc, outBase, &loop);
  // Gone before the dump is written, as if it was ended while it was.
  Require(TerminateProcess(child.pi.hProcess, 1u) != FALSE, "TerminateProcess failed");
  Require(WaitForSingleObject(child.pi.hProcess, 5000) == WAIT_OBJECT_0, "the child did not exit");
  HangDecision decision{};
  decision.isHang = true;
  decision.secondsSinceHeartbeat = 32.0;
  decision.thresholdSec = 10u;
  (void)ExecuteConfirmedHangCapture(
    cfg, proc, outBase, decision, skydiag::kState_None, &loop.pendingHangViewerDumpPath, &loop.hangState);
  Require(CountFilesWithExt(outBase, L".dmp") == 0u, "no dump of a process that is gone");
  Require(!loop.hangState.hangSkippedAsClosingThisEpisode, "not told as a closing game");
  Require(loop.hangState.hangCapturedThisEpisode, "the hang stays on record for the exit");
  Require(HandleProcessExitTick(cfg, proc, outBase, &loop), "the exit tick handles the exit");

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "The game exited during the hang capture", "the failed dump is told as the exit it met");
  AssertContains(
    log, "The game stopped responding and then exited (exit_code=1", "the exit is told as a frozen game that was ended");
  Require(log.find("while it was still closing") == std::string::npos, "not as a closing game");
  ShutdownRetentionWorker();
  TerminateChildProcess(&child);
  std::filesystem::remove_all(outBase);
}

// ADR-0027: the Address Library file SKSE plugins load for a game version.
void TestAddressLibraryFileName()
{
  using skydiag::helper::internal::AddressLibraryFileName;
  Require(AddressLibraryFileName(1, 5, 97, 0) == L"version-1-5-97-0.bin", "1.5.x");
  Require(AddressLibraryFileName(1, 6, 1170, 0) == L"versionlib-1-6-1170-0.bin", "1.6.x");
  Require(AddressLibraryFileName(1, 7, 104, 0) == L"versionlib-1-7-104-0.bin", "1.7.x");
  Require(skydiag::helper::internal::GameAddressLibraryBytes(GetCurrentProcessId()).empty(),
    "a process with no game Address Library beside it gets none");
}

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

// The thresholds the hang tick, a manual capture and the exit check hand to
// EvaluateHang (cfg, stateFlags, haveLoadingSamples, adaptiveLoadingThresholdSec).
void TestComputeHangThresholds()
{
  using skydiag::helper::ComputeHangThresholds;
  skydiag::helper::HelperConfig cfg{};
  cfg.hangThresholdInGameSec = 10u;
  cfg.hangThresholdInMenuSec = 30u;
  cfg.hangThresholdLoadingSec = 600u;
  cfg.enableAdaptiveLoadingThreshold = true;
  {
    const auto t = ComputeHangThresholds(cfg, skydiag::kState_None, true, 90u);
    Require(t.inGameSec == 10u && t.loadingSec == 90u, "in game: the in-game threshold, adaptive loading once sampled");
  }
  Require(ComputeHangThresholds(cfg, skydiag::kState_InMenu, true, 90u).inGameSec == 30u, "an open menu raises it");
  cfg.hangThresholdInMenuSec = 5u;
  Require(ComputeHangThresholds(cfg, skydiag::kState_InMenu, true, 90u).inGameSec == 10u,
    "a menu threshold below the in-game one does not lower it");
  Require(ComputeHangThresholds(cfg, skydiag::kState_None, false, 90u).loadingSec == 600u,
    "no loading samples yet: the configured loading threshold");
  cfg.enableAdaptiveLoadingThreshold = false;
  Require(ComputeHangThresholds(cfg, skydiag::kState_Loading, true, 90u).loadingSec == 600u,
    "adaptive threshold turned off: the configured one");
}

void TestWctHasCycle()
{
  using skydiag::helper::WctHasCycle;
  Require(WctHasCycle(nlohmann::json::parse(R"({"threads":[{"tid":1,"isCycle":false},{"tid":2,"isCycle":true}]})")),
    "a cycle thread is a deadlock");
  Require(!WctHasCycle(nlohmann::json::parse(R"({"threads":[{"tid":1,"isCycle":false},{"tid":2}]})")), "no cycle thread");
  Require(!WctHasCycle(nlohmann::json::parse(R"({"threads":{}})")), "threads that are not a list");
  Require(!WctHasCycle(nlohmann::json::array()), "not an object");
}

// A heartbeat stalled past the threshold is captured once per episode (dump
// and WCT after the grace check), and the episode ends when it comes back.
void TestHandleHangTick_CapturesAStaleHeartbeatOncePerEpisode()
{
  // The tick skips a process with one thread left (IsGameProcessClosing), and
  // this test process may have only its main thread.
  HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Require(stop != nullptr, "CreateEventW failed");
  std::thread parked([stop] { WaitForSingleObject(stop, INFINITE); });
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
  SetEvent(stop);
  parked.join();
  CloseHandle(stop);
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
  // The shared freeze capture (FreezeCapture.cpp) with the manual settings.
  const auto wct = nlohmann::json::parse(
    ReadAllTextUtf8(FindSingleFileByPrefix(outBase, L"SkyrimDiag_WCT_Manual_", L".json")));
  Require(wct["capture"]["kind"] == "manual" && wct["capture"].contains("dump_transport"), "the capture block");
  Require(!wct.contains("modal_dialogs"), "a manual capture does not probe dialog windows");
  const auto manifest = nlohmann::json::parse(
    ReadAllTextUtf8(FindSingleFileByPrefix(outBase, L"SkyrimDiag_Incident_Manual_", L".json")));
  Require(
    manifest["context"]["trigger"] == "test hotkey" && manifest["context"].contains("dump_transport") &&
      manifest["context"].contains("in_menu"),
    "the manifest context");
  ShutdownRetentionWorker();
  CloseAttachedProcess(&proc);
  std::filesystem::remove_all(outBase);
}

// A freeze capture whose dump cannot be written leaves no WCT file behind:
// retention removes one only with its dump (2026-11-01, one left for good).
void TestFreezeCaptureRemovesTheWctFileOfAFailedDump()
{
  const auto outBase = MakeTempDir(L"skydiag_freeze_failed_dump");
  ClearLog(outBase);
  auto shared = MakeStaleHeartbeatLayout(30u);
  auto proc = MakeSelfAttachedProcess(shared.get());
  skydiag::helper::internal::FreezeCaptureRequest request{};
  request.kind = "hang";
  request.logName = L"Hang";
  request.wctPath = outBase / L"SkyrimDiag_WCT_test.json";
  // A folder that does not exist: CreateFileW fails.
  request.dumpPath = (outBase / L"missing" / L"SkyrimDiag_Hang_test.dmp").wstring();
  HangDecision decision{};
  decision.isHang = true;
  decision.secondsSinceHeartbeat = 30.0;
  decision.thresholdSec = 10u;
  const auto result = skydiag::helper::internal::CaptureFreezeDumpAndWct(
    MakeHangTestConfig(), proc, outBase, decision, skydiag::kState_None, request);
  Require(!result.dumpWritten, "the dump fails");
  Require(!std::filesystem::exists(request.wctPath), "its WCT file is removed");
  AssertContains(ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log"), "Hang dump failed", "the failure is logged");
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
  InitializeLoopState(proc, outBase, &loop);

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
    SKYDIAG_RUN_TEST(TestEvaluateHangDecisions);
    SKYDIAG_RUN_TEST(TestAddressLibraryFileName);
    SKYDIAG_RUN_TEST(TestComputeHangThresholds);
    SKYDIAG_RUN_TEST(TestWctHasCycle);
    SKYDIAG_RUN_TEST(TestCountProcessThreads);
    SKYDIAG_RUN_TEST(TestHandleHangTick_CapturesAStaleHeartbeatOncePerEpisode);
    SKYDIAG_RUN_TEST(TestHandleHangTick_SkipsAGameThatIsClosing);
    SKYDIAG_RUN_TEST(TestProcessExitAfterASkippedClosingHang);
    SKYDIAG_RUN_TEST(TestExecuteConfirmedHangCapture_TellsAFailedDumpOfAClosingGame);
    SKYDIAG_RUN_TEST(TestExecuteConfirmedHangCapture_AGameEndedDuringTheCaptureIsNotClosing);
    SKYDIAG_RUN_TEST(TestHandleHangTick_LeavesAnExitDuringTheGraceWaitToTheExitTick);
    SKYDIAG_RUN_TEST(TestHandleHangTick_YieldsToACrashEvent);
    SKYDIAG_RUN_TEST(TestPssSnapshotWorksFromTheAttachHandle);
    SKYDIAG_RUN_TEST(TestManualCaptureWritesItsArtifacts);
    SKYDIAG_RUN_TEST(TestFreezeCaptureRemovesTheWctFileOfAFailedDump);
    SKYDIAG_RUN_TEST(TestRunHelperLoop_HandlesAnExitDuringTheHangGraceWait);
    SKYDIAG_RUN_TEST(TestRunHelperLoop_HoldsHangDetectionWhileACapturedCrashFreezesTheGame);
    SKYDIAG_RUN_TEST(TestLoadStatsAtomicSaveFailurePreservesExistingState);
    SKYDIAG_RUN_TEST(TestHandleHangTick_ReportsStatsPersistenceFailureAndUsesMemorySample);
    SKYDIAG_RUN_TEST(TestHandleHangTick_SkipsWhenHeartbeatNotInitialized);
    SKYDIAG_RUN_TEST(TestStartupLoadingIsNotALoadingSample);
    SKYDIAG_RUN_TEST(TestExecuteConfirmedHangCapture_WritesArtifacts);
    SKYDIAG_RUN_TEST(TestHeldHangDump_RemovedForASlowShutdown);
    SKYDIAG_RUN_TEST(TestHeldHangDump_KeptForAnEndedGame);
    SKYDIAG_RUN_TEST(TestHeldHangDump_KeptWhenAsked);
    SKYDIAG_RUN_TEST(TestHeldHangDump_ReleasedWhenTheGameRecovers);
    SKYDIAG_RUN_TEST(TestHeldHangDump_GameplayFreezeIsNotHeld);
    SKYDIAG_RUN_TEST(TestHeldHangDump_KeptWhenTheGameRecoveredBeforeAQuickQuit);
    SKYDIAG_RUN_TEST(TestHeldHangDump_HeldEpisodeIsNotSkippedAsClosing);
    SKYDIAG_RUN_TEST(TestHeldHangDump_StallJudgedByTheCaptureThreshold);
    SKYDIAG_RUN_TEST(TestHeldHangDump_KeptWhenADialogWasVisible);
    SKYDIAG_RUN_TEST(TestHeldHangDump_EpisodeEndsOnlyWhenTheHeartbeatMoves);
    SKYDIAG_RUN_TEST(TestGameplayCaptureThenTheGamesExitIsASlowShutdown);
    SKYDIAG_RUN_TEST(TestHeldHangDump_SettledByTheExitViewer);
    SKYDIAG_RUN_TEST(TestHeldHangDump_ReleasedWhenTheHelperStops);
    SKYDIAG_RUN_TEST(TestCaptureModalDialogsFindsMessageBoxOwnerThread);
    SKYDIAG_RUN_TEST(TestCaptureModalDialogsSendsNoMessageToOwnDialogs);
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
