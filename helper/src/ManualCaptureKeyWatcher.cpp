#include "ManualCaptureKeyWatcher.h"

#include <Windows.h>

#include <cstdint>
#include <utility>

#include "CrashCapture.h"

namespace skydiag::helper::internal {
namespace {

constexpr DWORD kKeyPollIntervalMs = 20;

bool IsKeyDown(std::uint32_t virtualKey) noexcept
{
  return (GetAsyncKeyState(static_cast<int>(virtualKey)) & 0x8000) != 0;
}

}  // namespace

ManualCaptureKeyWatcher::~ManualCaptureKeyWatcher()
{
  Stop();
}

void ManualCaptureKeyWatcher::Start(
  std::optional<ManualCaptureHotkey> manualCapture,
  std::optional<CrashLoggerThreadDumpHotkey> crashLoggerThreadDump,
  std::optional<ManualCaptureHotkey> legacyManualCapture)
{
  if (thread_.joinable() || (!manualCapture && !crashLoggerThreadDump && !legacyManualCapture)) {
    return;
  }
  stop_.store(false, std::memory_order_relaxed);
  pressed_.store(false, std::memory_order_relaxed);
  legacyPressed_.store(false, std::memory_order_relaxed);
  thread_ = std::thread([this, manualCapture, legacyManualCapture, crashLoggerThreadDump = std::move(crashLoggerThreadDump)] {
    std::optional<ManualCaptureChordEdge> edge;
    if (manualCapture) {
      edge.emplace(*manualCapture);
    }
    // The old chord has its own edge and matches exactly, like the configured
    // one; the helper loop logs its presses (ConsumeLegacyPress).
    std::optional<ManualCaptureChordEdge> legacyEdge;
    if (legacyManualCapture) {
      legacyEdge.emplace(*legacyManualCapture);
    }
    while (!stop_.load(std::memory_order_relaxed)) {
      if (edge && edge->Update(IsKeyDown)) {
        pressed_.store(true, std::memory_order_release);
      }
      if (legacyEdge && legacyEdge->Update(IsKeyDown)) {
        legacyPressed_.store(true, std::memory_order_release);
      }
      // Noted on every poll while held, so IsLikelyCrashLoggerThreadDumpFault's
      // window runs from the last poll that saw Crash Logger's keys down.
      if (crashLoggerThreadDump && IsCrashLoggerThreadDumpHotkeyDown(*crashLoggerThreadDump, IsKeyDown)) {
        NoteCrashLoggerThreadDumpHotkeyPressed(GetTickCount64());
      }
      Sleep(kKeyPollIntervalMs);
    }
  });
}

void ManualCaptureKeyWatcher::Stop()
{
  if (!thread_.joinable()) {
    return;
  }
  stop_.store(true, std::memory_order_relaxed);
  thread_.join();
}

bool ManualCaptureKeyWatcher::ConsumePress() noexcept
{
  return pressed_.exchange(false, std::memory_order_acq_rel);
}

bool ManualCaptureKeyWatcher::ConsumeLegacyPress() noexcept
{
  return legacyPressed_.exchange(false, std::memory_order_acq_rel);
}

ManualCaptureKeyWatcher& ManualCaptureKeys()
{
  static ManualCaptureKeyWatcher watcher;
  return watcher;
}

}  // namespace skydiag::helper::internal
