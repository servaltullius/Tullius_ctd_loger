// The polled manual capture hotkey detector must report one press per chord
// press, in whatever order the keys go down, and nothing while it is held.
// Like RegisterHotKey it needs exactly the chord's modifiers, so a chord that
// also holds another of Ctrl, Shift and Alt does not fire it.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <set>

#include "ManualCaptureKeyWatcher.h"

using skydiag::helper::kDefaultManualCaptureHotkey;
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
  TestDefaultIsCtrlShiftBackspace();
  TestOnePressPerChordHold();
  TestKeyOrderDoesNotMatter();
  TestPartialChordsNeverFire();
  TestAnotherModifierNeverFires();
  TestCrashLoggerThreadDumpChordDoesNotFireTheDefault();
  TestConfiguredChord();
  std::puts("manual capture chord edge tests passed");
  return 0;
}
