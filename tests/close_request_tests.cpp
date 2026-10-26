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

// WM_QUIT alone: the game's loop is ending on its own (Quit to desktop).
static_assert([] {
  CloseRequestTracker t{};
  return Obs(t, kWmQuit, 0, 0, false, false, 100) == CloseRequestKind::kGameQuit;
}());

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
// Of the same kind, the nearer one gives the time.
static_assert(Scan({ 9000.0 },
                { { 1000.0, K(CloseRequestKind::kAltF4) }, { 8000.0, K(CloseRequestKind::kAltF4) } })
                .Result()
                .msBeforeCrash == 1000.0);

}  // namespace

int main()
{
  // The ids the summary JSON and the viewer read.
  Require(std::strcmp(CloseRequestKindId(CloseRequestKind::kAltF4), "alt_f4") == 0, "alt_f4 id");
  Require(std::strcmp(CloseRequestKindId(CloseRequestKind::kWindowClose), "window_close") == 0, "window_close id");
  Require(std::strcmp(CloseRequestKindId(CloseRequestKind::kCloseMessage), "close_message") == 0, "close_message id");
  Require(std::strcmp(CloseRequestKindId(CloseRequestKind::kGameQuit), "game_quit") == 0, "game_quit id");
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
  return 0;
}
