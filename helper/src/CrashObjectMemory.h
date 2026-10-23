#pragma once

#include <Windows.h>

#include <chrono>
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
inline constexpr std::size_t kCrashObjectMemoryBudget = std::size_t{2} * 1024u * 1024u;

// Reads, in the live game process, the objects that the crash registers and
// the top of the crash stack point at (shared/SkyrimDiagRtti.h) and returns
// every byte range that took, merged. Adding these ranges to the crash dump
// lets the analyzer read the same objects back (ADR-0010). The game's heap is
// otherwise not in the dump.
std::vector<ExtraDumpMemory> CollectCrashObjectMemory(HANDLE process, const CONTEXT& context);

// The same for one thread of a game that keeps running afterwards (a freeze
// or manual capture): only committed, readable pages are touched (no guard,
// no-access, or executable pages outside a module image), at most
// budgetBytes are read, and no new value is examined after `deadline`.
std::vector<ExtraDumpMemory> CollectThreadObjectMemory(
  HANDLE process,
  const CONTEXT& context,
  std::size_t budgetBytes,
  std::chrono::steady_clock::time_point deadline);

// Committed, readable and never a guard page; an executable page only inside
// a module image (the rule CollectThreadObjectMemory reads by).
bool IsReadableRegion(const MEMORY_BASIC_INFORMATION& info);

// Sorts and merges overlapping or touching ranges.
std::vector<ExtraDumpMemory> MergeExtraDumpMemory(std::vector<ExtraDumpMemory> ranges);

}  // namespace skydiag::helper::internal
