// CommonLibSSE-NG must come before any Windows API header (REX/W32/BASE.h).
#include <RE/Skyrim.h>

#include "SkyrimDiag/CloseRequestHooks.h"

#include <Windows.h>

#include <atomic>

#include "SkyrimDiag/Blackbox.h"
#include "SkyrimDiagCloseRequest.h"

namespace skydiag::plugin {
namespace {

std::atomic_bool g_installed{ false };
HHOOK g_callWndProcHook = nullptr;

// Only the game window's thread runs the hook, so it alone touches this.
skydiag::CloseRequestTracker g_tracker{};

HWND GameWindow() noexcept
{
  auto* main = RE::Main::GetSingleton();
  return main ? reinterpret_cast<HWND>(main->wnd) : nullptr;
}

void ObserveSentMessage(const CWPSTRUCT& sent) noexcept
{
  // The fast path for every other message: two compares.
  if (sent.message != WM_SYSCOMMAND && sent.message != WM_CLOSE) {
    return;
  }
  if (sent.message == WM_SYSCOMMAND && (sent.wParam & 0xFFF0u) != SC_CLOSE) {
    return;
  }
  // The game window only, not a dialog or a helper window of another plugin
  // on the same thread.
  if (!sent.hwnd || sent.hwnd != GameWindow()) {
    return;
  }
  // The input state of the message being handled: Alt+F4 that DefWindowProc
  // turned into this SC_CLOSE / WM_CLOSE.
  const bool altF4Held = (GetKeyState(VK_MENU) & 0x8000) != 0 && (GetKeyState(VK_F4) & 0x8000) != 0;
  const auto kind = skydiag::ObserveCloseRequestMessage(
    &g_tracker,
    sent.message,
    static_cast<std::uint64_t>(sent.wParam),
    altF4Held,
    GetTickCount64());
  if (kind != skydiag::CloseRequestKind::kNone) {
    PushCloseRequestEvent(kind, sent.message, static_cast<std::uint64_t>(sent.wParam));
  }
}

// Looks only: WH_CALLWNDPROC cannot change the message, and the next hook
// (and then the window procedure, whoever subclassed it) always gets it.
LRESULT CALLBACK CallWndProcHook(int code, WPARAM wParam, LPARAM lParam) noexcept
{
  if (code == HC_ACTION && lParam != 0) {
    ObserveSentMessage(*reinterpret_cast<const CWPSTRUCT*>(lParam));
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
  // A thread hook in this process: no DLL is injected anywhere, and the
  // module is pinned (PinThisModuleForWorkerLifetime), so the hook procedure
  // stays valid for the process lifetime. It is never removed.
  g_callWndProcHook = SetWindowsHookExW(WH_CALLWNDPROC, CallWndProcHook, nullptr, threadId);
  if (!g_callWndProcHook) {
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
    PushCloseRequestEvent(skydiag::CloseRequestKind::kGameQuit, 0u, 0u);
  }
}

}  // namespace skydiag::plugin
