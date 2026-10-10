#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace skydiag::helper::internal {

struct ExtraDumpMemory
{
  std::uint64_t base = 0;
  std::uint32_t size = 0;
};

// Stack bytes above RSP whose qwords are examined, as Crash Logger's
// "possible relevant objects" do for registers and the stack.
inline constexpr std::size_t kCrashObjectStackBytes = 0x800;
// Upper bound on what one crash dump gains from this.
inline constexpr std::size_t kCrashObjectMemoryBudget = 2u * 1024u * 1024u;

// Reads, in the live game process, the objects that the crash registers and
// the top of the crash stack point at (shared/SkyrimDiagRtti.h) and returns
// every byte range that took, merged. Adding these ranges to the crash dump
// lets the analyzer read the same objects back (ADR-0010). The game's heap is
// otherwise not in the dump.
std::vector<ExtraDumpMemory> CollectCrashObjectMemory(HANDLE process, const CONTEXT& context);

// Sorts and merges overlapping or touching ranges.
std::vector<ExtraDumpMemory> MergeExtraDumpMemory(std::vector<ExtraDumpMemory> ranges);

}  // namespace skydiag::helper::internal
