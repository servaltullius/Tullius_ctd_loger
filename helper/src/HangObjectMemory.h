#pragma once

#include <Windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
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

// ADR-0021: the engine-lock windows (shared/SkyrimDiagGameLocks.h) get at most
// this long, before the objects; their bytes count against the same budget.
inline constexpr std::chrono::milliseconds kGameLockTimeBudget{ 20 };

// The game executable's image range (the process's first module).
bool GameImageRange(HANDLE process, std::uint64_t& base, std::uint64_t& end);

// Threads of the process whose start address is in the game executable (the
// main thread and the threads the game started).
std::vector<std::uint32_t> GameThreadIds(HANDLE process);

// The threads SelectThreadIds picks for this process; the game-started ones
// are found from their start addresses in the live process.
std::vector<DWORD> SelectHangObjectThreads(HANDLE process, std::optional<DWORD> mainTid, std::string_view wctJsonUtf8);
std::vector<DWORD> SelectHangObjectThreads(
  std::optional<DWORD> mainTid,
  std::string_view wctJsonUtf8,
  const std::vector<std::uint32_t>& gameThreads);

// ntdll!NtDelayExecution in the game: ntdll sits at the same address in every
// process of a boot, and the game's stub bytes are compared with this
// process's before the address is used. 0 when they differ or cannot be read.
std::uint64_t GameDelayStubAddress(HANDLE process);

// The engine-lock windows for the game threads waiting in a delay (their
// dump contexts): the image header, then aligned windows of the image's
// writable data around their non-volatile registers and stack top. Only
// committed, readable pages; nothing is read after `deadline`.
std::vector<ExtraDumpMemory> CollectGameLockMemory(
  HANDLE process,
  const std::vector<CONTEXT>& delayThreads,
  std::chrono::steady_clock::time_point deadline,
  DumpObjectMemoryStats& stats);

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

  // ADR-0021: also keep the engine-lock windows of these game threads when
  // the writer reports them waiting in the delay stub (0: no lock windows).
  void EnableGameLocks(std::vector<std::uint32_t> gameThreads, std::uint64_t delayStub);

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
  std::vector<std::uint32_t> lockThreads_;
  std::uint64_t delayStub_ = 0;
  std::vector<std::pair<DWORD, CONTEXT>> lockContexts_;
  std::vector<ExtraDumpMemory> ranges_;
  DumpObjectMemoryStats stats_{};
};

}  // namespace skydiag::helper::internal
