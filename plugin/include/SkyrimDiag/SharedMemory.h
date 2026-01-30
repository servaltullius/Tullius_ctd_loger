#pragma once

#include <Windows.h>

#include <string>

#include "SkyrimDiagShared.h"

namespace skydiag::plugin {

bool InitSharedMemory();
void ShutdownSharedMemory();

skydiag::SharedLayout* GetShared() noexcept;
HANDLE GetCrashEvent() noexcept;

std::wstring MakeKernelName(const wchar_t* suffix);

}  // namespace skydiag::plugin

