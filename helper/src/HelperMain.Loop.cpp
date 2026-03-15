#include "HelperMainInternal.h"

#include <filesystem>
#include <string>

#include "CrashCapture.h"
#include "HelperLog.h"
#include "ManualCapture.h"

namespace {

using skydiag::helper::internal::AppendLogLine;
using skydiag::helper::internal::ClearPendingCrashAnalysis;
using skydiag::helper::internal::DoManualCapture;
using skydiag::helper::internal::FinalizePendingCrashAnalysisIfReady;
using skydiag::helper::internal::HandleCrashEventTick;
using skydiag::helper::internal::MaybeStopPendingCrashEtwCapture;

constexpr std::uint64_t kCrashEventRetryIntervalMs = 2000;
constexpr std::uint64_t kCrashEventWarnIntervalMs = 30000;

}  // namespace

namespace skydiag::helper::internal {

bool TryTriggerManualCapture(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase,
  const LoadStats& loadStats,
  std::uint32_t adaptiveLoadingThresholdSec,
  std::wstring_view source,
  ULONGLONG* lastManualCaptureTick)
{
  const ULONGLONG now = GetTickCount64();
  if (lastManualCaptureTick &&
      *lastManualCaptureTick != 0 &&
      now - *lastManualCaptureTick < kManualCaptureDebounceMs) {
    return false;
  }

  DoManualCapture(cfg, proc, outBase, loadStats, adaptiveLoadingThresholdSec, source);
  if (lastManualCaptureTick) {
    *lastManualCaptureTick = now;
  }
  return true;
}

void PumpManualCaptureInputs(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase,
  LoadStats* loadStats,
  std::uint32_t adaptiveLoadingThresholdSec)
{
  if (!loadStats) {
    return;
  }

  static ULONGLONG s_lastManualCaptureTick = 0;
  bool triggeredFromHotkeyMessage = false;

  MSG msg{};
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    if (msg.message == WM_HOTKEY && static_cast<int>(msg.wParam) == kHotkeyId) {
      triggeredFromHotkeyMessage =
        TryTriggerManualCapture(
          cfg,
          proc,
          outBase,
          *loadStats,
          adaptiveLoadingThresholdSec,
          L"WM_HOTKEY",
          &s_lastManualCaptureTick) || triggeredFromHotkeyMessage;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  if (cfg.enableManualCaptureHotkey && !triggeredFromHotkeyMessage) {
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (ctrl && shift && ((GetAsyncKeyState(VK_F12) & 1) != 0)) {
      TryTriggerManualCapture(
        cfg,
        proc,
        outBase,
        *loadStats,
        adaptiveLoadingThresholdSec,
        L"GetAsyncKeyState",
        &s_lastManualCaptureTick);
    }
  }
}

void RunHelperLoop(
  const HelperConfig& cfg,
  AttachedProcess& proc,
  const std::filesystem::path& outBase,
  LoadStats* loadStats,
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

    if (!proc.crashEvent) {
      const auto nowTick = GetTickCount64();
      if (state->nextCrashEventRetryTick64 == 0 || nowTick >= state->nextCrashEventRetryTick64) {
        if (skydiag::helper::TryAttachCrashEvent(proc, nullptr)) {
          AppendLogLine(outBase, L"Crash event recovered; crash capture path is enabled.");
          state->nextCrashEventWarnTick64 = 0;
        } else {
          state->nextCrashEventRetryTick64 = nowTick + kCrashEventRetryIntervalMs;
          if (state->nextCrashEventWarnTick64 == 0 || nowTick >= state->nextCrashEventWarnTick64) {
            AppendLogLine(
              outBase,
              BuildCrashEventUnavailableMessage(
                proc,
                L"Crash event still unavailable; helper continues in hang-only mode and will retry."));
            state->nextCrashEventWarnTick64 = nowTick + kCrashEventWarnIntervalMs;
          }
        }
      }
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
  const HelperConfig& cfg,
  const AttachedProcess& proc,
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

}  // namespace skydiag::helper::internal
