#include "ManualCaptureKeyWatcher.h"

#include <Windows.h>

namespace skydiag::helper::internal {
namespace {

constexpr DWORD kKeyPollIntervalMs = 20;

bool IsKeyDown(int virtualKey) noexcept
{
  return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

}  // namespace

ManualCaptureKeyWatcher::~ManualCaptureKeyWatcher()
{
  Stop();
}

void ManualCaptureKeyWatcher::Start()
{
  if (thread_.joinable()) {
    return;
  }
  stop_.store(false, std::memory_order_relaxed);
  pressed_.store(false, std::memory_order_relaxed);
  thread_ = std::thread([this] {
    ManualCaptureChordEdge edge;
    while (!stop_.load(std::memory_order_relaxed)) {
      if (edge.Update(IsKeyDown(VK_CONTROL), IsKeyDown(VK_SHIFT), IsKeyDown(VK_F12))) {
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
