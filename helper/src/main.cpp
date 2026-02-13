#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/LoadStats.h"
#include "SkyrimDiagHelper/ProcessAttach.h"
#include "SkyrimDiagHelper/Retention.h"
#include "SkyrimDiagShared.h"

#include "CrashCapture.h"
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

  {
    skydiag::helper::RetentionLimits limits{};
    limits.maxCrashDumps = cfg.maxCrashDumps;
    limits.maxHangDumps = cfg.maxHangDumps;
    limits.maxManualDumps = cfg.maxManualDumps;
    limits.maxEtwTraces = cfg.maxEtwTraces;
    skydiag::helper::ApplyRetentionToOutputDir(outBase, limits);
  }

  skydiag::helper::LoadStats loadStats;
  std::uint32_t adaptiveLoadingThresholdSec = cfg.hangThresholdLoadingSec;
  const auto loadStatsPath = outBase / L"SkyrimDiag_LoadStats.json";
  if (cfg.enableAdaptiveLoadingThreshold) {
    loadStats.LoadFromFile(loadStatsPath);
    adaptiveLoadingThresholdSec = loadStats.SuggestedLoadingThresholdSec(cfg);
    std::wcout << L"[SkyrimDiagHelper] Adaptive loading threshold: "
               << adaptiveLoadingThresholdSec << L"s (fallback=" << cfg.hangThresholdLoadingSec << L"s)\n";
  }

  constexpr int kHotkeyId = 0x5344;  // 'SD' (arbitrary)
  if (cfg.enableManualCaptureHotkey) {
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

  LARGE_INTEGER attachNow{};
  QueryPerformanceCounter(&attachNow);
  const std::uint64_t attachNowQpc = static_cast<std::uint64_t>(attachNow.QuadPart);
  const std::uint64_t attachHeartbeatQpc = proc.shm->header.last_heartbeat_qpc;

  bool crashCaptured = false;

  HangCaptureState hangState{};
  hangState.wasLoading = (proc.shm->header.state_flags & skydiag::kState_Loading) != 0u;
  hangState.loadStartQpc = hangState.wasLoading ? proc.shm->header.start_qpc : 0;

  std::wstring pendingHangViewerDumpPath;
  std::wstring pendingCrashViewerDumpPath;
  PendingCrashAnalysis pendingCrashAnalysis{};

  PendingCrashEtwCapture pendingCrashEtw{};

  for (;;) {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_HOTKEY && static_cast<int>(msg.wParam) == kHotkeyId) {
        DoManualCapture(cfg, proc, outBase, loadStats, adaptiveLoadingThresholdSec, L"WM_HOTKEY");
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
        DoManualCapture(cfg, proc, outBase, loadStats, adaptiveLoadingThresholdSec, L"GetAsyncKeyState");
      }
    }

    FinalizePendingCrashAnalysisIfReady(cfg, proc, outBase, &pendingCrashAnalysis);
    MaybeStopPendingCrashEtwCapture(cfg, proc, outBase, /*force=*/false, &pendingCrashEtw);

    if (proc.process) {
      const DWORD w = WaitForSingleObject(proc.process, 0);
      if (w == WAIT_OBJECT_0) {
        // Check exit code: normal exit (0) means shutdown exceptions should
        // be ignored; non-zero means a real crash occurred.
        DWORD exitCode = STILL_ACTIVE;
        GetExitCodeProcess(proc.process, &exitCode);
        if (exitCode != 0) {
          // Drain any pending crash event before exiting — fixes race where
          // the process terminates before the next HandleCrashEventTick poll.
          HandleCrashEventTick(cfg, proc, outBase, /*waitMs=*/0,
            &crashCaptured, &pendingCrashEtw, &pendingCrashAnalysis, &pendingHangViewerDumpPath,
            &pendingCrashViewerDumpPath);
        } else {
          AppendLogLine(outBase, L"Process exited normally (exit_code=0); skipping crash event drain.");
        }
        MaybeStopPendingCrashEtwCapture(cfg, proc, outBase, /*force=*/true, &pendingCrashEtw);
        std::wcerr << L"[SkyrimDiagHelper] Target process exited (exit_code=" << exitCode << L").\n";
        AppendLogLine(outBase, L"Target process exited (exit_code=" + std::to_wstring(exitCode) + L").");
        if (!pendingCrashViewerDumpPath.empty() && cfg.autoOpenViewerOnCrash) {
          StartDumpToolViewer(cfg, pendingCrashViewerDumpPath, outBase, L"crash_deferred_exit");
          AppendLogLine(outBase, L"Auto-opened DumpTool viewer for crash dump after deferred process exit.");
          pendingCrashViewerDumpPath.clear();
        }
        if (!pendingHangViewerDumpPath.empty() && cfg.autoOpenViewerOnHang && cfg.autoOpenHangAfterProcessExit) {
          const DWORD delayMs = static_cast<DWORD>(std::min<std::uint32_t>(cfg.autoOpenHangDelayMs, 10000u));
          if (delayMs > 0) {
            Sleep(delayMs);
          }
          StartDumpToolViewer(cfg, pendingHangViewerDumpPath, outBase, L"hang_exit");
          AppendLogLine(outBase, L"Auto-opened DumpTool viewer for latest hang dump after process exit.");
        }
        break;
      }
      if (w == WAIT_FAILED) {
        HandleCrashEventTick(cfg, proc, outBase, /*waitMs=*/0,
          &crashCaptured, &pendingCrashEtw, &pendingCrashAnalysis, &pendingHangViewerDumpPath,
          &pendingCrashViewerDumpPath);
        MaybeStopPendingCrashEtwCapture(cfg, proc, outBase, /*force=*/true, &pendingCrashEtw);
        const DWORD le = GetLastError();
        std::wcerr << L"[SkyrimDiagHelper] Target process wait failed (err=" << le << L").\n";
        AppendLogLine(outBase, L"Target process wait failed: " + std::to_wstring(le));
        break;
      }
    }

    const DWORD waitMs = 250;
    if (HandleCrashEventTick(
          cfg,
          proc,
          outBase,
          waitMs,
          &crashCaptured,
          &pendingCrashEtw,
          &pendingCrashAnalysis,
          &pendingHangViewerDumpPath,
          &pendingCrashViewerDumpPath)) {
      continue;
    }
    if (HandleHangTick(
          cfg,
          proc,
          outBase,
          &loadStats,
          loadStatsPath,
          &adaptiveLoadingThresholdSec,
          attachNowQpc,
          attachHeartbeatQpc,
          &pendingHangViewerDumpPath,
          &hangState) == HangTickResult::kBreak) {
      break;
    }
  }

  MaybeStopPendingCrashEtwCapture(cfg, proc, outBase, /*force=*/true, &pendingCrashEtw);

  if (cfg.enableManualCaptureHotkey) {
    UnregisterHotKey(nullptr, kHotkeyId);
  }

  if (pendingCrashAnalysis.active) {
    AppendLogLine(outBase, L"Helper shutting down while crash analysis is still running; detaching from pending recapture task.");
    ClearPendingCrashAnalysis(&pendingCrashAnalysis);
  }

  skydiag::helper::Detach(proc);
  return 0;
}
