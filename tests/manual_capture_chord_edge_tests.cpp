// The polled manual capture hotkey detector must report one press per chord
// press, in whatever order the keys go down, and nothing while it is held.
// Like RegisterHotKey it needs exactly the chord's modifiers, so a chord that
// also holds another of Ctrl, Shift and Alt does not fire it. The same watcher
// polls Crash Logger's thread dump hotkey, which matches like Crash Logger.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <set>

#include "ManualCaptureKeyWatcher.h"

using skydiag::helper::CrashLoggerThreadDumpHotkey;
using skydiag::helper::IsCrashLoggerThreadDumpHotkeyDown;
using skydiag::helper::kDefaultManualCaptureHotkey;
using skydiag::helper::kLegacyManualCaptureHotkey;
using skydiag::helper::kVkAlt;
using skydiag::helper::kVkBack;
using skydiag::helper::kVkControl;
using skydiag::helper::kVkF12;
using skydiag::helper::kVkShift;
using skydiag::helper::ParseManualCaptureHotkey;
using skydiag::helper::internal::ManualCaptureChordEdge;

namespace {

// The keys held down at one poll.
struct Keys
{
  std::set<std::uint32_t> down;

  Keys(std::initializer_list<std::uint32_t> keys) : down(keys) {}

  bool operator()(std::uint32_t virtualKey) const { return down.count(virtualKey) != 0; }
};

bool Poll(ManualCaptureChordEdge& edge, std::initializer_list<std::uint32_t> keys)
{
  return edge.Update(Keys(keys));
}

void TestDefaultIsCtrlShiftBackspace()
{
  assert(kDefaultManualCaptureHotkey.virtualKey == kVkBack);
  ManualCaptureChordEdge edge;
  assert(Poll(edge, { kVkControl, kVkShift, kVkBack }));
}

void TestOnePressPerChordHold()
{
  ManualCaptureChordEdge edge;
  assert(!Poll(edge, {}));
  assert(!Poll(edge, { kVkControl }));
  assert(!Poll(edge, { kVkControl, kVkShift }));
  assert(Poll(edge, { kVkControl, kVkShift, kVkBack }));   // chord completes
  assert(!Poll(edge, { kVkControl, kVkShift, kVkBack }));  // still held
  assert(!Poll(edge, { kVkControl, kVkShift, kVkBack }));
  assert(!Poll(edge, { kVkControl, kVkShift }));  // key released
  assert(Poll(edge, { kVkControl, kVkShift, kVkBack }));   // pressed again
}

void TestKeyOrderDoesNotMatter()
{
  ManualCaptureChordEdge edge;
  assert(!Poll(edge, { kVkBack }));  // key first
  assert(!Poll(edge, { kVkControl, kVkBack }));
  assert(Poll(edge, { kVkControl, kVkShift, kVkBack }));
}

void TestPartialChordsNeverFire()
{
  ManualCaptureChordEdge edge;
  assert(!Poll(edge, { kVkShift, kVkBack }));
  assert(!Poll(edge, { kVkControl, kVkBack }));
  assert(!Poll(edge, { kVkControl, kVkShift }));
  assert(!Poll(edge, { kVkBack }));
}

void TestAnotherModifierNeverFires()
{
  ManualCaptureChordEdge edge;
  assert(!Poll(edge, { kVkControl, kVkShift, kVkAlt, kVkBack }));
  // Releasing the extra modifier completes the exact chord.
  assert(Poll(edge, { kVkControl, kVkShift, kVkBack }));
}

void TestCrashLoggerThreadDumpChordDoesNotFireTheDefault()
{
  ManualCaptureChordEdge edge;
  assert(!Poll(edge, { kVkControl, kVkShift, kVkF12 }));
}

// The old Ctrl+Shift+F12, watched on its own edge to log presses of it, fires
// once per press of exactly that chord.
void TestLegacyChordEdge()
{
  ManualCaptureChordEdge legacy(kLegacyManualCaptureHotkey);
  assert(!Poll(legacy, { kVkControl, kVkShift, kVkBack }));  // the new default
  assert(!Poll(legacy, { kVkControl, kVkShift, kVkAlt, kVkF12 }));
  assert(!Poll(legacy, { kVkControl, kVkF12 }));
  assert(Poll(legacy, { kVkControl, kVkShift, kVkF12 }));
  assert(!Poll(legacy, { kVkControl, kVkShift, kVkF12 }));  // still held
  assert(!Poll(legacy, { kVkControl, kVkShift }));
  assert(Poll(legacy, { kVkControl, kVkShift, kVkF12 }));  // pressed again

  // The default edge and the legacy edge, polled side by side as the watcher
  // does, each fire only on their own chord.
  ManualCaptureChordEdge current;
  ManualCaptureChordEdge old(kLegacyManualCaptureHotkey);
  assert(Poll(current, { kVkControl, kVkShift, kVkBack }));
  assert(!Poll(old, { kVkControl, kVkShift, kVkBack }));
  assert(!Poll(current, { kVkControl, kVkShift, kVkF12 }));
  assert(Poll(old, { kVkControl, kVkShift, kVkF12 }));
}

// Crash Logger's own thread dump hotkey, watched for
// IsLikelyCrashLoggerThreadDumpFault, matches the way Crash Logger does:
// all of its keys down, extra keys or not.
void TestCrashLoggerThreadDumpHotkeyDown()
{
  const CrashLoggerThreadDumpHotkey defaults;  // 17, 16, 123
  assert(IsCrashLoggerThreadDumpHotkeyDown(defaults, Keys{ kVkControl, kVkShift, kVkF12 }));
  assert(IsCrashLoggerThreadDumpHotkeyDown(defaults, Keys{ kVkControl, kVkShift, kVkAlt, kVkF12 }));
  assert(!IsCrashLoggerThreadDumpHotkeyDown(defaults, Keys{ kVkControl, kVkShift }));
  assert(!IsCrashLoggerThreadDumpHotkeyDown(defaults, Keys{ kVkControl, kVkF12 }));
  // The new manual capture default does not hold Crash Logger's keys.
  assert(!IsCrashLoggerThreadDumpHotkeyDown(defaults, Keys{ kVkControl, kVkShift, kVkBack }));

  CrashLoggerThreadDumpHotkey disabled;
  disabled.enabled = false;
  assert(!IsCrashLoggerThreadDumpHotkeyDown(disabled, Keys{ kVkControl, kVkShift, kVkF12 }));

  // Left / right modifier codes are their own keys to GetAsyncKeyState.
  CrashLoggerThreadDumpHotkey leftCtrl;
  leftCtrl.keys = { 162, 16, 123 };
  assert(IsCrashLoggerThreadDumpHotkeyDown(leftCtrl, Keys{ 162u, kVkShift, kVkF12 }));
  assert(!IsCrashLoggerThreadDumpHotkeyDown(leftCtrl, Keys{ kVkControl, kVkShift, kVkF12 }));

  // A code GetAsyncKeyState never reports down never fires.
  CrashLoggerThreadDumpHotkey outOfRange;
  outOfRange.keys = { 17, 0 };
  assert(!IsCrashLoggerThreadDumpHotkeyDown(outOfRange, [](std::uint32_t) { return true; }));
  outOfRange.keys = { 17, 256 };
  assert(!IsCrashLoggerThreadDumpHotkeyDown(outOfRange, [](std::uint32_t) { return true; }));
}

void TestConfiguredChord()
{
  const auto hotkey = ParseManualCaptureHotkey(L"Shift+F9");
  assert(hotkey);
  ManualCaptureChordEdge edge(*hotkey);
  constexpr std::uint32_t kVkF9 = 0x78u;
  assert(!Poll(edge, { kVkControl, kVkShift, kVkBack }));  // not the default any more
  assert(!Poll(edge, { kVkControl, kVkShift, kVkF9 }));    // Ctrl is not part of it
  assert(Poll(edge, { kVkShift, kVkF9 }));
  assert(!Poll(edge, { kVkShift, kVkF9 }));
}

}  // namespace

int main()
{
  try {
    TestDefaultIsCtrlShiftBackspace();
    TestOnePressPerChordHold();
    TestKeyOrderDoesNotMatter();
    TestPartialChordsNeverFire();
    TestAnotherModifierNeverFires();
    TestCrashLoggerThreadDumpChordDoesNotFireTheDefault();
    TestLegacyChordEdge();
    TestCrashLoggerThreadDumpHotkeyDown();
    TestConfiguredChord();
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
  std::puts("manual capture chord edge tests passed");
  return 0;
}
