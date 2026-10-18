#pragma once

#include <Windows.h>

#include <string>

#include "SkyrimDiagShared.h"

namespace skydiag::plugin {

bool InitSharedMemory();
void ShutdownSharedMemory();

skydiag::SharedLayout* GetShared() noexcept;
HANDLE GetCrashEvent() noexcept;
// The thread that loaded the plugin, the game main thread (SessionStart's tid).
std::uint32_t GetMainThreadId() noexcept;

std::wstring MakeKernelName(const wchar_t* suffix);

}  // namespace skydiag::plugin

