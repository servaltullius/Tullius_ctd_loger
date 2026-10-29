// CommonLibSSE-NG must come before any Windows API header (REX/W32/BASE.h).
#include <RE/Skyrim.h>

#include "SkyrimDiag/CloseRequestHooks.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

#include "SkyrimDiag/Blackbox.h"
#include "SkyrimDiagCloseRequest.h"

namespace skydiag::plugin {
namespace {

using skydiag::CloseRequestKind;
using skydiag::CloseRequestSource;

std::atomic_bool g_installed{ false };
// The game window thread that set the window hooks (tried there once), and
// the one a wait was last logged for.
std::atomic<DWORD> g_hookedThread{ 0 };
std::atomic<DWORD> g_hooksWaitLoggedFor{ 0 };
std::atomic_bool g_firstRequestLogged{ false };
// The game window, captured when the log starts and kept current by the
// heartbeat task (the window can be recreated): the key sampler and the crash
// handler compare the foreground window with it without touching Main.
std::atomic<std::uintptr_t> g_gameWindowValue{ 0 };

// Written by the key sampler (heartbeat scheduler thread), the menu sink (UI
// thread), the hooks (game window thread) and the crash handler, read when the
// quit flag is recorded: atomics only.
std::atomic<std::uint64_t> g_altF4SeenMs{ 0 };      // Alt+F4 last seen held with the game in front
std::atomic<std::uint64_t> g_altF4RecordedMs{ 0 };  // an alt_f4 request last recorded (any source)
std::atomic<std::uint32_t> g_quitMenusOpen{ 0 };     // bit per kQuitMenus entry
std::atomic<std::uint64_t> g_quitConfirmClosedMs{ 0 };

// The quit flag (&Main::quitGame), captured when the log starts. Read by the
// heartbeat task, the key sampler and the crash handler, whichever sees it
// set first: a Quit from a menu ends the main loop in the next frame, before
// the heartbeat task (a UI task) runs again (2026-10-27 15:11 and 16:15).
std::atomic<std::uintptr_t> g_quitFlagAddress{ 0 };
std::atomic_bool g_quitFlagSeenClear{ false };
std::atomic_bool g_quitFlagRecorded{ false };

// Menus open as one quits: the confirmation (MessageBoxMenu), the main menu,
// the console (qqq), and the Journal, whose System page asks its own
// confirmation inside the Journal: a Quit from it read as "no menu" in game
// (2026-10-27 17:09) while the Journal was left out.
constexpr std::string_view kQuitMenus[] = {
  RE::MessageBoxMenu::MENU_NAME,
  RE::MainMenu::MENU_NAME,
  RE::Console::MENU_NAME,
  RE::JournalMenu::MENU_NAME,
};
// Only the confirmation counts once closed (the game closes it as it quits):
// a console or Journal closed before an Alt+F4 the sampler missed must not
// read as Quit.
constexpr std::uint32_t kQuitConfirmMenu = 0;

// Only the hooked game window thread gets past Observe's thread check, so it
// alone touches this.
skydiag::CloseRequestTracker g_tracker{};
// Only the heartbeat scheduler thread touches this.
skydiag::AltF4KeySampler g_keySampler{};

bool AltF4HeldNow() noexcept
{
  return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 && (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
}

bool GameIsForeground() noexcept
{
  const auto game = g_gameWindowValue.load(std::memory_order_relaxed);
  return game != 0u && reinterpret_cast<std::uintptr_t>(GetForegroundWindow()) == game;
}

// One Alt+F4 is recorded once whichever source sees it first
// (IsAltF4AlreadyRecorded); false when it already was.
bool TryClaimAltF4(std::uint64_t nowMs, std::uint64_t windowMs) noexcept
{
  std::uint64_t previous = g_altF4RecordedMs.load();
  for (;;) {
    if (skydiag::IsAltF4AlreadyRecorded(previous, nowMs, windowMs)) {
      return false;
    }
    if (g_altF4RecordedMs.compare_exchange_weak(previous, nowMs)) {
      return true;
    }
  }
}

// The quit flag through ReadProcessMemory on this process: Main may already
// be gone at shutdown, and a read that fails returns false where a direct
// read would fault into our own crash handler.
bool QuitFlagSetNow() noexcept
{
  const auto address = g_quitFlagAddress.load(std::memory_order_relaxed);
  if (address == 0u) {
    return false;
  }
  std::uint8_t value = 0;
  SIZE_T read = 0;
  return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), &value, sizeof(value), &read) &&
         read == sizeof(value) && value != 0u;
}

// True once, for the caller that first sees the quit flag change from clear
// to set (set from the start counts as nothing).
bool ClaimQuitFlagChange() noexcept
{
  if (g_quitFlagRecorded.load()) {
    return false;
  }
  if (!QuitFlagSetNow()) {
    g_quitFlagSeenClear.store(true);
    return false;
  }
  return g_quitFlagSeenClear.load() && !g_quitFlagRecorded.exchange(true);
}

HWND GameWindow() noexcept
{
  auto* main = RE::Main::GetSingleton();
  return main ? reinterpret_cast<HWND>(main->wnd) : nullptr;
}

// The thread of a window of this process, or 0.
DWORD WindowThreadInThisProcess(HWND window) noexcept
{
  DWORD pid = 0;
  const DWORD thread = window ? GetWindowThreadProcessId(window, &pid) : 0u;
  return thread != 0u && pid == GetCurrentProcessId() ? thread : 0u;
}

// The game window as the heartbeat task last saw it. The hooks compare with
// this instead of reading Main: at shutdown Main can be gone while the window
// still gets messages, and a fault there would be recorded as our own crash.
HWND KnownGameWindow() noexcept
{
  return reinterpret_cast<HWND>(g_gameWindowValue.load(std::memory_order_relaxed));
}

// The game window or a child of it (a posted key goes to the focus window).
bool IsGameWindowOrChild(HWND hwnd) noexcept
{
  const HWND game = KnownGameWindow();
  return hwnd && game && (hwnd == game || GetAncestor(hwnd, GA_ROOT) == game);
}

const char* MessageName(UINT message) noexcept
{
  switch (message) {
    case WM_SYSKEYDOWN:
      return "WM_SYSKEYDOWN";
    case WM_SYSCOMMAND:
      return "WM_SYSCOMMAND SC_CLOSE";
    case WM_CLOSE:
      return "WM_CLOSE";
    case WM_QUIT:
      return "WM_QUIT";
    default:
      return "message";
  }
}

// how: why, for the SkyrimDiag.log line ("WM_SYSKEYDOWN posted",
// "quit flag, Alt+F4 held", "keys held, game foreground").
void Record(
  CloseRequestKind kind,
  UINT message,
  std::uint64_t wParam,
  CloseRequestSource source,
  std::string_view how) noexcept
{
  PushCloseRequestEvent(kind, message, wParam, static_cast<std::uint64_t>(source));
  // One SkyrimDiag.log line for the first one, so a test shows it was seen
  // without a crash.
  if (!g_firstRequestLogged.exchange(true)) {
    try {
      spdlog::info("SkyrimDiag: close request seen: {} ({})", skydiag::CloseRequestKindId(kind), how);
    } catch (...) {
      // Logging is optional; the blackbox event is already written.
      OutputDebugStringW(L"SkyrimDiag: could not log the first close request.\n");
    }
  }
}

void Observe(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, bool posted) noexcept
{
  // The fast path for every other message: four compares.
  if (message != WM_SYSKEYDOWN && message != WM_SYSCOMMAND && message != WM_CLOSE && message != WM_QUIT) {
    return;
  }
  if (message == WM_SYSKEYDOWN && wParam != VK_F4) {
    return;
  }
  if (message == WM_SYSCOMMAND && (wParam & 0xFFF0u) != SC_CLOSE) {
    return;
  }
  // Only on the thread whose hooks are current: a window thread the game
  // left keeps our hooks, and the tracker belongs to one thread.
  if (GetCurrentThreadId() != g_hookedThread.load(std::memory_order_relaxed)) {
    return;
  }
  // WM_QUIT belongs to the thread (no window), so it is the game's message
  // loop ending. A posted key goes to the focus window, which may be a child
  // of the game window. A close must be for the game window itself: another
  // plugin closing its own child window, or a dialog, is not the player
  // closing the game (DefWindowProc sends Alt+F4's SC_CLOSE to the root).
  if (message == WM_SYSKEYDOWN) {
    if (!IsGameWindowOrChild(hwnd)) {
      return;
    }
  } else if (message != WM_QUIT && (hwnd == nullptr || hwnd != KnownGameWindow())) {
    return;
  }
  // The input state of the message being handled.
  const bool altDown = (GetKeyState(VK_MENU) & 0x8000) != 0;
  const bool f4Down = (GetKeyState(VK_F4) & 0x8000) != 0;
  const std::uint64_t now = GetTickCount64();
  const auto kind = skydiag::ObserveCloseRequestMessage(
    &g_tracker,
    message,
    static_cast<std::uint64_t>(wParam),
    static_cast<std::uint64_t>(lParam),
    altDown,
    f4Down,
    now);
  if (kind == CloseRequestKind::kNone ||
      (kind == CloseRequestKind::kAltF4 && !TryClaimAltF4(now, skydiag::kCloseRequestRepeatMs))) {
    return;
  }
  try {
    const std::string how = std::string(MessageName(message)) + (posted ? " posted" : " sent");
    Record(
      kind,
      message,
      static_cast<std::uint64_t>(wParam),
      posted ? CloseRequestSource::kPostedMessage : CloseRequestSource::kSentMessage,
      how);
  } catch (...) {
    Record(
      kind,
      message,
      static_cast<std::uint64_t>(wParam),
      posted ? CloseRequestSource::kPostedMessage : CloseRequestSource::kSentMessage,
      MessageName(message));
  }
}

// Posted messages as the game's loop takes them from the queue. Only on
// PM_REMOVE: a PeekMessage(PM_NOREMOVE) shows the same message again later.
// The hook gets PeekMessage's flags, so PM_REMOVE | PM_NOYIELD arrives as 3.
// Looks only: the MSG is never changed, and the next hook always gets it.
LRESULT CALLBACK GetMessageHook(int code, WPARAM wParam, LPARAM lParam) noexcept
{
  if (code == HC_ACTION && (wParam & PM_REMOVE) != 0u && lParam != 0) {
    const auto& msg = *reinterpret_cast<const MSG*>(lParam);
    Observe(msg.hwnd, msg.message, msg.wParam, msg.lParam, true);
  }
  return CallNextHookEx(nullptr, code, wParam, lParam);
}

// Sent messages: the WM_CLOSE DefWindowProc sends for SC_CLOSE, and an
// SC_CLOSE sent by the X button. WH_CALLWNDPROC cannot change the message,
// and the next hook (and then the window procedure, whoever subclassed it)
// always gets it.
LRESULT CALLBACK CallWndProcHook(int code, WPARAM wParam, LPARAM lParam) noexcept
{
  if (code == HC_ACTION && lParam != 0) {
    const auto& sent = *reinterpret_cast<const CWPSTRUCT*>(lParam);
    Observe(sent.hwnd, sent.message, sent.wParam, sent.lParam, false);
  }
  return CallNextHookEx(nullptr, code, wParam, lParam);
}

// A hook belongs to the thread that sets it, and Windows removes it when
// that thread exits. kDataLoaded runs on the game's loader thread, which
// exits right after the main menu opens, so hooks set there were gone by the
// time anyone closed the game: no in-game test ever saw one fire (2026-10-29,
// the window thread had no WH_CALLWNDPROC hook in its TEB). The game window's
// own thread sets them instead, from the heartbeat task once it runs there,
// and they live as long as that thread. Thread hooks in this process: no DLL
// is injected anywhere, and the module is pinned
// (PinThisModuleForWorkerLifetime), so the hook procedures stay valid. They
// are never removed. Tried once per window thread; a refusal is logged, not
// retried. A window recreated on another thread gets them on that thread
// (the old thread's stay, and Observe ignores them).
void SetWindowHooksOnWindowThread(HWND window) noexcept
{
  const DWORD windowThread = WindowThreadInThisProcess(window);
  if (windowThread == 0u || windowThread == g_hookedThread.load(std::memory_order_relaxed)) {
    return;
  }
  const DWORD thisThread = GetCurrentThreadId();
  if (thisThread != windowThread) {
    // While loading, the heartbeat task runs on the loader thread.
    if (g_hooksWaitLoggedFor.exchange(windowThread) != windowThread) {
      try {
        spdlog::info(
          "SkyrimDiag: window hooks wait for the game window's thread {} (heartbeat task on thread {})",
          windowThread,
          thisThread);
      } catch (...) {
        OutputDebugStringW(L"SkyrimDiag: could not log the window hooks wait.\n");
      }
    }
    return;
  }
  g_hookedThread.store(thisThread);
  const HHOOK getMessageHook = SetWindowsHookExW(WH_GETMESSAGE, GetMessageHook, nullptr, thisThread);
  const DWORD getMessageError = getMessageHook ? 0u : GetLastError();
  const HHOOK callWndProcHook = SetWindowsHookExW(WH_CALLWNDPROC, CallWndProcHook, nullptr, thisThread);
  const DWORD callWndProcError = callWndProcHook ? 0u : GetLastError();
  try {
    if (getMessageHook && callWndProcHook) {
      spdlog::info("SkyrimDiag: window hooks on (game window thread {})", thisThread);
    } else {
      // The key sampler and the quit flag work without them.
      spdlog::warn(
        "SkyrimDiag: window hooks refused (game window thread {}: WH_GETMESSAGE err={}, WH_CALLWNDPROC err={})",
        thisThread,
        getMessageError,
        callWndProcError);
    }
  } catch (...) {
    OutputDebugStringW(L"SkyrimDiag: could not log the window hooks result.\n");
  }
}

// The quit flag's change, recorded from the heartbeat task or the key sampler
// (both may read the keys and log).
void RecordQuitFlagChange() noexcept
{
  if (!ClaimQuitFlagChange()) {
    return;
  }
  const std::uint64_t now = GetTickCount64();
  const std::uint32_t open = g_quitMenusOpen.load();
  const std::uint64_t closedMs = g_quitConfirmClosedMs.load();
  const auto decision = skydiag::QuitFlagCloseRequestKind(
    AltF4HeldNow() && GameIsForeground(), g_altF4SeenMs.load(), open != 0u, closedMs, now);
  // The same Alt+F4 the key sampler or the crash handler already recorded.
  if (decision.kind == CloseRequestKind::kAltF4 && !TryClaimAltF4(now, skydiag::kQuitFlagAltF4RecentMs)) {
    return;
  }
  std::string how;
  try {
    // The open one for kMenuOpen; the confirmation for kMenuClosed.
    std::uint32_t menu = kQuitConfirmMenu;
    for (std::uint32_t i = 0; i < std::size(kQuitMenus); ++i) {
      if ((open & (1u << i)) != 0u) {
        menu = i;
        break;
      }
    }
    const std::string menuName(kQuitMenus[menu]);
    switch (decision.reason) {
      case skydiag::QuitFlagReason::kAltF4Held:
        how = "quit flag, Alt+F4 held";
        break;
      case skydiag::QuitFlagReason::kMenuOpen:
        how = "quit flag, " + menuName + " open";
        break;
      case skydiag::QuitFlagReason::kMenuClosed:
        how = "quit flag, " + menuName + " closed " + std::to_string(now - closedMs) + " ms before";
        break;
      case skydiag::QuitFlagReason::kNoMenuNoKeys:
        how = "quit flag, no menu, keys not held";
        break;
    }
  } catch (...) {
    how.clear();
  }
  Record(decision.kind, 0u, static_cast<std::uint64_t>(decision.reason), CloseRequestSource::kQuitFlag, how);
}

}  // namespace

bool StartCloseRequestLog() noexcept
{
  if (g_installed.load()) {
    return true;
  }
  // The caller logs the error: 0 when Main had no window of this process,
  // not one left over from an unrelated call.
  SetLastError(ERROR_SUCCESS);
  const HWND window = GameWindow();
  if (WindowThreadInThisProcess(window) == 0u) {
    return false;
  }
  // No window hooks here: this is the loader thread, and they would go with
  // it (SetWindowHooksOnWindowThread). The key sampler and the quit flag
  // start now and work without them.
  g_gameWindowValue.store(reinterpret_cast<std::uintptr_t>(window));
  if (auto* main = RE::Main::GetSingleton()) {
    g_quitFlagAddress.store(reinterpret_cast<std::uintptr_t>(&main->GetRuntimeData().quitGame));
  }
  g_installed.store(true);
  return true;
}

bool CloseKeySamplingOn() noexcept
{
  return g_installed.load(std::memory_order_relaxed);
}

void SampleCloseKeys() noexcept
{
  if (!g_installed.load(std::memory_order_relaxed)) {
    return;
  }
  // The quit flag every sample too: a menu Quit can end the main loop
  // before the heartbeat task looks again.
  RecordQuitFlagChange();
  const bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
  const bool f4Down = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
  // The foreground window only when both keys are down.
  const bool foreground = altDown && f4Down && GameIsForeground();
  if (!skydiag::ObserveAltF4Keys(&g_keySampler, altDown, f4Down, foreground)) {
    if (foreground) {
      g_altF4SeenMs.store(GetTickCount64(), std::memory_order_relaxed);
    }
    return;
  }
  const std::uint64_t now = GetTickCount64();
  g_altF4SeenMs.store(now, std::memory_order_relaxed);
  // Recorded now, not left to the quit flag: the crash can come in the very
  // frame that handles Alt+F4 (2026-10-26 19:58), before the heartbeat task
  // ever sees the flag.
  if (TryClaimAltF4(now, skydiag::kCloseRequestRepeatMs)) {
    Record(CloseRequestKind::kAltF4, 0u, 0u, CloseRequestSource::kKeys, "keys held, game foreground");
  }
}

void RecordCloseRequestAtCrash() noexcept
{
  if (!g_installed.load(std::memory_order_relaxed)) {
    return;
  }
  // Allocation-free and lock-free on our side: three win32k key/window
  // reads, one ReadProcessMemory and blackbox writes, no logging. The keys
  // only on a thread that is already a GUI thread: on any other, the first
  // win32k call converts it, a callback into user32 that is not ours to run
  // in a crash. The input thread that crashed on Alt+F4 (2026-10-26 19:58,
  // tid 33300) was one.
  const bool altF4Held = IsGUIThread(FALSE) && AltF4HeldNow() && GameIsForeground();
  const std::uint64_t now = GetTickCount64();
  if (altF4Held) {
    g_altF4SeenMs.store(now, std::memory_order_relaxed);
    if (TryClaimAltF4(now, skydiag::kQuitFlagAltF4RecentMs)) {
      PushCloseRequestEvent(
        CloseRequestKind::kAltF4, 0u, 0u, static_cast<std::uint64_t>(CloseRequestSource::kKeysAtCrash));
    }
  }
  // The quit flag set since the pollers last looked: a menu Quit whose
  // shutdown crashed within one 25 ms sample.
  if (!ClaimQuitFlagChange()) {
    return;
  }
  const auto decision = skydiag::QuitFlagCloseRequestKind(
    altF4Held, g_altF4SeenMs.load(), g_quitMenusOpen.load() != 0u, g_quitConfirmClosedMs.load(), now);
  if (decision.kind == CloseRequestKind::kAltF4 && !TryClaimAltF4(now, skydiag::kQuitFlagAltF4RecentMs)) {
    return;
  }
  PushCloseRequestEvent(
    decision.kind,
    0u,
    static_cast<std::uint64_t>(decision.reason),
    static_cast<std::uint64_t>(CloseRequestSource::kQuitFlag));
}

void NoteMenuForQuit(std::string_view menuName, bool opening) noexcept
{
  for (std::uint32_t i = 0; i < std::size(kQuitMenus); ++i) {
    if (menuName != kQuitMenus[i]) {
      continue;
    }
    const std::uint32_t bit = 1u << i;
    if (opening) {
      g_quitMenusOpen.fetch_or(bit);
    } else if ((g_quitMenusOpen.fetch_and(~bit) & bit) != 0u && i == kQuitConfirmMenu) {
      g_quitConfirmClosedMs.store(GetTickCount64());
    }
    return;
  }
}

void PollGameQuitRequest() noexcept
{
  if (!g_installed.load()) {
    return;
  }
  auto* main = RE::Main::GetSingleton();
  if (!main) {
    return;
  }
  // A display mode change can recreate the game window; without this the
  // key sampler and the crash handler would compare with the old one.
  if (const auto window = reinterpret_cast<std::uintptr_t>(main->wnd); window != 0u) {
    g_gameWindowValue.store(window, std::memory_order_relaxed);
    SetWindowHooksOnWindowThread(reinterpret_cast<HWND>(window));
  }
  RecordQuitFlagChange();
}

}  // namespace skydiag::plugin
