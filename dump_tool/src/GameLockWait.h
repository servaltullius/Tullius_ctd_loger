#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Analyzer.h"
#include "ModalDialogWait.h"
#include "SkyrimDiagGameLocks.h"

namespace skydiag::dump_tool {

// ADR-0021: what an engine-lock wait reads from the dump for one game thread.
struct GameLockThreadInput
{
  std::uint32_t tid = 0;
  bool is_main_thread = false;
  // Its top frames, innermost first (the wait is told from these, as for the
  // main thread: ClassifyMainThreadWait).
  std::vector<ModalStackFrame> top_frames;
  // Every walked frame as module + offset, innermost first (empty module for
  // an address outside every module).
  std::vector<GameFrameRef> frames;
  // Its non-volatile registers ("RBX") and stack slots ("RSP+28"), in the
  // helper's order (shared/SkyrimDiagGameLocks.h).
  std::vector<std::pair<std::wstring, std::uint64_t>> roots;
};

struct GameLockImage
{
  std::wstring module;  // "SkyrimSE.exe"
  std::uint64_t base = 0;
  // Writable data of the image, from the header the dump holds; empty means
  // no lock can be read.
  std::vector<skydiag::game_locks::AddressRange> writable;
};

// Reads bytes the dump itself holds (never a local file: the data must be
// what the game had at the capture).
using DumpMemoryReader = std::function<bool(std::uint64_t address, void* destination, std::size_t size)>;

// Frames kept per thread for the signatures' anchors.
inline constexpr std::size_t kGameLockFrames = 32;
// Candidate locks kept per thread.
inline constexpr std::size_t kGameLockMaxCandidates = 16;

// The game threads waiting in a delay (a Sleep under a game-executable frame,
// which is how the engine's spin and read/write locks wait), each with the
// held locks its registers and stack top point at in the game's writable
// data: a BSSpinLock or a BSReadWriteLock held for write by another thread of
// the dump. owner_waiting is set when that owner is one of these threads.
std::vector<GameLockWaitThread> FindGameLockWaits(
  const std::vector<GameLockThreadInput>& threads,
  const GameLockImage& image,
  const DumpMemoryReader& read,
  const std::vector<std::uint32_t>& dumpThreadIds);

// A wait-for cycle among those threads: each waits on a lock (spin, or
// read/write held for write) that the next one holds, the last on one the
// first holds. Only owners that are themselves waiting count, so a single
// waiter, an owner that is still running and a stale pointer to a free lock
// close no cycle. The first cycle in thread order; empty when there is none.
std::vector<EngineLockDeadlockInfo::Link> FindEngineLockCycle(const std::vector<GameLockWaitThread>& waits);

// "SkyrimSE.exe+0x132c5a".
std::wstring GameFrameText(const GameFrameRef& frame);

// The deadlocked threads: the cycle's, else those the known pattern matched.
std::vector<std::uint32_t> EngineLockDeadlockThreads(const EngineLockDeadlockInfo& deadlock);

// A wait the report and the summary JSON list: one with a candidate lock, or
// one of the deadlocked threads. Dumps without the game's lock memory list
// none.
bool IsReportedGameLockWait(const GameLockWaitThread& wait, const EngineLockDeadlockInfo& deadlock);

// "5096, 18456" (language-neutral).
std::wstring JoinThreadIds(const std::vector<std::uint32_t>& tids);

// "spin lock" / "스핀 잠금"; "read/write lock held for write" / "쓰기로 잡힌 읽기/쓰기 잠금".
std::wstring DescribeEngineLockKind(const std::string& kind, bool en);

// The cycle in words: "thread 5096 waited for SkyrimSE.exe+0x2f3b8e8 (spin
// lock), held by thread 18456; thread 18456 waited for ..., held by thread 5096".
std::wstring DescribeEngineLockCycle(const EngineLockDeadlockInfo& deadlock, bool en);

}  // namespace skydiag::dump_tool
