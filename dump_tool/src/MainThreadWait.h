#pragma once

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

// A wait that the modules further down the stack did not perform themselves:
// those modules are on the call path, not evidence of the cause.
bool IsBystanderWait(const MainThreadWaitInfo& wait);

}  // namespace skydiag::dump_tool
