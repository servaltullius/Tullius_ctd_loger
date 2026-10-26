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

std::atomic_bool g_installed{ false };
HHOOK g_getMessageHook = nullptr;
HHOOK g_callWndProcHook = nullptr;
std::atomic_bool g_firstRequestLogged{ false };

// For the quit flag (QuitFlagCloseRequestKind). Written by the key sampler
// (heartbeat scheduler thread) and the menu sink (UI thread), read by the
// heartbeat task: atomics only.
std::atomic<std::uint64_t> g_altF4SeenMs{ 0 };
std::atomic<std::uint32_t> g_quitMenusOpen{ 0 };  // bit per kQuitMenus entry
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

bool AltF4HeldNow() noexcept
{
  return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 && (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
}

// Only the game window's thread runs the hooks, so it alone touches this.
skydiag::CloseRequestTracker g_tracker{};

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
      return "quit flag";
  }
}

// how: the log's reason for the quit flag (message 0).
void Record(
  skydiag::CloseRequestKind kind,
  UINT message,
  std::uint64_t wParam,
  bool posted,
  std::string_view how = {}) noexcept
{
  PushCloseRequestEvent(kind, message, wParam, posted);
  // One SkyrimDiag.log line for the first one, so a test shows it was seen
  // without a crash.
  if (!g_firstRequestLogged.exchange(true)) {
    try {
      if (message == 0u) {
        spdlog::info("SkyrimDiag: close request seen: {} (quit flag, {})", skydiag::CloseRequestKindId(kind), how);
      } else {
        spdlog::info(
          "SkyrimDiag: close request seen: {} ({} {})",
          skydiag::CloseRequestKindId(kind),
          MessageName(message),
          posted ? "posted" : "sent");
      }
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
  const auto kind = skydiag::ObserveCloseRequestMessage(
    &g_tracker,
    message,
    static_cast<std::uint64_t>(wParam),
    static_cast<std::uint64_t>(lParam),
    altDown,
    f4Down,
    GetTickCount64());
  if (kind != skydiag::CloseRequestKind::kNone) {
    Record(kind, message, static_cast<std::uint64_t>(wParam), posted);
  }
}

// Posted messages as the game's loop takes them from the queue: Alt+F4 is a
// posted WM_SYSKEYDOWN, and DefWindowProc posts the SC_CLOSE it becomes.
// Only on PM_REMOVE: a PeekMessage(PM_NOREMOVE) shows the same message again
// later. Looks only: the MSG is never changed, and the next hook always
// gets it.
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
  // stay valid for the process lifetime. They are never removed.
  if (!g_getMessageHook) {
    g_getMessageHook = SetWindowsHookExW(WH_GETMESSAGE, GetMessageHook, nullptr, threadId);
  }
  if (!g_callWndProcHook) {
    g_callWndProcHook = SetWindowsHookExW(WH_CALLWNDPROC, CallWndProcHook, nullptr, threadId);
  }
  if (!g_getMessageHook && !g_callWndProcHook) {
    return false;
  }
  g_installed.store(true);
  return true;
}

void SampleCloseKeys() noexcept
{
  if (g_installed.load(std::memory_order_relaxed) && AltF4HeldNow()) {
    g_altF4SeenMs.store(GetTickCount64(), std::memory_order_relaxed);
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
    AltF4HeldNow(), g_altF4SeenMs.load(), open != 0u, closedMs, now);
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
        how = "Alt+F4 held";
        break;
      case skydiag::QuitFlagReason::kMenuOpen:
        how = menuName + " open";
        break;
      case skydiag::QuitFlagReason::kMenuClosed:
        how = menuName + " closed " + std::to_string(now - closedMs) + " ms before";
        break;
      case skydiag::QuitFlagReason::kNoMenuNoKeys:
        how = "no menu, keys not held";
        break;
    }
  } catch (...) {
    how.clear();
  }
  Record(decision.kind, 0u, static_cast<std::uint64_t>(decision.reason), false, how);
}

}  // namespace skydiag::plugin
