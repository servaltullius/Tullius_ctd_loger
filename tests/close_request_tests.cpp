// ADR-0022: the plugin's close-request classification and the forced-close
// window the helper and the analyzer apply (SkyrimDiagCloseRequest.h).
// Portable: runs on Linux too.

#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <string_view>
#include <utility>

#include "SkyrimDiagCloseRequest.h"

using skydiag::CloseRequestKind;
using skydiag::CloseRequestKindId;
using skydiag::CloseRequestTracker;
using skydiag::ForcedCloseScanner;
using skydiag::kForcedCloseCrashWindowMs;
using skydiag::kScClose;
using skydiag::kWmClose;
using skydiag::kWmSysCommand;
using skydiag::ObserveCloseRequestMessage;

namespace {

constexpr std::uint32_t kWmSize = 0x0005u;
constexpr std::uint64_t kScMinimize = 0xF020u;
constexpr std::uint64_t kScKeyMenu = 0xF100u;

void Require(bool condition, std::string_view message)
{
  if (!condition) {
    std::cerr << "close-request-test failure: " << message << '\n';
    std::exit(1);
  }
}

using skydiag::kKeyAltContextBit;
using skydiag::kKeyRepeatBit;
using skydiag::kVkF4;
using skydiag::kWmQuit;
using skydiag::kWmSysKeyDown;

constexpr CloseRequestKind Obs(
  CloseRequestTracker& t,
  std::uint32_t message,
  std::uint64_t wParam,
  std::uint64_t lParam,
  bool altDown,
  bool f4Down,
  std::uint64_t nowMs)
{
  return ObserveCloseRequestMessage(&t, message, wParam, lParam, altDown, f4Down, nowMs);
}

// The lParam of an Alt+F4 key-down (scan code 0x3E, Alt held).
constexpr std::uint64_t kAltF4KeyLParam = kKeyAltContextBit | (0x3Eull << 16) | 1u;

// Alt+F4 as Windows delivers it (2026-10-26 in-game test): the key-down is
// posted, DefWindowProc posts SC_CLOSE for it, and SC_CLOSE becomes a sent
// WM_CLOSE. The key-down alone is the Alt+F4 request; the rest is the same
// one, also once the keys are up.
constexpr bool PostedAltF4IsOneRequest()
{
  CloseRequestTracker t{};
  return Obs(t, kWmSysKeyDown, kVkF4, kAltF4KeyLParam, true, true, 1000) == CloseRequestKind::kAltF4 &&
         Obs(t, kWmSysCommand, kScClose, 0, false, false, 1010) == CloseRequestKind::kNone &&
         Obs(t, kWmClose, 0, 0, false, false, 1011) == CloseRequestKind::kNone &&
         Obs(t, kWmQuit, 0, 0, false, false, 1020) == CloseRequestKind::kNone;
}
static_assert(PostedAltF4IsOneRequest());

// A game that turns Alt+F4 into its own quit never lets SC_CLOSE come: the
// key-down is enough. Alt comes from the context bit or from the key state.
static_assert([] {
  CloseRequestTracker t{};
  return Obs(t, kWmSysKeyDown, kVkF4, kAltF4KeyLParam, false, false, 5) == CloseRequestKind::kAltF4;
}());
static_assert([] {
  CloseRequestTracker t{};
  return Obs(t, kWmSysKeyDown, kVkF4, 1u, true, true, 5) == CloseRequestKind::kAltF4;
}());
// F4 without Alt, another system key, or the auto-repeat of a held Alt+F4
// is not a new request.
static_assert([] {
  CloseRequestTracker t{};
  return Obs(t, kWmSysKeyDown, kVkF4, 1u, false, true, 5) == CloseRequestKind::kNone &&
         Obs(t, kWmSysKeyDown, 0x12u, kAltF4KeyLParam, true, false, 6) == CloseRequestKind::kNone &&
         Obs(t, kWmSysKeyDown, kVkF4, kAltF4KeyLParam | kKeyRepeatBit, true, true, 7) == CloseRequestKind::kNone &&
         !t.recorded;
}());

// Sent only (no key-down seen): SC_CLOSE while Alt and F4 are held is
// still Alt+F4, and its WM_CLOSE the same request.
constexpr bool SentAltF4IsOneRequest()
{
  CloseRequestTracker t{};
  return Obs(t, kWmSysCommand, kScClose, 0, true, true, 1000) == CloseRequestKind::kAltF4 &&
         Obs(t, kWmClose, 0, 0, true, true, 1001) == CloseRequestKind::kNone;
}
static_assert(SentAltF4IsOneRequest());

// The X button (SC_CLOSE without the keys) and its WM_CLOSE.
constexpr bool XButtonIsOneWindowClose()
{
  CloseRequestTracker t{};
  return Obs(t, kWmSysCommand, kScClose | 0x2u, 0, false, false, 5000) == CloseRequestKind::kWindowClose &&
         Obs(t, kWmClose, 0, 0, false, false, 5010) == CloseRequestKind::kNone;
}
static_assert(XButtonIsOneWindowClose(), "the low bits of an SC_ command are ignored");
// Alt held alone (Alt+click) is not Alt+F4.
static_assert([] {
  CloseRequestTracker t{};
  return Obs(t, kWmSysCommand, kScClose, 0, true, false, 5000) == CloseRequestKind::kWindowClose;
}());

// WM_CLOSE alone (Task Manager's End task, another program, the game itself).
static_assert([] {
  CloseRequestTracker t{};
  return Obs(t, kWmClose, 0, 0, false, false, 100) == CloseRequestKind::kCloseMessage;
}());

// WM_QUIT alone ends the loop after any quit and does not say which one
// (the quit flag does): nothing is recorded for it.
static_assert([] {
  CloseRequestTracker t{};
  return Obs(t, kWmQuit, 0, 0, false, false, 100) == CloseRequestKind::kNone && !t.recorded;
}());

// The quit flag (2026-10-26 19:30 in-game test): Skyrim takes Alt+F4 through
// DirectInput, so no window message carried it and the flag was the only
// signal. Its kind: Alt+F4 held (now, or seen by the 25 ms sampler within
// 1.5 s), else a quit menu open or closed within 3 s, else a forced quit.
using skydiag::QuitFlagCloseRequestKind;
using skydiag::QuitFlagReason;
constexpr bool Decides(
  bool heldNow,
  std::uint64_t seenMs,
  bool menuOpen,
  std::uint64_t menuClosedMs,
  std::uint64_t nowMs,
  CloseRequestKind kind,
  QuitFlagReason reason)
{
  const auto d = QuitFlagCloseRequestKind(heldNow, seenMs, menuOpen, menuClosedMs, nowMs);
  return d.kind == kind && d.reason == reason;
}
static_assert(Decides(true, 0, false, 0, 50000, CloseRequestKind::kAltF4, QuitFlagReason::kAltF4Held));
static_assert(Decides(false, 49000, false, 0, 50000, CloseRequestKind::kAltF4, QuitFlagReason::kAltF4Held),
  "keys released, but seen held a second ago");
static_assert(Decides(false, 50000 - skydiag::kQuitFlagAltF4RecentMs, false, 0, 50000, CloseRequestKind::kAltF4,
  QuitFlagReason::kAltF4Held));
static_assert(Decides(false, 50000 - skydiag::kQuitFlagAltF4RecentMs - 1, false, 0, 50000,
  CloseRequestKind::kForcedQuit, QuitFlagReason::kNoMenuNoKeys), "an older Alt+F4 is not this quit");
// Alt+F4 outranks an open menu: it can be pressed with the console open.
static_assert(Decides(true, 0, true, 0, 50000, CloseRequestKind::kAltF4, QuitFlagReason::kAltF4Held));
static_assert(Decides(false, 0, true, 0, 50000, CloseRequestKind::kGameQuit, QuitFlagReason::kMenuOpen));
// The game closes the quit confirmation as it quits (only that one counts).
static_assert(Decides(false, 0, false, 48000, 50000, CloseRequestKind::kGameQuit, QuitFlagReason::kMenuClosed));
static_assert(Decides(false, 0, false, 50000 - skydiag::kQuitFlagMenuRecentMs - 1, 50000,
  CloseRequestKind::kForcedQuit, QuitFlagReason::kNoMenuNoKeys));
// The second test's case: no menu, keys not seen.
static_assert(Decides(false, 0, false, 0, 345779, CloseRequestKind::kForcedQuit, QuitFlagReason::kNoMenuNoKeys));
// The key sampler (2026-10-26 19:58 in-game test): the crash came in the very
// frame that handled Alt+F4, before the heartbeat task saw the quit flag, so
// the sampler records the press itself. Both keys, the game in front; one
// event per press.
using skydiag::AltF4KeySampler;
using skydiag::IsAltF4AlreadyRecorded;
using skydiag::ObserveAltF4Keys;
static_assert([] {
  AltF4KeySampler s{};
  return ObserveAltF4Keys(&s, true, true, true) &&   // pressed
         !ObserveAltF4Keys(&s, true, true, true) &&  // still held: the same press
         !ObserveAltF4Keys(&s, true, false, false) &&
         ObserveAltF4Keys(&s, true, true, true);  // pressed again
}());
static_assert([] {
  AltF4KeySampler s{};
  return !ObserveAltF4Keys(&s, true, false, false) &&  // Alt alone
         !ObserveAltF4Keys(&s, false, true, false) &&  // F4 alone
         !ObserveAltF4Keys(&s, true, true, false) &&   // Alt+F4 in another program
         !s.held && !ObserveAltF4Keys(nullptr, true, true, true);
}());
// Alt+F4 pressed in another program, then the game comes in front while the
// keys are still down: that is the game's Alt+F4 now.
static_assert([] {
  AltF4KeySampler s{};
  return !ObserveAltF4Keys(&s, true, true, false) && ObserveAltF4Keys(&s, true, true, true);
}());

// One press, several sources: the sampler records it at 1000; the crash
// handler (1020) and the quit flag (2200) see the same press within
// kQuitFlagAltF4RecentMs and record nothing; a press 1.1 s after the
// sampler's is a new one for the sampler.
constexpr bool OnePressAcrossSources()
{
  std::uint64_t recorded = 0;
  const auto claim = [&recorded](std::uint64_t now, std::uint64_t window) {
    if (IsAltF4AlreadyRecorded(recorded, now, window)) {
      return false;
    }
    recorded = now;
    return true;
  };
  return claim(1000, skydiag::kCloseRequestRepeatMs) &&
         !claim(1020, skydiag::kQuitFlagAltF4RecentMs) &&
         !claim(2200, skydiag::kQuitFlagAltF4RecentMs) &&
         claim(2101, skydiag::kCloseRequestRepeatMs) &&
         !claim(2500, skydiag::kCloseRequestRepeatMs);
}
static_assert(OnePressAcrossSources());
// When the sampler missed the keys, the crash handler or the quit flag
// records it; nothing recorded yet (0) is never a repeat.
static_assert(!IsAltF4AlreadyRecorded(0, 5000, skydiag::kQuitFlagAltF4RecentMs));
static_assert(!IsAltF4AlreadyRecorded(5001, 5000, skydiag::kQuitFlagAltF4RecentMs), "a tick from the future");

// A tick from the future (clock read order) is not "recent".
static_assert(Decides(false, 50001, false, 50001, 50000, CloseRequestKind::kForcedQuit, QuitFlagReason::kNoMenuNoKeys));

// A WM_CLOSE long after an SC_CLOSE is a new request of its own, and an
// SC_CLOSE long after an Alt+F4 key-down is no longer Alt+F4.
static_assert([] {
  CloseRequestTracker t{};
  return Obs(t, kWmSysCommand, kScClose, 0, false, false, 100) == CloseRequestKind::kWindowClose &&
         Obs(t, kWmClose, 0, 0, false, false, 100 + skydiag::kCloseRequestRepeatMs + 1) ==
           CloseRequestKind::kCloseMessage;
}());
static_assert([] {
  CloseRequestTracker t{};
  return Obs(t, kWmSysKeyDown, kVkF4, kAltF4KeyLParam, true, true, 0) == CloseRequestKind::kAltF4 &&
         Obs(t, kWmSysCommand, kScClose, 0, false, false, skydiag::kCloseRequestRepeatMs + 1) ==
           CloseRequestKind::kWindowClose;
}());

// Pressing Alt+F4 again after a second (the game is slow to close) records
// it again; the window keeps the nearer one.
static_assert([] {
  CloseRequestTracker t{};
  return Obs(t, kWmSysKeyDown, kVkF4, kAltF4KeyLParam, true, true, 0) == CloseRequestKind::kAltF4 &&
         Obs(t, kWmSysKeyDown, kVkF4, kAltF4KeyLParam, true, true, 500) == CloseRequestKind::kNone &&
         Obs(t, kWmSysKeyDown, kVkF4, kAltF4KeyLParam, true, true, 1500) == CloseRequestKind::kAltF4;
}());

// Other messages and other system commands are not close requests.
constexpr bool OtherMessagesAreIgnored()
{
  constexpr std::uint32_t kWmKeyDown = 0x0100u;
  CloseRequestTracker t{};
  return Obs(t, kWmSysCommand, kScMinimize, 0, false, false, 1) == CloseRequestKind::kNone &&
         Obs(t, kWmSysCommand, kScKeyMenu, 0, true, false, 2) == CloseRequestKind::kNone &&
         Obs(t, kWmSize, 0, 0, true, true, 3) == CloseRequestKind::kNone &&
         Obs(t, kWmKeyDown, kVkF4, kAltF4KeyLParam, true, true, 4) == CloseRequestKind::kNone &&
         ObserveCloseRequestMessage(nullptr, kWmClose, 0, 0, true, true, 4) == CloseRequestKind::kNone &&
         !t.recorded;
}
static_assert(OtherMessagesAreIgnored());

constexpr ForcedCloseScanner Scan(
  std::initializer_list<double> crashes,
  std::initializer_list<std::pair<double, std::uint64_t>> requests,
  bool requestsFirst = true)
{
  ForcedCloseScanner scanner;
  if (!requestsFirst) {
    for (const double crash : crashes) {
      scanner.AddCrash(crash);
    }
  }
  for (const auto& [time, kind] : requests) {
    scanner.AddCloseRequest(time, kind);
  }
  if (requestsFirst) {
    for (const double crash : crashes) {
      scanner.AddCrash(crash);
    }
  }
  return scanner;
}

constexpr std::uint64_t K(CloseRequestKind kind)
{
  return static_cast<std::uint64_t>(kind);
}

// ADR-0035: only the quit flag's kinds mean the game was quitting.
static_assert(skydiag::IsQuitFlagCloseRequest(K(CloseRequestKind::kGameQuit)));
static_assert(skydiag::IsQuitFlagCloseRequest(K(CloseRequestKind::kForcedQuit)));
static_assert(!skydiag::IsQuitFlagCloseRequest(K(CloseRequestKind::kAltF4)));
static_assert(!skydiag::IsQuitFlagCloseRequest(K(CloseRequestKind::kWindowClose)));
static_assert(!skydiag::IsQuitFlagCloseRequest(K(CloseRequestKind::kCloseMessage)));
static_assert(!skydiag::IsQuitFlagCloseRequest(K(CloseRequestKind::kNone)));
static_assert(!skydiag::IsQuitFlagCloseRequest(99u));

// CloseRequestSource::kQuitFlag in payload.d also marks an Alt+F4 quit-flag
// transition as a quit-flag event, whereas key-sampled Alt+F4 is not.
constexpr std::uint64_t S(skydiag::CloseRequestSource source)
{
  return static_cast<std::uint64_t>(source);
}
static_assert(skydiag::IsQuitFlagCloseRequestEvent(K(CloseRequestKind::kAltF4), S(skydiag::CloseRequestSource::kQuitFlag)));
static_assert(!skydiag::IsQuitFlagCloseRequestEvent(K(CloseRequestKind::kAltF4), S(skydiag::CloseRequestSource::kKeys)));
static_assert(!skydiag::IsQuitFlagCloseRequestEvent(K(CloseRequestKind::kNone), S(skydiag::CloseRequestSource::kQuitFlag)));
static_assert(skydiag::IsQuitFlagCloseRequestEvent(K(CloseRequestKind::kGameQuit), S(skydiag::CloseRequestSource::kSentMessage)));

// The 2026-10-26 case: Alt+F4, and 0.8 s later the access violation.
static_assert(Scan({ 120800.0 }, { { 120000.0, K(CloseRequestKind::kAltF4) } }).Result().kind == CloseRequestKind::kAltF4);
static_assert(Scan({ 120800.0 }, { { 120000.0, K(CloseRequestKind::kAltF4) } }).Result().msBeforeCrash == 800.0);
// The window's edge is inside; a millisecond past it is not.
static_assert(Scan({ 10000.0 + kForcedCloseCrashWindowMs }, { { 10000.0, K(CloseRequestKind::kAltF4) } }).Result().matched());
static_assert(!Scan({ 10001.0 + kForcedCloseCrashWindowMs }, { { 10000.0, K(CloseRequestKind::kAltF4) } }).Result().matched());
// No crash event, no close request, or a request only after the crash: an
// ordinary crash.
static_assert(!Scan({}, { { 1000.0, K(CloseRequestKind::kAltF4) } }).Result().matched());
static_assert(!Scan({ 5000.0 }, {}).Result().matched());
static_assert(!Scan({ 5000.0 }, { { 5000.5, K(CloseRequestKind::kWindowClose) } }, false).Result().matched());
// The newest crash decides: an Alt+F4 near an older, released exception does
// not make a later crash a forced close.
static_assert(!Scan({ 2000.0, 60000.0 }, { { 1500.0, K(CloseRequestKind::kAltF4) } }).Result().matched());
// An unknown kind (a newer plugin) is skipped.
static_assert(!Scan({ 2000.0 }, { { 1500.0, 99u }, { 1600.0, 0u } }).Result().matched());

// When several came, the player's own key or click names the close: the
// game's quit flag and a bare WM_CLOSE (which the game may send itself) come
// after it.
static_assert(Scan({ 9000.0 },
                { { 1000.0, K(CloseRequestKind::kAltF4) },
                  { 1200.0, K(CloseRequestKind::kGameQuit) },
                  { 1300.0, K(CloseRequestKind::kCloseMessage) } })
                .Result()
                .kind == CloseRequestKind::kAltF4);
static_assert(Scan({ 9000.0 },
                { { 1000.0, K(CloseRequestKind::kCloseMessage) }, { 1200.0, K(CloseRequestKind::kGameQuit) } })
                .Result()
                .kind == CloseRequestKind::kGameQuit);
// The X button is named over the quit flag it leads to; a forced quit over
// the WM_CLOSE the game may send itself.
static_assert(Scan({ 9000.0 },
                { { 1000.0, K(CloseRequestKind::kWindowClose) }, { 1100.0, K(CloseRequestKind::kForcedQuit) } })
                .Result()
                .kind == CloseRequestKind::kWindowClose);
static_assert(Scan({ 9000.0 },
                { { 1000.0, K(CloseRequestKind::kCloseMessage) }, { 1100.0, K(CloseRequestKind::kForcedQuit) } })
                .Result()
                .kind == CloseRequestKind::kForcedQuit);
static_assert(Scan({ 345779.0 }, { { 345779.0, K(CloseRequestKind::kForcedQuit) } }).Result().matched(),
  "the flag seen right before the crash");
// Of the same kind, the nearer one gives the time.
static_assert(Scan({ 9000.0 },
                { { 1000.0, K(CloseRequestKind::kAltF4) }, { 8000.0, K(CloseRequestKind::kAltF4) } })
                .Result()
                .msBeforeCrash == 1000.0);

// ADR-0022 (2026-11-17): the quit flag has no time limit. Once it is set the
// main loop has ended, so a crash minutes later (qqq, then a shutdown of over
// two minutes under MO2's usvfs) is still in the shutdown. Alt+F4 seen by the
// keys, the X button and a close message keep the window: the game may have
// ignored them.
static_assert(skydiag::CloseRequestCoversCrash(CloseRequestKind::kGameQuit, 130000.0));
static_assert(skydiag::CloseRequestCoversCrash(CloseRequestKind::kForcedQuit, 3600000.0));
static_assert(skydiag::CloseRequestCoversCrash(CloseRequestKind::kGameQuit, 0.0));
static_assert(!skydiag::CloseRequestCoversCrash(CloseRequestKind::kGameQuit, -0.5), "after the crash");
static_assert(skydiag::CloseRequestCoversCrash(CloseRequestKind::kAltF4, static_cast<double>(kForcedCloseCrashWindowMs)));
static_assert(!skydiag::CloseRequestCoversCrash(CloseRequestKind::kAltF4, kForcedCloseCrashWindowMs + 1.0));
static_assert(!skydiag::CloseRequestCoversCrash(CloseRequestKind::kWindowClose, kForcedCloseCrashWindowMs + 1.0));
static_assert(!skydiag::CloseRequestCoversCrash(CloseRequestKind::kCloseMessage, kForcedCloseCrashWindowMs + 1.0));
static_assert(!skydiag::CloseRequestCoversCrash(CloseRequestKind::kNone, 0.0));
static_assert(!skydiag::CloseRequestCoversCrash(static_cast<CloseRequestKind>(99), 0.0));
// The summary JSON's window_seconds goes with the windowed kinds only.
static_assert(skydiag::CloseRequestKindIdHasWindow("alt_f4"));
static_assert(skydiag::CloseRequestKindIdHasWindow("window_close"));
static_assert(skydiag::CloseRequestKindIdHasWindow("close_message"));
static_assert(!skydiag::CloseRequestKindIdHasWindow("game_quit"));
static_assert(!skydiag::CloseRequestKindIdHasWindow("forced_quit"));
static_assert(!skydiag::CloseRequestKindIdHasWindow(""));

// qqq, and 130 s later the crash.
static_assert(Scan({ 230000.0 }, { { 100000.0, K(CloseRequestKind::kGameQuit) } }).Result().kind ==
              CloseRequestKind::kGameQuit);
static_assert(Scan({ 230000.0 }, { { 100000.0, K(CloseRequestKind::kGameQuit) } }).Result().msBeforeCrash == 130000.0);
static_assert(Scan({ 3700000.0 }, { { 100000.0, K(CloseRequestKind::kForcedQuit) } }).Result().kind ==
              CloseRequestKind::kForcedQuit);
// Still only before the crash, and the newest crash decides.
static_assert(!Scan({ 5000.0 }, { { 5000.5, K(CloseRequestKind::kGameQuit) } }, false).Result().matched());
static_assert(!Scan({ 2000.0, 60000.0 }, { { 61000.0, K(CloseRequestKind::kGameQuit) } }).Result().matched());
static_assert(Scan({ 2000.0, 60000.0 }, { { 1000.0, K(CloseRequestKind::kForcedQuit) } }).Result().matched(),
  "a released exception after the flag does not reset it");
// An Alt+F4 long before the crash no longer names it; the quit flag it led to
// does.
static_assert(Scan({ 200000.0 },
                { { 1000.0, K(CloseRequestKind::kAltF4) }, { 1100.0, K(CloseRequestKind::kForcedQuit) } })
                .Result()
                .kind == CloseRequestKind::kForcedQuit);
static_assert(Scan({ 200000.0 },
                { { 1000.0, K(CloseRequestKind::kAltF4) }, { 1100.0, K(CloseRequestKind::kForcedQuit) } })
                .Result()
                .msBeforeCrash == 198900.0);
// Alt+F4 pressed during the slow shutdown, 2 s before the crash, names it.
static_assert(Scan({ 200000.0 },
                { { 1000.0, K(CloseRequestKind::kGameQuit) }, { 198000.0, K(CloseRequestKind::kAltF4) } })
                .Result()
                .kind == CloseRequestKind::kAltF4);
// A close message alone keeps the window.
static_assert(!Scan({ 200000.0 }, { { 1000.0, K(CloseRequestKind::kCloseMessage) } }).Result().matched());

}  // namespace

int main()
{
  // The ids the summary JSON and the viewer read.
  Require(std::strcmp(CloseRequestKindId(CloseRequestKind::kAltF4), "alt_f4") == 0, "alt_f4 id");
  Require(std::strcmp(CloseRequestKindId(CloseRequestKind::kWindowClose), "window_close") == 0, "window_close id");
  Require(std::strcmp(CloseRequestKindId(CloseRequestKind::kCloseMessage), "close_message") == 0, "close_message id");
  Require(std::strcmp(CloseRequestKindId(CloseRequestKind::kGameQuit), "game_quit") == 0, "game_quit id");
  Require(std::strcmp(CloseRequestKindId(CloseRequestKind::kForcedQuit), "forced_quit") == 0, "forced_quit id");
  Require(std::strcmp(CloseRequestKindId(CloseRequestKind::kNone), "") == 0, "no id for kNone");

  // Only the newest kKeptRequests requests are kept; a flood of old ones does
  // not push out the one before the crash.
  ForcedCloseScanner scanner;
  for (int i = 0; i < 20; ++i) {
    scanner.AddCloseRequest(100.0 * i, static_cast<std::uint64_t>(CloseRequestKind::kCloseMessage));
  }
  scanner.AddCloseRequest(50000.0, static_cast<std::uint64_t>(CloseRequestKind::kWindowClose));
  scanner.AddCrash(52500.0);
  const auto match = scanner.Result();
  Require(match.kind == CloseRequestKind::kWindowClose && match.msBeforeCrash == 2500.0, "the newest request is kept");

  // The quit flag is kept apart: the WM_CLOSEs and other requests that come
  // during a long shutdown do not push it out.
  ForcedCloseScanner shutdown;
  shutdown.AddCloseRequest(100000.0, static_cast<std::uint64_t>(CloseRequestKind::kGameQuit));
  for (int i = 0; i < 20; ++i) {
    shutdown.AddCloseRequest(100500.0 + 1000.0 * i, static_cast<std::uint64_t>(CloseRequestKind::kCloseMessage));
  }
  shutdown.AddCrash(230000.0);
  const auto afterQuit = shutdown.Result();
  Require(afterQuit.kind == CloseRequestKind::kGameQuit && afterQuit.msBeforeCrash == 130000.0 && afterQuit.fromQuitFlag,
    "the quit flag outlives a flood of close messages");

  // Alt+F4 recorded from Main::quitGame (payload.d == kQuitFlag) has no 10 s
  // time limit and survives a flood of window close messages, even when a
  // key-sampled Alt+F4 also fired right before it.
  ForcedCloseScanner altF4QuitFlag;
  altF4QuitFlag.AddCloseRequest(100000.0, K(CloseRequestKind::kAltF4), S(skydiag::CloseRequestSource::kKeys));
  altF4QuitFlag.AddCloseRequest(100025.0, K(CloseRequestKind::kAltF4), S(skydiag::CloseRequestSource::kQuitFlag));
  for (int i = 0; i < 20; ++i) {
    altF4QuitFlag.AddCloseRequest(100500.0 + 1000.0 * i, K(CloseRequestKind::kCloseMessage));
  }
  altF4QuitFlag.AddCrash(230025.0);
  const auto altF4Match = altF4QuitFlag.Result();
  Require(
    altF4Match.kind == CloseRequestKind::kAltF4 && altF4Match.msBeforeCrash == 130000.0 && altF4Match.fromQuitFlag,
    "Alt+F4 from Main::quitGame covers a slow-shutdown crash past 10 s");
  return 0;
}
