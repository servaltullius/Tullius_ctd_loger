#pragma once

#include <cstdint>

namespace skydiag::plugin {

// CrashHookMode:
//   0 = Off
//   1 = Fatal exceptions only (recommended; avoids many false positives)
//   2 = All exceptions (can false-trigger on handled exceptions)
bool InstallCrashHandler(std::uint32_t crashHookMode);

}  // namespace skydiag::plugin
