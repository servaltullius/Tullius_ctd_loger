#pragma once

#include <cstddef>
#include <vector>

namespace skydiag::dump_tool {

enum class StackFrameKind
{
  kGame,     // SkyrimSE.exe
  kPlugin,   // any other non-system module (SKSE plugins, the SKSE runtime)
  kSystem,   // Windows and C/C++ runtime DLLs
  kUnknown,  // an address outside every module
};

// Plugins hook a game thread's entry point (an event raised when the thread
// starts, a wrapper around its loop), so their frames sit at the bottom of
// every stack that thread ever has. When the fault is deep in game code above
// such frames, they are where the thread started, not what crashed it
// (2026-10-13: NPCWaterAIFix.dll and Trails.dll under a C++ exception the game
// threw while reading ContentCatalog.txt).
//
// Frames are innermost first. A block of consecutive plugin frames is marked
// when
// - at least kMinGameFramesAbove game frames sit directly above it, and
// - below it there is exactly one game frame (the game's thread start routine)
//   and then only system frames to the end of the stack (BaseThreadInitThunk,
//   RtlUserThreadStart), so the walk reached the thread start.
// Never marked: a plugin's own thread (no game frame below it), a plugin that
// calls into the game from code the game called (more game frames below it),
// and the main thread, whose bottom holds two game frames (WinMain's loop and
// the CRT start), so a main-loop hook that itself calls the faulting game code
// keeps its blame.
inline constexpr std::size_t kMinGameFramesAboveThreadEntryHook = 3;

inline std::vector<bool> MarkThreadEntryHookFrames(const std::vector<StackFrameKind>& frames)
{
  std::vector<bool> marked(frames.size(), false);
  const std::size_t n = frames.size();
  if (n == 0 || frames[n - 1] != StackFrameKind::kSystem) {
    return marked;
  }
  for (std::size_t a = 0; a < n;) {
    if (frames[a] != StackFrameKind::kPlugin) {
      ++a;
      continue;
    }
    std::size_t b = a;
    while (b + 1 < n && frames[b + 1] == StackFrameKind::kPlugin) {
      ++b;
    }

    bool gameAbove = a >= kMinGameFramesAboveThreadEntryHook;
    for (std::size_t k = 1; gameAbove && k <= kMinGameFramesAboveThreadEntryHook; ++k) {
      gameAbove = frames[a - k] == StackFrameKind::kGame;
    }

    std::size_t i = b + 1;
    std::size_t gameBelow = 0;
    while (i < n && frames[i] == StackFrameKind::kGame) {
      ++gameBelow;
      ++i;
    }
    bool onlySystemAfter = i < n;
    for (std::size_t j = i; onlySystemAfter && j < n; ++j) {
      onlySystemAfter = frames[j] == StackFrameKind::kSystem;
    }

    if (gameAbove && gameBelow == 1 && onlySystemAfter) {
      for (std::size_t k = a; k <= b; ++k) {
        marked[k] = true;
      }
    }
    a = b + 1;
  }
  return marked;
}

}  // namespace skydiag::dump_tool
