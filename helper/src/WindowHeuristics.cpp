#include "WindowHeuristics.h"

namespace skydiag::helper::internal {
namespace {

struct FindWindowCtx
{
  DWORD pid = 0;
  HWND hwnd = nullptr;
};

BOOL CALLBACK EnumWindows_FindTopLevelForPid(HWND hwnd, LPARAM lParam)
{
  auto* ctx = reinterpret_cast<FindWindowCtx*>(lParam);
  if (!ctx) {
    return TRUE;
  }

  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != ctx->pid) {
    return TRUE;
  }

  // Prefer visible, unowned top-level windows as the "main" window.
  if (!IsWindowVisible(hwnd)) {
    return TRUE;
  }
  if (GetWindow(hwnd, GW_OWNER) != nullptr) {
    return TRUE;
  }

  ctx->hwnd = hwnd;
  return FALSE;  // stop
}

}  // namespace

bool IsPidInForeground(DWORD pid)
{
  const HWND fg = GetForegroundWindow();
  if (!fg) {
    return false;
  }

  HWND root = GetAncestor(fg, GA_ROOTOWNER);
  if (!root) {
    root = fg;
  }

  DWORD fgPid = 0;
  GetWindowThreadProcessId(root, &fgPid);
  return fgPid == pid;
}

HWND FindMainWindowForPid(DWORD pid)
{
  FindWindowCtx ctx{};
  ctx.pid = pid;
  EnumWindows(EnumWindows_FindTopLevelForPid, reinterpret_cast<LPARAM>(&ctx));
  return ctx.hwnd;
}

bool IsWindowResponsive(HWND hwnd, UINT timeoutMs)
{
  if (!hwnd || !IsWindow(hwnd)) {
    return false;
  }

  DWORD_PTR result = 0;
  SetLastError(ERROR_SUCCESS);
  const LRESULT ok = SendMessageTimeoutW(
    hwnd,
    WM_NULL,
    0,
    0,
    SMTO_ABORTIFHUNG | SMTO_BLOCK,
    timeoutMs,
    &result);
  return ok != 0;
}

}  // namespace skydiag::helper::internal

