#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "SkyrimDiagHelper/CrashHeuristics.h"
#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/LoadStats.h"
#include "SkyrimDiagHelper/ProcessAttach.h"
#include "SkyrimDiagShared.h"

#include "CrashCapture.h"
#include "CaptureCommon.h"
#include "CompatibilityPreflight.h"
#include "CrashEtwCapture.h"
#include "DumpToolLaunch.h"
#include "HangCapture.h"
#include "HelperCommon.h"
#include "HelperLog.h"
#include "ManualCapture.h"
#include "PendingCrashAnalysis.h"

namespace {

using skydiag::helper::internal::MakeOutputBase;

using skydiag::helper::internal::AppendLogLine;
using skydiag::helper::internal::SetHelperLogRotation;
using skydiag::helper::internal::RunCompatibilityPreflight;
using skydiag::helper::internal::ApplyRetentionFromConfig;

using skydiag::helper::internal::PendingCrashAnalysis;
using skydiag::helper::internal::ClearPendingCrashAnalysis;
using skydiag::helper::internal::FinalizePendingCrashAnalysisIfReady;

using skydiag::helper::internal::PendingCrashEtwCapture;
using skydiag::helper::internal::MaybeStopPendingCrashEtwCapture;

using skydiag::helper::internal::HandleCrashEventTick;

using skydiag::helper::internal::DoManualCapture;

using skydiag::helper::internal::HangCaptureState;
using skydiag::helper::internal::HangTickResult;
using skydiag::helper::internal::HandleHangTick;

using skydiag::helper::internal::StartDumpToolViewer;

std::wstring Hex32(std::uint32_t v)
{
  wchar_t buf[11]{};
  std::swprintf(buf, 11, L"0x%08X", static_cast<unsigned int>(v));
  return buf;
}

std::uint32_t RemoveCrashArtifactsForDump(
  const std::filesystem::path& outBase,
  std::wstring_view dumpPath,
  const std::filesystem::path& extraArtifactPath = {})
{
  if (dumpPath.empty()) {
    return 0;
  }

  const std::filesystem::path dumpFs(dumpPath);
  const std::wstring stem = dumpFs.stem().wstring();
  if (stem.empty()) {
    return 0;
  }

  std::vector<std::filesystem::path> artifacts;
  artifacts.reserve(7);
  artifacts.push_back(dumpFs);
  artifacts.push_back(outBase / (stem + L"_SkyrimDiagBlackbox.jsonl"));
  artifacts.push_back(outBase / (stem + L"_SkyrimDiagReport.txt"));
  artifacts.push_back(outBase / (stem + L"_SkyrimDiagSummary.json"));
  artifacts.push_back(outBase / (stem + L"_SkyrimDiagWct.json"));
  artifacts.push_back(outBase / (stem + L".etl"));

  const std::wstring kCrashStemPrefix = L"SkyrimDiag_Crash_";
  if (stem.rfind(kCrashStemPrefix, 0) == 0) {
    std::wstring ts = stem.substr(kCrashStemPrefix.size());
    const std::wstring kFullSuffix = L"_Full";
    if (ts.size() > kFullSuffix.size() && ts.compare(ts.size() - kFullSuffix.size(), kFullSuffix.size(), kFullSuffix) == 0) {
      ts.resize(ts.size() - kFullSuffix.size());
    }
    if (!ts.empty()) {
      artifacts.push_back(outBase / (L"SkyrimDiag_Incident_Crash_" + ts + L".json"));
    }
  }
  if (!extraArtifactPath.empty()) {
    artifacts.push_back(extraArtifactPath);
  }

  std::uint32_t removedCount = 0;
  for (const auto& path : artifacts) {
    if (path.empty()) {
      continue;
    }
    std::error_code ec;
    const bool removed = std::filesystem::remove(path, ec);
    if (ec) {
      AppendLogLine(
        outBase,
        L"Failed to remove crash artifact: " + path.wstring()
          + L" (err=" + std::to_wstring(ec.value()) + L")");
      continue;
    }
    if (!ec && removed) {
      ++removedCount;
    }
  }
  return removedCount;
}

constexpr int kHotkeyId = 0x5344;  // 'SD' (arbitrary)

struct HelperLoopState
{
  bool crashCaptured = false;
  HangCaptureState hangState{};
  std::wstring pendingHangViewerDumpPath;
  std::wstring pendingCrashViewerDumpPath;
  std::wstring capturedCrashDumpPath;
  PendingCrashAnalysis pendingCrashAnalysis{};
  PendingCrashEtwCapture pendingCrashEtw{};
};

void InitializeLoopState(const skydiag::helper::AttachedProcess& proc, HelperLoopState* state)
{
  if (!state) {
    return;
  }
  state->hangState.wasLoading = (proc.shm->header.state_flags & skydiag::kState_Loading) != 0u;
  state->hangState.loadStartQpc = state->hangState.wasLoading ? proc.shm->header.start_qpc : 0;
}

void RegisterManualCaptureHotkeyIfEnabled(
  const skydiag::helper::HelperConfig& cfg,
  const std::filesystem::path& outBase)
{
  if (!cfg.enableManualCaptureHotkey) {
    return;
  }

  if (!RegisterHotKey(nullptr, kHotkeyId, MOD_CONTROL | MOD_SHIFT, VK_F12)) {
    const DWORD le = GetLastError();
    std::wcerr << L"[SkyrimDiagHelper] Warning: RegisterHotKey(Ctrl+Shift+F12) failed: " << le << L"\n";
    AppendLogLine(outBase, L"Warning: RegisterHotKey(Ctrl+Shift+F12) failed: " + std::to_wstring(le) +
      L" (falling back to GetAsyncKeyState polling)");
  } else {
    std::wcout << L"[SkyrimDiagHelper] Manual capture hotkey: Ctrl+Shift+F12\n";
    AppendLogLine(outBase, L"Manual capture hotkey registered: Ctrl+Shift+F12");
  }
}

void UnregisterManualCaptureHotkeyIfEnabled(const skydiag::helper::HelperConfig& cfg)
{
  if (!cfg.enableManualCaptureHotkey) {
    return;
  }
  UnregisterHotKey(nullptr, kHotkeyId);
}

void PumpManualCaptureInputs(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  skydiag::helper::LoadStats* loadStats,
  std::uint32_t adaptiveLoadingThresholdSec)
{
  if (!loadStats) {
    return;
  }

  MSG msg{};
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    if (msg.message == WM_HOTKEY && static_cast<int>(msg.wParam) == kHotkeyId) {
      DoManualCapture(cfg, proc, outBase, *loadStats, adaptiveLoadingThresholdSec, L"WM_HOTKEY");
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  // Fallback manual hotkey detection: some environments can miss WM_HOTKEY even when RegisterHotKey succeeds.
  // Polling is low overhead (once per loop) and makes manual capture more reliable.
  if (cfg.enableManualCaptureHotkey) {
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (ctrl && shift && ((GetAsyncKeyState(VK_F12) & 1) != 0)) {
      DoManualCapture(cfg, proc, outBase, *loadStats, adaptiveLoadingThresholdSec, L"GetAsyncKeyState");
    }
  }
}

void DrainCrashEventBeforeExit(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  HelperLoopState* state)
{
  if (!state) {
    return;
  }
  HandleCrashEventTick(
    cfg,
    proc,
    outBase,
    /*waitMs=*/0,
    &state->crashCaptured,
    &state->pendingCrashEtw,
    &state->pendingCrashAnalysis,
    &state->capturedCrashDumpPath,
    &state->pendingHangViewerDumpPath,
    &state->pendingCrashViewerDumpPath);
}

void CleanupCrashArtifactsAfterZeroExit(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  bool exitCode0StrongCrash,
  std::uint32_t exceptionCode,
  HelperLoopState* state)
{
  if (!state) {
    return;
  }
  if (exitCode0StrongCrash) {
    AppendLogLine(
      outBase,
      L"exit_code=0 after crash capture but exception_code="
        + Hex32(exceptionCode)
        + L" is strong; preserving crash artifacts and crash auto-actions.");
    return;
  }
  if (!state->crashCaptured) {
    return;
  }

  if (state->pendingCrashAnalysis.active && state->pendingCrashAnalysis.process) {
    if (!TerminateProcess(state->pendingCrashAnalysis.process, 1)) {
      AppendLogLine(
        outBase,
        L"exit_code=0 after crash capture; failed to terminate pending crash analysis process: "
          + std::to_wstring(GetLastError()));
    } else {
      AppendLogLine(outBase, L"exit_code=0 after crash capture; terminated pending crash analysis process.");
    }
    ClearPendingCrashAnalysis(&state->pendingCrashAnalysis);
  }

  const std::filesystem::path crashEtwPath = state->pendingCrashEtw.etwPath;
  MaybeStopPendingCrashEtwCapture(cfg, proc, outBase, /*force=*/true, &state->pendingCrashEtw);

  if (state->capturedCrashDumpPath.empty() && !state->pendingCrashViewerDumpPath.empty()) {
    state->capturedCrashDumpPath = state->pendingCrashViewerDumpPath;
  }
  if (!state->capturedCrashDumpPath.empty()) {
    const std::uint32_t removed = RemoveCrashArtifactsForDump(outBase, state->capturedCrashDumpPath, crashEtwPath);
    AppendLogLine(
      outBase,
      L"exit_code=0 after crash capture; removed "
        + std::to_wstring(removed)
        + L" crash artifact(s): "
        + std::filesystem::path(state->capturedCrashDumpPath).filename().wstring());
  }
  state->capturedCrashDumpPath.clear();
  state->pendingCrashViewerDumpPath.clear();
  state->crashCaptured = false;
}

void AppendExitClassificationLog(const std::filesystem::path& outBase, bool exitCode0StrongCrash, std::uint32_t exceptionCode)
{
  if (exitCode0StrongCrash) {
    AppendLogLine(
      outBase,
      L"Process exited with exit_code=0 but crash exception_code="
        + Hex32(exceptionCode)
        + L" is strong; treating as crash for viewer/deferred behavior.");
  } else {
    AppendLogLine(outBase, L"Process exited normally (exit_code=0); skipping crash event drain.");
  }
}

void LaunchDeferredViewersAfterExit(
  const skydiag::helper::HelperConfig& cfg,
  const std::filesystem::path& outBase,
  DWORD exitCode,
  bool exitCode0StrongCrash,
  HelperLoopState* state)
{
  if (!state) {
    return;
  }

  if (!state->pendingCrashViewerDumpPath.empty() &&
      cfg.autoOpenViewerOnCrash &&
      (exitCode != 0 || exitCode0StrongCrash)) {
    const std::wstring deferredDumpPath = state->pendingCrashViewerDumpPath;
    StartDumpToolViewer(
      cfg,
      deferredDumpPath,
      outBase,
      exitCode0StrongCrash ? L"crash_deferred_exit_code0_strong" : L"crash_deferred_exit");
    AppendLogLine(
      outBase,
      L"Deferred crash viewer launch attempted after process exit (exit_code="
        + std::to_wstring(exitCode)
        + L", dump="
        + std::filesystem::path(deferredDumpPath).filename().wstring()
        + L").");
    state->pendingCrashViewerDumpPath.clear();
  }

  if (!state->pendingHangViewerDumpPath.empty() && cfg.autoOpenViewerOnHang && cfg.autoOpenHangAfterProcessExit) {
    const DWORD delayMs = static_cast<DWORD>(std::min<std::uint32_t>(cfg.autoOpenHangDelayMs, 10000u));
    if (delayMs > 0) {
      Sleep(delayMs);
    }
    const auto launch = StartDumpToolViewer(cfg, state->pendingHangViewerDumpPath, outBase, L"hang_exit");
    if (launch == skydiag::helper::internal::DumpToolViewerLaunchResult::kLaunched) {
      AppendLogLine(outBase, L"Auto-opened DumpTool viewer for latest hang dump after process exit.");
    } else {
      AppendLogLine(outBase, L"Hang viewer auto-open attempt failed after process exit.");
    }
  }
}

void HandleProcessWaitFailed(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  DWORD waitError,
  HelperLoopState* state)
{
  if (!state) {
    return;
  }
  DrainCrashEventBeforeExit(cfg, proc, outBase, state);
  MaybeStopPendingCrashEtwCapture(cfg, proc, outBase, /*force=*/true, &state->pendingCrashEtw);
  std::wcerr << L"[SkyrimDiagHelper] Target process wait failed (err=" << waitError << L").\n";
  AppendLogLine(outBase, L"Target process wait failed: " + std::to_wstring(waitError));
}

bool HandleProcessExitTick(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  HelperLoopState* state)
{
  if (!state || !proc.process) {
    return false;
  }

  bool& crashCaptured = state->crashCaptured;
  auto& pendingCrashEtw = state->pendingCrashEtw;

  const DWORD w = WaitForSingleObject(proc.process, 0);
  if (w == WAIT_OBJECT_0) {
    DWORD exitCode = STILL_ACTIVE;
    GetExitCodeProcess(proc.process, &exitCode);
    const std::uint32_t exceptionCode = proc.shm ? proc.shm->header.crash.exception_code : 0u;
    const bool exitCode0StrongCrash = (exitCode == 0) && crashCaptured && skydiag::helper::IsStrongCrashException(exceptionCode);
    if (exitCode != 0) {
      DrainCrashEventBeforeExit(cfg, proc, outBase, state);
    } else {
      CleanupCrashArtifactsAfterZeroExit(cfg, proc, outBase, exitCode0StrongCrash, exceptionCode, state);
      AppendExitClassificationLog(outBase, exitCode0StrongCrash, exceptionCode);
    }
    MaybeStopPendingCrashEtwCapture(cfg, proc, outBase, /*force=*/true, &pendingCrashEtw);
    std::wcerr << L"[SkyrimDiagHelper] Target process exited (exit_code=" << exitCode << L").\n";
    AppendLogLine(outBase, L"Target process exited (exit_code=" + std::to_wstring(exitCode) + L").");
    LaunchDeferredViewersAfterExit(cfg, outBase, exitCode, exitCode0StrongCrash, state);
    return true;
  }
  if (w == WAIT_FAILED) {
    const DWORD le = GetLastError();
    HandleProcessWaitFailed(cfg, proc, outBase, le, state);
    return true;
  }
  return false;
}

void RunHelperLoop(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  skydiag::helper::LoadStats* loadStats,
  const std::filesystem::path& loadStatsPath,
  std::uint32_t* adaptiveLoadingThresholdSec,
  std::uint64_t attachNowQpc,
  HelperLoopState* state)
{
  if (!loadStats || !adaptiveLoadingThresholdSec || !state) {
    return;
  }

  for (;;) {
    PumpManualCaptureInputs(cfg, proc, outBase, loadStats, *adaptiveLoadingThresholdSec);

    FinalizePendingCrashAnalysisIfReady(cfg, proc, outBase, &state->pendingCrashAnalysis);
    MaybeStopPendingCrashEtwCapture(cfg, proc, outBase, /*force=*/false, &state->pendingCrashEtw);

    if (HandleProcessExitTick(cfg, proc, outBase, state)) {
      break;
    }

    const DWORD waitMs = 250;
    if (HandleCrashEventTick(
          cfg,
          proc,
          outBase,
          waitMs,
          &state->crashCaptured,
          &state->pendingCrashEtw,
          &state->pendingCrashAnalysis,
          &state->capturedCrashDumpPath,
          &state->pendingHangViewerDumpPath,
          &state->pendingCrashViewerDumpPath)) {
      continue;
    }
    if (HandleHangTick(
          cfg,
          proc,
          outBase,
          loadStats,
          loadStatsPath,
          adaptiveLoadingThresholdSec,
          attachNowQpc,
          &state->pendingHangViewerDumpPath,
          &state->hangState) == HangTickResult::kBreak) {
      break;
    }
  }
}

void ShutdownLoopState(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  HelperLoopState* state)
{
  if (!state) {
    return;
  }

  MaybeStopPendingCrashEtwCapture(cfg, proc, outBase, /*force=*/true, &state->pendingCrashEtw);
  UnregisterManualCaptureHotkeyIfEnabled(cfg);

  if (state->pendingCrashAnalysis.active) {
    AppendLogLine(outBase, L"Helper shutting down while crash analysis is still running; detaching from pending recapture task.");
    ClearPendingCrashAnalysis(&state->pendingCrashAnalysis);
  }
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
  // Keep helper overhead minimal vs. the game.
  SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

  std::wstring err;
  const auto cfg = skydiag::helper::LoadConfig(&err);
  SetHelperLogRotation(cfg.maxHelperLogBytes, cfg.maxHelperLogFiles);
  if (!err.empty()) {
    std::wcerr << L"[SkyrimDiagHelper] Config warning: " << err << L"\n";
  }

  skydiag::helper::AttachedProcess proc{};
  if (argc >= 3 && std::wstring_view(argv[1]) == L"--pid") {
    const auto pid = static_cast<std::uint32_t>(std::wcstoul(argv[2], nullptr, 10));
    if (!skydiag::helper::AttachByPid(pid, proc, &err)) {
      std::wcerr << L"[SkyrimDiagHelper] Attach failed: " << err << L"\n";
      return 2;
    }
  } else {
    if (!skydiag::helper::FindAndAttach(proc, &err)) {
      std::wcerr << L"[SkyrimDiagHelper] Attach failed: " << err << L"\n";
      return 2;
    }
  }

  if (!proc.shm || proc.shm->header.magic != skydiag::kMagic) {
    std::wcerr << L"[SkyrimDiagHelper] Shared memory invalid/missing.\n";
    skydiag::helper::Detach(proc);
    return 3;
  }
  if (proc.shm->header.version != skydiag::kVersion) {
    std::wcerr << L"[SkyrimDiagHelper] Shared memory version mismatch (got="
               << proc.shm->header.version << L", expected=" << skydiag::kVersion << L").\n";
    AppendLogLine(MakeOutputBase(cfg), L"Shared memory version mismatch (got=" + std::to_wstring(proc.shm->header.version) +
      L", expected=" + std::to_wstring(skydiag::kVersion) + L")");
    skydiag::helper::Detach(proc);
    return 3;
  }

  const auto outBase = MakeOutputBase(cfg);
  skydiag::helper::internal::ClearLog(outBase);
  std::wcout << L"[SkyrimDiagHelper] Attached to pid=" << proc.pid << L", output=" << outBase.wstring() << L"\n";
  AppendLogLine(outBase, L"Attached to pid=" + std::to_wstring(proc.pid) + L", output=" + outBase.wstring());
  if (!err.empty()) {
    AppendLogLine(outBase, L"Config warning: " + err);
  }

  RunCompatibilityPreflight(cfg, proc, outBase);

  ApplyRetentionFromConfig(cfg, outBase);

  skydiag::helper::LoadStats loadStats;
  std::uint32_t adaptiveLoadingThresholdSec = cfg.hangThresholdLoadingSec;
  const auto loadStatsPath = outBase / L"SkyrimDiag_LoadStats.json";
  if (cfg.enableAdaptiveLoadingThreshold) {
    loadStats.LoadFromFile(loadStatsPath);
    adaptiveLoadingThresholdSec = loadStats.SuggestedLoadingThresholdSec(cfg);
    std::wcout << L"[SkyrimDiagHelper] Adaptive loading threshold: "
               << adaptiveLoadingThresholdSec << L"s (fallback=" << cfg.hangThresholdLoadingSec << L"s)\n";
  }

  RegisterManualCaptureHotkeyIfEnabled(cfg, outBase);

  LARGE_INTEGER attachNow{};
  QueryPerformanceCounter(&attachNow);
  const std::uint64_t attachNowQpc = static_cast<std::uint64_t>(attachNow.QuadPart);

  HelperLoopState loopState{};
  InitializeLoopState(proc, &loopState);
  RunHelperLoop(
    cfg,
    proc,
    outBase,
    &loadStats,
    loadStatsPath,
    &adaptiveLoadingThresholdSec,
    attachNowQpc,
    &loopState);
  ShutdownLoopState(cfg, proc, outBase, &loopState);

  skydiag::helper::Detach(proc);
  return 0;
}
