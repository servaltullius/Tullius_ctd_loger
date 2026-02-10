#pragma once

#include <Windows.h>

namespace skydiag::helper::internal {

bool IsPidInForeground(DWORD pid);
HWND FindMainWindowForPid(DWORD pid);
bool IsWindowResponsive(HWND hwnd, UINT timeoutMs);

}  // namespace skydiag::helper::internal

