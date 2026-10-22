#pragma once

#include <Windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "CrashObjectMemory.h"
#include "SkyrimDiagHelper/DumpWriter.h"

namespace skydiag::helper::internal {

// Freeze and manual dumps (ADR-0010, addendum 2026-10-22): the objects that
// the registers and stack top of the threads that matter point at
// (shared/SkyrimDiagHangObjectThreads.h). The game keeps running after a
// manual capture, so reads are bounded in bytes and time and touch only
// committed, readable pages (CollectThreadObjectMemory).
inline constexpr std::size_t kHangObjectMemoryBudget = std::size_t{4} * 1024u * 1024u;
// No single thread may take more than this of it.
inline constexpr std::size_t kHangObjectThreadBudget = std::size_t{1} * 1024u * 1024u;
inline constexpr std::chrono::milliseconds kHangObjectTimeBudget{ 100 };

// The threads SelectThreadIds picks for this process; the game-started ones
// are found from their start addresses in the live process.
std::vector<DWORD> SelectHangObjectThreads(HANDLE process, std::optional<DWORD> mainTid, std::string_view wctJsonUtf8);

// Fed by the dump writer's thread callbacks with the contexts it captured, so
// the objects match the registers the dump holds; read once, in the order of
// the selection, when the writer first asks for extra memory.
class HangObjectMemoryCollector
{
public:
  HangObjectMemoryCollector(
    HANDLE process,
    std::vector<DWORD> tids,
    std::size_t budgetBytes = kHangObjectMemoryBudget,
    std::chrono::milliseconds timeBudget = kHangObjectTimeBudget);

  // A thread that was not selected is ignored.
  void OnThreadContext(DWORD tid, const CONTEXT& context);
  // Reads the objects (the first call only) and returns their ranges, merged.
  const std::vector<ExtraDumpMemory>& Collect();
  const DumpObjectMemoryStats& Stats() const { return stats_; }

private:
  HANDLE process_;
  std::vector<DWORD> tids_;
  std::vector<std::optional<CONTEXT>> contexts_;
  std::size_t budgetBytes_;
  std::chrono::milliseconds timeBudget_;
  bool collected_ = false;
  std::vector<ExtraDumpMemory> ranges_;
  DumpObjectMemoryStats stats_{};
};

}  // namespace skydiag::helper::internal
