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
HHOOK g_getMessageHook = nullptr;
HHOOK g_callWndProcHook = nullptr;
std::atomic_bool g_firstRequestLogged{ false };
// The game window, captured at install: the key sampler and the crash
// handler compare the foreground window with it without touching Main.
std::atomic<std::uintptr_t> g_gameWindowValue{ 0 };

// Written by the key sampler (heartbeat scheduler thread), the menu sink (UI
// thread), the hooks (game window thread) and the crash handler, read by the
// heartbeat task: atomics only.
std::atomic<std::uint64_t> g_altF4SeenMs{ 0 };      // Alt+F4 last seen held with the game in front
std::atomic<std::uint64_t> g_altF4RecordedMs{ 0 };  // an alt_f4 request last recorded (any source)
std::atomic<std::uint32_t> g_quitMenusOpen{ 0 };     // bit per kQuitMenus entry
std::atomic<std::uint64_t> g_quitMenuClosedMs{ 0 };
std::atomic<std::uint32_t> g_lastQuitMenu{ 0 };

// Menus one quits from: the Journal's system tab and its confirmation, the
// main menu, the console (qqq).
constexpr std::string_view kQuitMenus[] = {
  RE::JournalMenu::MENU_NAME,
  RE::MessageBoxMenu::MENU_NAME,
  RE::MainMenu::MENU_NAME,
  RE::Console::MENU_NAME,
};

// Only the game window's thread runs the hooks, so it alone touches this.
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

HWND GameWindow() noexcept
{
  auto* main = RE::Main::GetSingleton();
  return main ? reinterpret_cast<HWND>(main->wnd) : nullptr;
}

// The game window or a child of it (a posted key goes to the focus window).
bool IsGameWindowOrChild(HWND hwnd) noexcept
{
  const HWND game = GameWindow();
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
  // WM_QUIT belongs to the thread (no window); the hooks run only on the
  // game window's thread, so it is the game's message loop ending. The rest
  // must be for the game window, not a dialog or another plugin's window.
  if (message != WM_QUIT && !IsGameWindowOrChild(hwnd)) {
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
// Looks only: the MSG is never changed, and the next hook always gets it.
LRESULT CALLBACK GetMessageHook(int code, WPARAM wParam, LPARAM lParam) noexcept
{
  if (code == HC_ACTION && wParam == PM_REMOVE && lParam != 0) {
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

}  // namespace

bool InstallCloseRequestHooks() noexcept
{
  if (g_installed.load()) {
    return true;
  }
  const HWND window = GameWindow();
  DWORD pid = 0;
  const DWORD threadId = window ? GetWindowThreadProcessId(window, &pid) : 0u;
  if (threadId == 0u || pid != GetCurrentProcessId()) {
    return false;
  }
  // Thread hooks in this process: no DLL is injected anywhere, and the
  // module is pinned (PinThisModuleForWorkerLifetime), so the hook procedures
  // stay valid for the process lifetime. They are never removed. Skyrim
  // reads the keyboard through DirectInput, so they do not see Alt+F4 (the
  // key sampler and the quit flag do); they see the X button and close
  // requests from other programs.
  if (!g_getMessageHook) {
    g_getMessageHook = SetWindowsHookExW(WH_GETMESSAGE, GetMessageHook, nullptr, threadId);
  }
  if (!g_callWndProcHook) {
    g_callWndProcHook = SetWindowsHookExW(WH_CALLWNDPROC, CallWndProcHook, nullptr, threadId);
  }
  // The key sampler and the quit flag work without the hooks.
  g_gameWindowValue.store(reinterpret_cast<std::uintptr_t>(window));
  g_installed.store(true);
  return true;
}

void SampleCloseKeys() noexcept
{
  if (!g_installed.load(std::memory_order_relaxed)) {
    return;
  }
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

void RecordCloseKeysAtCrash() noexcept
{
  // Allocation-free and lock-free on our side: three win32k key/window
  // reads and one blackbox write, no logging.
  if (!g_installed.load(std::memory_order_relaxed) || !AltF4HeldNow() || !GameIsForeground()) {
    return;
  }
  const std::uint64_t now = GetTickCount64();
  g_altF4SeenMs.store(now, std::memory_order_relaxed);
  if (TryClaimAltF4(now, skydiag::kQuitFlagAltF4RecentMs)) {
    PushCloseRequestEvent(CloseRequestKind::kAltF4, 0u, 0u, static_cast<std::uint64_t>(CloseRequestSource::kKeysAtCrash));
  }
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
    } else if ((g_quitMenusOpen.fetch_and(~bit) & bit) != 0u) {
      g_quitMenuClosedMs.store(GetTickCount64());
    }
    g_lastQuitMenu.store(i);
    return;
  }
}

void PollGameQuitRequest() noexcept
{
  if (!g_installed.load()) {
    return;
  }
  // Recorded once, on the change from clear to set.
  static bool seenClear = false;
  static bool recorded = false;
  if (recorded) {
    return;
  }
  auto* main = RE::Main::GetSingleton();
  if (!main) {
    return;
  }
  if (!main->GetRuntimeData().quitGame) {
    seenClear = true;
    return;
  }
  if (!seenClear) {
    return;
  }
  recorded = true;
  const std::uint64_t now = GetTickCount64();
  const std::uint32_t open = g_quitMenusOpen.load();
  const std::uint64_t closedMs = g_quitMenuClosedMs.load();
  const auto decision = skydiag::QuitFlagCloseRequestKind(
    AltF4HeldNow() && GameIsForeground(), g_altF4SeenMs.load(), open != 0u, closedMs, now);
  // The same Alt+F4 the key sampler or the crash handler already recorded.
  if (decision.kind == CloseRequestKind::kAltF4 && !TryClaimAltF4(now, skydiag::kQuitFlagAltF4RecentMs)) {
    return;
  }
  std::string how;
  try {
    std::uint32_t menu = g_lastQuitMenu.load();
    for (std::uint32_t i = 0; i < std::size(kQuitMenus); ++i) {
      if ((open & (1u << i)) != 0u) {
        menu = i;
        break;
      }
    }
    const std::string menuName(kQuitMenus[menu < std::size(kQuitMenus) ? menu : 0u]);
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

}  // namespace skydiag::plugin
