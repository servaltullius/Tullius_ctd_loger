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

// EventType::kCloseRequest payload.a. Payload b is the window message (0 when
// none), c its wParam (the QuitFlagReason for the quit flag), d the
// CloseRequestSource.
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
  // The game's quit flag (Main::quitGame) became set while a menu one quits
  // from was open or had just closed: Quit (to desktop) in its menus, or qqq
  // in the console.
  kGameQuit = 4,
  // The quit flag became set with no such menu and without Alt+F4 seen held:
  // a forced quit, most likely Alt+F4 (Skyrim reads the keyboard through
  // DirectInput and quits on Alt+F4 itself; no window message carries it).
  kForcedQuit = 5,
};

// Where the plugin saw it (payload d).
enum class CloseRequestSource : std::uint8_t
{
  kSentMessage = 0,    // WH_CALLWNDPROC
  kPostedMessage = 1,  // WH_GETMESSAGE
  kQuitFlag = 2,       // Main::quitGame became set (heartbeat task)
  kKeys = 3,           // Alt+F4 held with the game in front (25 ms key sampler)
  kKeysAtCrash = 4,    // Alt+F4 held with the game in front when the crash was recorded
};

constexpr bool IsCloseRequestKind(std::uint64_t value) noexcept
{
  return value >= static_cast<std::uint64_t>(CloseRequestKind::kAltF4) &&
         value <= static_cast<std::uint64_t>(CloseRequestKind::kForcedQuit);
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
    case CloseRequestKind::kForcedQuit:
      return "forced_quit";
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
      return L"quit from the game's menu";
    case CloseRequestKind::kForcedQuit:
      return L"forced quit, probably Alt+F4";
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
    // The loop ends after any quit; alone it does not say which one (the
    // quit flag does, QuitFlagCloseRequestKind).
    if (!afterAltF4) {
      return CloseRequestKind::kNone;
    }
    kind = CloseRequestKind::kAltF4;
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

// Skyrim takes Alt+F4 through DirectInput and sets its quit flag itself, so
// the change of Main::quitGame is the signal every quit gives; what it was is
// decided when it is seen:
//   - Alt and F4 held then, or seen held (GetAsyncKeyState, which DirectInput
//     does not hide) within kQuitFlagAltF4RecentMs: Alt+F4;
//   - else a menu open as one quits (the confirmation MessageBoxMenu, Main
//     Menu, Console, the Journal's System page) still open, or the confirmation closed within
//     kQuitFlagMenuRecentMs (the game closes it as it quits): Quit from the
//     menu. Any other menu just closed does not count: a console closed
//     before an Alt+F4 the sampler missed would read as Quit;
//   - else a forced quit, most likely Alt+F4 the plugin did not see held.
inline constexpr std::uint64_t kQuitFlagAltF4RecentMs = 1500;
inline constexpr std::uint64_t kQuitFlagMenuRecentMs = 3000;

enum class QuitFlagReason : std::uint8_t
{
  kAltF4Held,
  kMenuOpen,
  kMenuClosed,
  kNoMenuNoKeys,
};

struct QuitFlagDecision
{
  CloseRequestKind kind = CloseRequestKind::kForcedQuit;
  QuitFlagReason reason = QuitFlagReason::kNoMenuNoKeys;
};

// altF4HeldNow: both keys down when the flag was seen. altF4SeenMs /
// quitMenuClosedMs: GetTickCount64 of the last time (0: never).
constexpr QuitFlagDecision QuitFlagCloseRequestKind(
  bool altF4HeldNow,
  std::uint64_t altF4SeenMs,
  bool quitMenuOpen,
  std::uint64_t quitMenuClosedMs,
  std::uint64_t nowMs) noexcept
{
  const auto recent = [nowMs](std::uint64_t thenMs, std::uint64_t windowMs) {
    return thenMs != 0u && nowMs >= thenMs && nowMs - thenMs <= windowMs;
  };
  if (altF4HeldNow || recent(altF4SeenMs, kQuitFlagAltF4RecentMs)) {
    return { CloseRequestKind::kAltF4, QuitFlagReason::kAltF4Held };
  }
  if (quitMenuOpen) {
    return { CloseRequestKind::kGameQuit, QuitFlagReason::kMenuOpen };
  }
  if (recent(quitMenuClosedMs, kQuitFlagMenuRecentMs)) {
    return { CloseRequestKind::kGameQuit, QuitFlagReason::kMenuClosed };
  }
  return { CloseRequestKind::kForcedQuit, QuitFlagReason::kNoMenuNoKeys };
}

// The 25 ms key sampler: true on the sample where Alt and F4 become held
// together while the game window is the foreground window (Alt+F4 in
// another program while the game runs behind it does not count). Holding
// the keys records one press; releasing and pressing again records another,
// subject to the shared repeat check below.
struct AltF4KeySampler
{
  bool held = false;
};

constexpr bool ObserveAltF4Keys(AltF4KeySampler* sampler, bool altDown, bool f4Down, bool gameForeground) noexcept
{
  if (!sampler) {
    return false;
  }
  const bool held = altDown && f4Down && gameForeground;
  const bool pressed = held && !sampler->held;
  sampler->held = held;
  return pressed;
}

// One Alt+F4 can be seen by the key sampler, a window hook, the crash
// handler and the quit flag; the first records it, and an Alt+F4 recorded
// within windowMs before is the same press (kCloseRequestRepeatMs for the
// sampler and the hooks, kQuitFlagAltF4RecentMs for the quit flag and the
// crash, which come later in the same press).
constexpr bool IsAltF4AlreadyRecorded(std::uint64_t recordedMs, std::uint64_t nowMs, std::uint64_t windowMs) noexcept
{
  return recordedMs != 0u && nowMs >= recordedMs && nowMs - recordedMs <= windowMs;
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
// click first, then the quit flag (menu, then forced), then a bare WM_CLOSE
// (which the game may send itself while quitting).
constexpr int CloseRequestRank(CloseRequestKind kind) noexcept
{
  switch (kind) {
    case CloseRequestKind::kAltF4:
      return 0;
    case CloseRequestKind::kWindowClose:
      return 1;
    case CloseRequestKind::kGameQuit:
      return 2;
    case CloseRequestKind::kForcedQuit:
      return 3;
    case CloseRequestKind::kCloseMessage:
      return 4;
    default:
      return 5;
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
