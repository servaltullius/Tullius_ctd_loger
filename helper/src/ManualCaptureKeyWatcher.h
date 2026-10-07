#pragma once

#include <atomic>
#include <thread>

namespace skydiag::helper::internal {

// Turns polled Ctrl / Shift / F12 down states into one press per press of the
// whole chord, however long it is held.
class ManualCaptureChordEdge
{
public:
  bool Update(bool ctrlDown, bool shiftDown, bool f12Down) noexcept
  {
    const bool chordDown = ctrlDown && shiftDown && f12Down;
    const bool pressed = chordDown && !chordWasDown_;
    chordWasDown_ = chordDown;
    return pressed;
  }

private:
  bool chordWasDown_ = false;
};

// Watches Ctrl+Shift+F12 by polling the keys' current down state on its own
// thread, alongside the RegisterHotKey registration.
//
// In the v0.2.59-rc5 field test WM_HOTKEY did not arrive while the game was in
// gameplay (it did in a menu), and the old fallback read GetAsyncKeyState's
// "pressed since the last call" bit. That bit is shared by every process, and
// Crash Logger 1.25 polls the same keys for its thread dump, so it took the
// press and no manual capture was made. The current down state is not
// consumed by other readers, and a 20 ms poll does not miss a key press.
class ManualCaptureKeyWatcher
{
public:
  ManualCaptureKeyWatcher() = default;
  ~ManualCaptureKeyWatcher();
  ManualCaptureKeyWatcher(const ManualCaptureKeyWatcher&) = delete;
  ManualCaptureKeyWatcher& operator=(const ManualCaptureKeyWatcher&) = delete;

  void Start();
  void Stop();

  // True once for each chord press seen since the previous call.
  bool ConsumePress() noexcept;

private:
  std::atomic<bool> stop_{ false };
  std::atomic<bool> pressed_{ false };
  std::thread thread_;
};

// The helper's single watcher, started and stopped with the hotkey
// registration.
ManualCaptureKeyWatcher& ManualCaptureKeys();

}  // namespace skydiag::helper::internal
