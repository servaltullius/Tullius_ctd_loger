#include <Windows.h>

#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/LoadStats.h"
#include "SkyrimDiagHelper/ProcessAttach.h"
#include "SkyrimDiagShared.h"

#include "CaptureCommon.h"
#include "CompatibilityPreflight.h"
#include "HelperCommon.h"
#include "HelperMainInternal.h"
#include "HelperLog.h"
#include "RetentionWorker.h"

using skydiag::helper::internal::AppendLogLine;
using skydiag::helper::internal::ApplyRetentionFromConfig;
using skydiag::helper::internal::BuildCrashEventUnavailableMessage;
using skydiag::helper::internal::MakeOutputBase;
using skydiag::helper::internal::SetHelperLogRotation;
using skydiag::helper::internal::RunCompatibilityPreflight;
using skydiag::helper::internal::ShutdownRetentionWorker;

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
  const std::wstring configWarning = err;
  err.clear();

  HANDLE helperSingletonMutex = skydiag::helper::internal::AcquireHelperSingletonMutex(proc.pid, &err);
  if (helperSingletonMutex == INVALID_HANDLE_VALUE) {
    AppendLogLine(outBase, L"Another helper instance is already active for this pid; exiting duplicate helper.");
    skydiag::helper::Detach(proc);
    return 0;
  }
  if (!helperSingletonMutex && !err.empty()) {
    AppendLogLine(outBase, L"Warning: helper singleton mutex unavailable: " + err);
  }

  skydiag::helper::internal::ClearLog(outBase);
  std::wcout << L"[SkyrimDiagHelper] Attached to pid=" << proc.pid << L", output=" << outBase.wstring() << L"\n";
  AppendLogLine(outBase, L"Attached to pid=" + std::to_wstring(proc.pid) + L", output=" + outBase.wstring());
  if (!configWarning.empty()) {
    AppendLogLine(outBase, L"Config warning: " + configWarning);
  }
  if (!proc.crashEvent) {
    AppendLogLine(
      outBase,
      BuildCrashEventUnavailableMessage(
        proc,
        L"Warning: crash event unavailable; helper is running in hang-only mode and will keep retrying."));
  }

  RunCompatibilityPreflight(cfg, proc, outBase);

  const bool grassCacheMode = skydiag::helper::internal::DetectGrassCacheMode(cfg, proc, outBase);

  if (grassCacheMode) {
    ApplyRetentionFromConfig(cfg, outBase);
    skydiag::helper::internal::RunGrassCacheLoop(proc, outBase);
    ShutdownRetentionWorker();
  } else {
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

    skydiag::helper::internal::RegisterManualCaptureHotkeyIfEnabled(cfg, outBase);

    LARGE_INTEGER attachNow{};
    QueryPerformanceCounter(&attachNow);
    const std::uint64_t attachNowQpc = static_cast<std::uint64_t>(attachNow.QuadPart);

    skydiag::helper::internal::HelperLoopState loopState{};
    skydiag::helper::internal::InitializeLoopState(proc, &loopState);
    skydiag::helper::internal::RunHelperLoop(
      cfg,
      proc,
      outBase,
      &loadStats,
      loadStatsPath,
      &adaptiveLoadingThresholdSec,
      attachNowQpc,
      &loopState);
    skydiag::helper::internal::ShutdownLoopState(cfg, proc, outBase, &loopState);
    ShutdownRetentionWorker();
  }

  if (helperSingletonMutex && helperSingletonMutex != INVALID_HANDLE_VALUE) {
    CloseHandle(helperSingletonMutex);
  }
  skydiag::helper::Detach(proc);
  return 0;
}
