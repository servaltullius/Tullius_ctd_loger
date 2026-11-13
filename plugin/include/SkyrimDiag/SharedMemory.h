#pragma once

#include <Windows.h>

#include <string>

#include "SkyrimDiagShared.h"

namespace skydiag::plugin {

bool InitSharedMemory();
void ShutdownSharedMemory();

skydiag::SharedLayout* GetShared() noexcept;
HANDLE GetCrashEvent() noexcept;
// ADR-0032: the event this plugin sets when an exception was not handled, and
// the name of the event the helper sets when the crash dump is written. The
// name is built once at init so the crash path only reads it.
HANDLE GetUnhandledEvent() noexcept;
const wchar_t* GetDumpSettledEventName() noexcept;
// The thread that loaded the plugin, the game main thread (SessionStart's tid).
std::uint32_t GetMainThreadId() noexcept;

std::wstring MakeKernelName(const wchar_t* suffix);

}  // namespace skydiag::plugin

