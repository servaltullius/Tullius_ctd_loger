#include "ManualCaptureKeyWatcher.h"

#include <Windows.h>

#include <cstdint>

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

void ManualCaptureKeyWatcher::Start(const ManualCaptureHotkey& hotkey)
{
  if (thread_.joinable()) {
    return;
  }
  stop_.store(false, std::memory_order_relaxed);
  pressed_.store(false, std::memory_order_relaxed);
  thread_ = std::thread([this, hotkey] {
    ManualCaptureChordEdge edge(hotkey);
    while (!stop_.load(std::memory_order_relaxed)) {
      if (edge.Update(IsKeyDown)) {
        pressed_.store(true, std::memory_order_release);
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

ManualCaptureKeyWatcher& ManualCaptureKeys()
{
  static ManualCaptureKeyWatcher watcher;
  return watcher;
}

}  // namespace skydiag::helper::internal
