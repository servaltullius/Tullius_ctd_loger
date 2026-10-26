// CommonLibSSE-NG must come before any Windows API header (REX/W32/BASE.h).
#include <RE/Skyrim.h>

#include "SkyrimDiag/CloseRequestHooks.h"

#include <Windows.h>

#include <atomic>

#include <spdlog/spdlog.h>

#include "SkyrimDiag/Blackbox.h"
#include "SkyrimDiagCloseRequest.h"

namespace skydiag::plugin {
namespace {

std::atomic_bool g_installed{ false };
HHOOK g_getMessageHook = nullptr;
HHOOK g_callWndProcHook = nullptr;
std::atomic_bool g_firstRequestLogged{ false };

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

void Record(skydiag::CloseRequestKind kind, UINT message, std::uint64_t wParam, bool posted) noexcept
{
  PushCloseRequestEvent(kind, message, wParam, posted);
  // One SkyrimDiag.log line for the first one, so a test shows it was seen
  // without a crash.
  if (!g_firstRequestLogged.exchange(true)) {
    try {
      spdlog::info(
        "SkyrimDiag: close request seen: {} ({} {})",
        skydiag::CloseRequestKindId(kind),
        MessageName(message),
        message == 0u ? "polled" : (posted ? "posted" : "sent"));
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
  if (seenClear) {
    recorded = true;
    Record(skydiag::CloseRequestKind::kGameQuit, 0u, 0u, false);
  }
}

}  // namespace skydiag::plugin
