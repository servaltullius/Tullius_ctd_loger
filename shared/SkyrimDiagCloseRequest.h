#pragma once

// ADR-0022: a request to close the game (Alt+F4, the window's X button, the
// game's own Quit) recorded in the blackbox (EventType::kCloseRequest), and
// the rule that makes a crash right after one a forced-close error, not a CTD.
// The plugin classifies the window messages, the helper and the analyzer
// apply the same rule to the blackbox.
//
// Deliberately free of <Windows.h>: the Linux unit tests use it too.

#include <cstddef>
#include <cstdint>

namespace skydiag {

// EventType::kCloseRequest payload.a. Payload b is the window message (0 for
// the quit flag), c its wParam, d 1 when the message was posted (seen by the
// WH_GETMESSAGE hook) and 0 when it was sent (WH_CALLWNDPROC).
enum class CloseRequestKind : std::uint8_t
{
  kNone = 0,
  // Alt+F4 on the game window (Alt and F4 held when the close came).
  kAltF4 = 1,
  // SC_CLOSE without Alt+F4: the X button, the window menu, the taskbar's
  // "Close window".
  kWindowClose = 2,
  // WM_CLOSE with no SC_CLOSE before it: another program (Task Manager's End
  // task) or the game itself asked the window to close.
  kCloseMessage = 3,
  // The game is quitting on its own: its quit flag (Main::quitGame) became
  // set, or WM_QUIT reached its message loop (Quit to desktop in its menus,
  // or the game quitting after one of the above).
  kGameQuit = 4,
};

constexpr bool IsCloseRequestKind(std::uint64_t value) noexcept
{
  return value >= static_cast<std::uint64_t>(CloseRequestKind::kAltF4) &&
         value <= static_cast<std::uint64_t>(CloseRequestKind::kGameQuit);
}

// Summary JSON / report / helper log id.
constexpr const char* CloseRequestKindId(CloseRequestKind kind) noexcept
{
  switch (kind) {
    case CloseRequestKind::kAltF4:
      return "alt_f4";
    case CloseRequestKind::kWindowClose:
      return "window_close";
    case CloseRequestKind::kCloseMessage:
      return "close_message";
    case CloseRequestKind::kGameQuit:
      return "game_quit";
    default:
      return "";
  }
}

// Helper log wording: "Alt+F4", "the window was closed", ...
constexpr const wchar_t* CloseRequestKindLabel(CloseRequestKind kind) noexcept
{
  switch (kind) {
    case CloseRequestKind::kAltF4:
      return L"Alt+F4";
    case CloseRequestKind::kWindowClose:
      return L"window closed";
    case CloseRequestKind::kCloseMessage:
      return L"close request to the window";
    case CloseRequestKind::kGameQuit:
      return L"quit from the game";
    default:
      return L"";
  }
}

// The window messages the plugin looks at (Windows values).
inline constexpr std::uint32_t kWmClose = 0x0010u;
inline constexpr std::uint32_t kWmQuit = 0x0012u;
inline constexpr std::uint32_t kWmSysKeyDown = 0x0104u;
inline constexpr std::uint32_t kWmSysCommand = 0x0112u;
inline constexpr std::uint64_t kScClose = 0xF060u;
inline constexpr std::uint64_t kVkF4 = 0x73u;
// WM_SYSKEYDOWN lParam: bit 29 is the context code (Alt held), bit 30 the
// previous key state (set on auto-repeat).
inline constexpr std::uint64_t kKeyAltContextBit = 1ull << 29;
inline constexpr std::uint64_t kKeyRepeatBit = 1ull << 30;

// One Alt+F4 is a posted WM_SYSKEYDOWN, then SC_CLOSE (DefWindowProc posts
// it), then WM_CLOSE (DefWindowProc sends it for SC_CLOSE); a request of the
// same kind within this time is the same one, and an SC_CLOSE / WM_CLOSE this
// soon after Alt+F4 came from it.
inline constexpr std::uint64_t kCloseRequestRepeatMs = 1000;

// Plugin state for ObserveCloseRequestMessage; only the window's thread
// touches it.
struct CloseRequestTracker
{
  bool altF4KeySeen = false;
  std::uint64_t altF4KeyMs = 0;
  bool sysCloseSeen = false;
  std::uint64_t sysCloseMs = 0;
  bool recorded = false;
  CloseRequestKind recordedKind = CloseRequestKind::kNone;
  std::uint64_t recordedMs = 0;
};

constexpr bool IsWithinCloseRequestRepeat(bool seen, std::uint64_t thenMs, std::uint64_t nowMs) noexcept
{
  return seen && nowMs >= thenMs && nowMs - thenMs <= kCloseRequestRepeatMs;
}

// A message for the game window, posted (taken from the queue) or sent.
// altDown / f4Down: GetKeyState of Alt and F4, the input state of the
// message being handled. Returns the kind to record, or kNone (not a close,
// or the same close again).
//
// WM_SYSKEYDOWN F4 with Alt counts as Alt+F4 by itself, before any SC_CLOSE:
// a game that turns Alt+F4 into its own quit never lets DefWindowProc post
// one. If the game ignores the keys nothing closes, and only a crash within
// kForcedCloseCrashWindowMs after would be taken for a forced close.
constexpr CloseRequestKind ObserveCloseRequestMessage(
  CloseRequestTracker* tracker,
  std::uint32_t message,
  std::uint64_t wParam,
  std::uint64_t lParam,
  bool altDown,
  bool f4Down,
  std::uint64_t nowMs) noexcept
{
  if (!tracker) {
    return CloseRequestKind::kNone;
  }
  const bool afterAltF4 =
    (altDown && f4Down) || IsWithinCloseRequestRepeat(tracker->altF4KeySeen, tracker->altF4KeyMs, nowMs);
  CloseRequestKind kind = CloseRequestKind::kNone;
  if (message == kWmSysKeyDown) {
    // An auto-repeat of a held Alt+F4 is the same press.
    if ((wParam & 0xFFFFu) != kVkF4 || (lParam & kKeyRepeatBit) != 0u ||
        ((lParam & kKeyAltContextBit) == 0u && !altDown)) {
      return CloseRequestKind::kNone;
    }
    kind = CloseRequestKind::kAltF4;
    tracker->altF4KeySeen = true;
    tracker->altF4KeyMs = nowMs;
  } else if (message == kWmSysCommand) {
    // The low four bits of an SC_ command are the system's own.
    if ((wParam & 0xFFF0u) != kScClose) {
      return CloseRequestKind::kNone;
    }
    kind = afterAltF4 ? CloseRequestKind::kAltF4 : CloseRequestKind::kWindowClose;
    tracker->sysCloseSeen = true;
    tracker->sysCloseMs = nowMs;
  } else if (message == kWmQuit) {
    kind = afterAltF4 ? CloseRequestKind::kAltF4 : CloseRequestKind::kGameQuit;
  } else if (message == kWmClose) {
    if (afterAltF4) {
      kind = CloseRequestKind::kAltF4;
    } else if (IsWithinCloseRequestRepeat(tracker->sysCloseSeen, tracker->sysCloseMs, nowMs)) {
      kind = CloseRequestKind::kWindowClose;
    } else {
      kind = CloseRequestKind::kCloseMessage;
    }
  } else {
    return CloseRequestKind::kNone;
  }
  if (tracker->recorded && tracker->recordedKind == kind &&
      IsWithinCloseRequestRepeat(true, tracker->recordedMs, nowMs)) {
    return CloseRequestKind::kNone;
  }
  tracker->recorded = true;
  tracker->recordedKind = kind;
  tracker->recordedMs = nowMs;
  return kind;
}

// A crash this soon after a close request is part of the game closing: after
// Alt+F4 Skyrim stops its main loop and tears down (renderer, heaps, plugin
// DLLs), which takes a few seconds with a large load order. Ten seconds covers
// a slow teardown; a crash in play that soon after a close the game ignored is
// far less likely than one in the teardown.
inline constexpr std::uint64_t kForcedCloseCrashWindowMs = 10'000;

struct ForcedCloseMatch
{
  CloseRequestKind kind = CloseRequestKind::kNone;
  // From the close request to the crash.
  double msBeforeCrash = 0.0;

  constexpr bool matched() const noexcept { return kind != CloseRequestKind::kNone; }
};

// Which request names the close when several came: the player's own key or
// click first, then the game's quit flag, then a bare WM_CLOSE (which the
// game may send itself while quitting).
constexpr int CloseRequestRank(CloseRequestKind kind) noexcept
{
  switch (kind) {
    case CloseRequestKind::kAltF4:
      return 0;
    case CloseRequestKind::kWindowClose:
      return 1;
    case CloseRequestKind::kGameQuit:
      return 2;
    case CloseRequestKind::kCloseMessage:
      return 3;
    default:
      return 4;
  }
}

// Reads the blackbox oldest first (AddCloseRequest for kCloseRequest events,
// AddCrash for kCrash events, times on one clock in milliseconds) and finds a
// close request at most kForcedCloseCrashWindowMs before the newest crash.
class ForcedCloseScanner
{
public:
  constexpr void AddCloseRequest(double timeMs, std::uint64_t payloadA) noexcept
  {
    if (!IsCloseRequestKind(payloadA)) {
      return;
    }
    requests_[next_] = Request{ static_cast<CloseRequestKind>(payloadA), timeMs };
    next_ = (next_ + 1u) % kKeptRequests;
    if (count_ < kKeptRequests) {
      ++count_;
    }
  }

  constexpr void AddCrash(double timeMs) noexcept
  {
    hasCrash_ = true;
    crashMs_ = timeMs;
  }

  constexpr ForcedCloseMatch Result() const noexcept
  {
    ForcedCloseMatch best{};
    if (!hasCrash_) {
      return best;
    }
    for (std::size_t i = 0; i < count_; ++i) {
      const Request& request = requests_[i];
      if (request.timeMs > crashMs_ || crashMs_ - request.timeMs > static_cast<double>(kForcedCloseCrashWindowMs)) {
        continue;
      }
      const double before = crashMs_ - request.timeMs;
      const int rank = CloseRequestRank(request.kind);
      const int bestRank = CloseRequestRank(best.kind);
      // The nearer one of the same kind.
      if (rank < bestRank || (rank == bestRank && before < best.msBeforeCrash)) {
        best.kind = request.kind;
        best.msBeforeCrash = before;
      }
    }
    return best;
  }

private:
  // Close requests are rare (one per close); the newest few are enough.
  static constexpr std::size_t kKeptRequests = 8;
  struct Request
  {
    CloseRequestKind kind = CloseRequestKind::kNone;
    double timeMs = 0.0;
  };
  Request requests_[kKeptRequests]{};
  std::size_t count_ = 0;
  std::size_t next_ = 0;
  bool hasCrash_ = false;
  double crashMs_ = 0.0;
};

}  // namespace skydiag
