#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "Analyzer.h"
#include "ModalDialogWait.h"

namespace skydiag::dump_tool {

// Graphics drivers' user-mode DLLs and the D3D/DXGI runtimes. Drivers can load
// from DriverStore paths that look system-like, so they are matched by name.
bool IsGraphicsStackModule(std::wstring_view moduleFilename);
bool IsGraphicsDriverModule(std::wstring_view moduleFilename);

// Reads the top of the game main thread's stack (innermost first, the frames
// the modal-dialog check uses) and says what the thread was doing: waiting in
// the engine, in a plugin, in the graphics driver, or running code. Called for
// freeze captures that are not modal-dialog waits.
MainThreadWaitInfo ClassifyMainThreadWait(const std::vector<ModalStackFrame>& frames);

// The code before a Sleep call site (the caller frame's code_before, ending at
// the return address) polls a Direct3D 11 query: a call through vtable slot
// 0xE8 (ID3D11DeviceContext::GetData) shortly before the Sleep call. Field
// freezes on 1.6.1170 sat in exactly this loop (SkyrimSE.exe+0xe46e21).
bool LooksLikeGpuQueryPoll(const std::vector<std::uint8_t>& codeBefore);

// A wait that the modules further down the stack did not perform themselves:
// those modules are on the call path, not evidence of the cause.
bool IsBystanderWait(const MainThreadWaitInfo& wait);

// The main thread was waiting for the GPU: inside the graphics driver, or in
// the engine's Direct3D query poll.
bool IsGpuWait(const MainThreadWaitInfo& wait);

}  // namespace skydiag::dump_tool
