// The polled Ctrl+Shift+F12 detector must report one press per chord press,
// in whatever order the keys go down, and nothing while it is held.

#include <cassert>
#include <cstdio>

#include "ManualCaptureKeyWatcher.h"

using skydiag::helper::internal::ManualCaptureChordEdge;

static void TestOnePressPerChordHold()
{
  ManualCaptureChordEdge edge;
  assert(!edge.Update(false, false, false));
  assert(!edge.Update(true, false, false));
  assert(!edge.Update(true, true, false));
  assert(edge.Update(true, true, true));   // chord completes
  assert(!edge.Update(true, true, true));  // still held
  assert(!edge.Update(true, true, true));
  assert(!edge.Update(true, true, false)); // F12 released
  assert(edge.Update(true, true, true));   // pressed again
}

static void TestKeyOrderDoesNotMatter()
{
  ManualCaptureChordEdge edge;
  assert(!edge.Update(false, false, true));  // F12 first
  assert(!edge.Update(true, false, true));
  assert(edge.Update(true, true, true));
}

static void TestPartialChordsNeverFire()
{
  ManualCaptureChordEdge edge;
  assert(!edge.Update(false, true, true));
  assert(!edge.Update(true, false, true));
  assert(!edge.Update(true, true, false));
  assert(!edge.Update(false, false, true));
}

int main()
{
  TestOnePressPerChordHold();
  TestKeyOrderDoesNotMatter();
  TestPartialChordsNeverFire();
  std::puts("manual capture chord edge tests passed");
  return 0;
}
