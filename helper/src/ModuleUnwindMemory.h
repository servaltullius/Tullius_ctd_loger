#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "CrashObjectMemory.h"

namespace skydiag::helper::internal {

// Upper bound on what one dump gains from this (Windows modules come to about
// 17 MB on a large mod list).
inline constexpr std::size_t kModuleUnwindMemoryBudget = std::size_t{ 24 } * 1024u * 1024u;

// For each module loaded from the Windows directory (system DLLs and DriverStore
// graphics drivers), the ranges a stack walk and symbol lookup need: the PE
// headers, the export directory, the exception directory (.pdata) and the
// UNWIND_INFO entries it points at. A Windows or driver update replaces these
// files, after which the local copies no longer match a dump taken before it
// (ADR-0011). Crash, freeze and manual dumps all get them.
std::vector<ExtraDumpMemory> CollectModuleUnwindMemory(HANDLE process);

// The same ranges for one module (ADR-0023 adds them for the game and the
// modules the stacks point into). Returns the bytes added to `out`.
std::size_t AddModuleUnwindMemory(
  HANDLE process,
  std::uint64_t base,
  std::uint32_t imageSize,
  std::vector<ExtraDumpMemory>& out);

// The Windows directory, lower case with a trailing backslash ("c:\windows\");
// empty when it cannot be read. CollectModuleUnwindMemory covers the modules
// whose lower-case path starts with it.
std::wstring WindowsDirectoryPrefix();

// The UNWIND_INFO ranges for a .pdata table, given the image bytes that hold
// the unwind data starting at `spanRva`. Exposed for tests.
std::vector<ExtraDumpMemory> UnwindInfoRanges(
  std::uint64_t imageBase,
  const std::vector<RUNTIME_FUNCTION>& functions,
  std::uint32_t spanRva,
  const std::vector<std::uint8_t>& span);

}  // namespace skydiag::helper::internal
