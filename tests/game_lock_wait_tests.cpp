// ADR-0021: engine-lock deadlocks. The helper's choice of game-data windows
// and the lock decoding (shared/SkyrimDiagGameLocks.h), and the analyzer's
// waits and wait-for cycle (GameLockWait.h), on hand-built memory shaped like
// the published 1.5.97 captures (FreezeLogger case study, appendix A).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "GameLockWait.h"
#include "SkyrimDiagGameLocks.h"

namespace {

namespace locks = skydiag::game_locks;
using skydiag::dump_tool::EngineLockDeadlockInfo;
using skydiag::dump_tool::FindEngineLockCycle;
using skydiag::dump_tool::FindGameLockWaits;
using skydiag::dump_tool::GameFrameRef;
using skydiag::dump_tool::GameLockImage;
using skydiag::dump_tool::GameLockThreadInput;
using skydiag::dump_tool::GameLockWaitThread;
using skydiag::dump_tool::ModalStackFrame;

void Require(bool condition, const char* message)
{
  if (!condition) {
    std::fprintf(stderr, "FAILED: %s\n", message);
    throw std::runtime_error(message);
  }
}

constexpr std::uint64_t kBase = 0x140000000ull;
constexpr std::uint64_t kDataBegin = kBase + 0x1E00000ull;
constexpr std::uint64_t kDataEnd = kBase + 0x3100000ull;
constexpr std::uint64_t kLockA = kBase + 0x2EFF8E0ull;  // spin lock
constexpr std::uint64_t kLockB = kBase + 0x2F3B8E8ull;  // spin lock
constexpr std::uint64_t kRwLock = kBase + 0x2F3B8F0ull;

// A PE header with .text (code), .rdata (read-only), .data (writable) and a
// writable executable section that must not count.
std::vector<std::uint8_t> PeHeader()
{
  std::vector<std::uint8_t> header(locks::kHeaderBytes, 0);
  header[0] = 'M';
  header[1] = 'Z';
  const std::uint32_t nt = 0x100;
  std::memcpy(header.data() + 0x3C, &nt, 4);
  std::memcpy(header.data() + nt, "PE\0\0", 4);
  const std::uint16_t count = 4;
  const std::uint16_t optionalSize = 0xF0;
  std::memcpy(header.data() + nt + 6, &count, 2);
  std::memcpy(header.data() + nt + 20, &optionalSize, 2);
  struct Section
  {
    std::uint32_t va;
    std::uint32_t size;
    std::uint32_t characteristics;
  };
  const Section sections[] = {
    { 0x1000, 0x174D938, 0x60000020 },              // .text: code, execute, read
    { 0x1750000, 0x6A0000, 0x40000040 },            // .rdata: read
    { 0x1E00000, 0x12FF800, 0xC0000040 },           // .data: read, write (rounded up to a page)
    { 0x3200000, 0x1000, 0xE0000020 },              // write + execute: not data
  };
  std::size_t at = nt + 24 + optionalSize;
  for (const auto& s : sections) {
    std::memcpy(header.data() + at + 8, &s.size, 4);
    std::memcpy(header.data() + at + 12, &s.va, 4);
    std::memcpy(header.data() + at + 36, &s.characteristics, 4);
    at += 40;
  }
  return header;
}

void TestWritableSections()
{
  const auto header = PeHeader();
  const auto writable = locks::WritableSections(header.data(), header.size(), kBase);
  Require(writable.size() == 1u, "Only the writable, non-executable section");
  Require(writable[0].begin == kDataBegin && writable[0].end == kDataEnd, "Rounded up to whole pages");
  Require(locks::PointsIntoWritable(writable, kLockB), "A lock in .data");
  Require(!locks::PointsIntoWritable(writable, kBase + 0x132C5A), "Code is not data");
  Require(!locks::PointsIntoWritable(writable, kLockB + 1), "Unaligned");
  Require(!locks::PointsIntoWritable(writable, kDataEnd - 4), "All 8 bytes inside");
  Require(locks::PointsIntoWritable(writable, kDataEnd - 8), "The last lock of the section");

  auto broken = header;
  broken[0] = 'X';
  Require(locks::WritableSections(broken.data(), broken.size(), kBase).empty(), "Not a PE header");
  auto truncated = header;
  const std::uint32_t far = 0x2000;
  std::memcpy(truncated.data() + 0x3C, &far, 4);
  Require(locks::WritableSections(truncated.data(), truncated.size(), kBase).empty(), "NT header out of range");
  Require(locks::WritableSections(nullptr, 0, kBase).empty(), "No bytes");
}

void TestSelectWindows()
{
  const std::vector<locks::AddressRange> writable = { { kDataBegin, kDataEnd } };
  locks::WindowStats stats{};
  // A spin lock and the read/write lock next to it share a window; a value
  // in code, a heap pointer and a number are skipped; the same window is not
  // taken twice; a value whose 8 bytes cross into the next window takes both.
  const std::vector<std::vector<std::uint64_t>> values = {
    { kLockB, kBase + 0x132C5A, 0x25C4B8BA838ull, 3u, kRwLock },
    { kLockB, kDataBegin + 0x3C },
  };
  const auto windows = locks::SelectWindows(writable, values, &stats);
  Require(windows.size() == 3u, "Three windows");
  Require(windows[0].base == (kLockB & ~0x3Full) && windows[0].size == 0x40, "Aligned 64-byte window");
  Require(windows[1].base == kDataBegin && windows[2].base == kDataBegin + 0x40, "A lock across two windows");
  Require(stats.pointers == 4u && stats.windows == 3u && stats.bytes == 0xC0u && !stats.budget_reached,
    "Pointers counted, bytes counted");

  // Per-thread and total caps.
  std::vector<std::uint64_t> many;
  for (std::uint64_t i = 0; i < 100; ++i) {
    many.push_back(kDataBegin + i * 0x100);
  }
  const auto capped = locks::SelectWindows(writable, { many }, &stats);
  Require(capped.size() == locks::kMaxWindowsPerThread && stats.budget_reached, "Per-thread cap");
  std::vector<std::vector<std::uint64_t>> threads;
  for (std::uint64_t t = 0; t < 8; ++t) {
    std::vector<std::uint64_t> v;
    for (std::uint64_t i = 0; i < 32; ++i) {
      v.push_back(kDataBegin + (t * 32 + i) * 0x100);
    }
    threads.push_back(std::move(v));
  }
  const auto total = locks::SelectWindows(writable, threads, &stats);
  Require(total.size() == locks::kMaxWindows && stats.budget_reached, "Total cap");
  Require(stats.bytes <= locks::kMaxBytes, "Byte cap");
  Require(locks::SelectWindows({}, values, &stats).empty() && stats.pointers == 0u, "No writable data, nothing");
}

void TestDecodeLocks()
{
  auto held = locks::DecodeHeldLock(18456u, 1u);
  Require(held.kind == locks::HeldKind::kSpin && held.owner == 18456u && held.count == 1u, "A held spin lock");
  held = locks::DecodeHeldLock(5096u, 0x80000001u);
  Require(held.kind == locks::HeldKind::kRwWrite && held.owner == 5096u && held.count == 1u, "Held for write");
  Require(locks::DecodeHeldLock(0u, 0u).kind == locks::HeldKind::kNone, "Free");
  Require(locks::DecodeHeldLock(256u, 0u).kind == locks::HeldKind::kNone, "A stale owner with no count");
  Require(locks::DecodeHeldLock(0u, 1u).kind == locks::HeldKind::kNone, "Read-held names no owner");
  Require(locks::DecodeHeldLock(7u, 0x90000001u).kind == locks::HeldKind::kNone, "Unknown flag bits");
  Require(locks::DecodeHeldLock(7u, 0x12345678u).kind == locks::HeldKind::kNone, "A count no lock has");
  Require(locks::RwReaders(0u, 2u) == 2u, "Two readers");
  Require(!locks::RwReaders(0u, 0u) && !locks::RwReaders(9u, 1u) && !locks::RwReaders(0u, 0x80000001u),
    "Free, a writer, or held for write");
  Require(locks::InDelayStub(0x7FF82DFC1D24ull, 0x7FF82DFC1D10ull), "Just after the syscall");
  Require(!locks::InDelayStub(0x7FF82DFC1D40ull, 0x7FF82DFC1D10ull) && !locks::InDelayStub(5, 0), "Elsewhere");
}

ModalStackFrame System(std::wstring module, std::wstring symbol, std::uint64_t displacement)
{
  ModalStackFrame frame{};
  frame.has_module = true;
  frame.module_filename = std::move(module);
  frame.symbol = std::move(symbol);
  frame.displacement = displacement;
  frame.is_system = true;
  return frame;
}

ModalStackFrame Exe()
{
  ModalStackFrame frame{};
  frame.has_module = true;
  frame.module_filename = L"SkyrimSE.exe";
  frame.is_game_exe = true;
  return frame;
}

// A game thread sleeping in the engine's lock wait: NtDelayExecution,
// RtlDelayExecution, SleepEx, then game frames (offsets given). With a
// caller below the lock function, a last stack slot holds the return address
// into it (the end of the lock function's frame), after the given roots.
GameLockThreadInput Sleeper(
  std::uint32_t tid,
  std::vector<std::uint64_t> exeFrames,
  std::vector<std::pair<std::wstring, std::uint64_t>> roots)
{
  GameLockThreadInput input{};
  input.tid = tid;
  input.top_frames = { System(L"ntdll.dll", L"NtDelayExecution", 0x14),
    System(L"ntdll.dll", L"RtlDelayExecution", 0x34), System(L"KERNELBASE.dll", L"SleepEx", 0x91) };
  input.frames = { { L"ntdll.dll", 0x9D1D24 }, { L"ntdll.dll", 0x9A1234 }, { L"KERNELBASE.dll", 0x2E40F } };
  for (const auto offset : exeFrames) {
    auto frame = Exe();
    frame.pc = kBase + offset;
    input.top_frames.push_back(frame);
    input.frames.push_back(GameFrameRef{ L"SkyrimSE.exe", offset });
  }
  input.roots = std::move(roots);
  if (exeFrames.size() >= 2u) {
    input.roots.emplace_back(L"RSP+3F8", kBase + exeFrames[1]);
  }
  return input;
}

struct Memory
{
  std::map<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>> words;
  bool operator()(std::uint64_t address, void* destination, std::size_t size) const
  {
    const auto it = words.find(address);
    if (it == words.end() || size != 8) {
      return false;
    }
    const std::uint32_t pair[2] = { it->second.first, it->second.second };
    std::memcpy(destination, pair, 8);
    return true;
  }
};

GameLockImage Image()
{
  GameLockImage image{};
  image.module = L"SkyrimSE.exe";
  image.base = kBase;
  image.writable = { { kDataBegin, kDataEnd } };
  return image;
}

const std::vector<std::uint32_t> kDumpTids = { 13584u, 5096u, 18456u, 13052u, 28176u, 777u };

// The FreezeLogger capture: 5096 holds LockA and spins on LockB, 18456 holds
// LockB and spins on LockA; 13052 and 28176 queue behind them; every stack
// also holds stale values that look like locks.
void TestAbbaCycle()
{
  Memory memory;
  memory.words = {
    { kLockA, { 5096u, 1u } },
    { kLockB, { 18456u, 1u } },
    { kBase + 0x2F37EA0ull, { 256u, 0u } },  // stale: owner 256, not held
    { kBase + 0x2F00000ull, { 777u, 1u } },  // held by a thread that is not waiting
  };
  const std::vector<GameLockThreadInput> threads = {
    Sleeper(5096, { 0x132C5A, 0x6D9750, 0x297DDB, 0x22D176, 0x2971A4 },
      { { L"RSI", 0x842A444000ull }, { L"RSP+20", kBase + 0x2F37EA0ull }, { L"RSP+88", kLockB },
        { L"RSP+90", kBase + 0x2F00000ull } }),
    Sleeper(18456, { 0x132C5A, 0x296C3D, 0x6025FB, 0x61C780, 0x6EF480 },
      { { L"RBX", 0x842F8FF5B0ull }, { L"RSP+40", kLockA }, { L"RSP+48", kLockA } }),
    Sleeper(13052, { 0x132C5A, 0x6D9750 }, { { L"RSP+88", kLockB } }),
    Sleeper(28176, { 0x132C5A, 0x296C3D }, { { L"RSP+40", kLockA } }),
  };
  const auto waits = FindGameLockWaits(threads, Image(), memory, kDumpTids);
  Require(waits.size() == 4u, "Four game threads in the lock wait");
  Require(waits[0].wait_module == L"SkyrimSE.exe" && waits[0].wait_offset == 0x132C5A, "Wait site");
  Require(waits[0].frames.size() == 5u && waits[0].frames[4].offset == 0x2971A4, "Frames from the wait site");
  Require(waits[0].candidates.size() == 2u, "The stale lock is left out");
  Require(waits[0].candidates[0].rva == 0x2F3B8E8ull && waits[0].candidates[0].owner_tid == 18456u &&
            waits[0].candidates[0].kind == "spin" && waits[0].candidates[0].owner_waiting &&
            waits[0].candidates[0].location == L"RSP+88",
    "LockB, held by a waiting thread");
  Require(waits[0].candidates[1].owner_tid == 777u && !waits[0].candidates[1].owner_waiting,
    "A lock whose owner is not waiting");
  Require(waits[1].candidates.size() == 1u, "The same lock once");

  const auto cycle = FindEngineLockCycle(waits);
  Require(cycle.size() == 2u, "A two-thread cycle");
  Require(cycle[0].tid == 5096u && cycle[0].lock_rva == 0x2F3B8E8ull && cycle[0].owner_tid == 18456u, "5096 -> 18456");
  Require(cycle[1].tid == 18456u && cycle[1].lock_rva == 0x2EFF8E0ull && cycle[1].owner_tid == 5096u, "18456 -> 5096");

  EngineLockDeadlockInfo deadlock{};
  deadlock.detected = true;
  deadlock.module = L"SkyrimSE.exe";
  deadlock.cycle = cycle;
  Require(skydiag::dump_tool::DescribeEngineLockCycle(deadlock, true) ==
            L"thread 5096 waited for SkyrimSE.exe+0x2f3b8e8 (spin lock), held by thread 18456; thread 18456 waited for "
            L"SkyrimSE.exe+0x2eff8e0 (spin lock), held by thread 5096",
    "The cycle in words");
  Require(skydiag::dump_tool::JoinThreadIds(skydiag::dump_tool::EngineLockDeadlockThreads(deadlock)) == L"5096, 18456",
    "Its threads");
  Require(skydiag::dump_tool::IsReportedGameLockWait(waits[2], deadlock), "A waiter with a lock is reported");
}

// Only the lock function's own frame is read: a value further up the stack
// (a caller's stale pointer to a lock it let go, now held by another waiter)
// adds no wait, and without the frame's end only the registers are read.
void TestOnlyTheLockFunctionsFrame()
{
  Memory memory;
  memory.words = { { kLockA, { 5096u, 1u } }, { kLockB, { 18456u, 1u } } };
  auto a = Sleeper(5096, { 0x132C5A, 0x6D9750 }, { { L"RSP+88", kLockB } });
  a.roots.emplace_back(L"RSP+400", kLockB);  // beyond the frame end: ignored either way
  auto b = Sleeper(18456, { 0x132C5A, 0x296C3D }, {});
  b.roots.emplace_back(L"RSP+400", kLockA);  // a caller's stale pointer
  auto waits = FindGameLockWaits({ a, b }, Image(), memory, kDumpTids);
  Require(waits.size() == 2u && waits[0].candidates.size() == 1u && waits[1].candidates.empty(),
    "A value above the lock function's frame is no wait");
  Require(FindEngineLockCycle(waits).empty(), "No cycle from a caller's stale pointer");

  // No slot with the return address into the caller: registers only.
  auto noEnd = Sleeper(18456, { 0x132C5A }, { { L"RSP+40", kLockA } });
  noEnd.roots.emplace_back(L"RBX", kLockA);
  noEnd.roots.insert(noEnd.roots.begin(), { L"R12", kLockA });
  waits = FindGameLockWaits({ noEnd }, Image(), memory, kDumpTids);
  Require(waits.size() == 1u && waits[0].candidates.size() == 1u && waits[0].candidates[0].location == L"R12",
    "Without the frame end, registers only");
}

void TestNoCycle()
{
  Memory memory;
  memory.words = { { kLockA, { 5096u, 1u } }, { kLockB, { 18456u, 1u } } };
  // A single waiter: 5096 waits on LockB held by 18456, which is not waiting.
  {
    const auto waits = FindGameLockWaits({ Sleeper(5096, { 0x132C5A }, { { L"RBX", kLockB } }) }, Image(), memory,
      kDumpTids);
    Require(waits.size() == 1u && waits[0].candidates.size() == 1u && !waits[0].candidates[0].owner_waiting,
      "One waiter, owner running");
    Require(FindEngineLockCycle(waits).empty(), "No cycle from one waiter");
  }
  // Two waiters on locks whose owners only wait on each other in one direction.
  {
    const auto waits = FindGameLockWaits({
      Sleeper(5096, { 0x132C5A }, { { L"RBX", kLockB } }),
      Sleeper(18456, { 0x132C5A }, { { L"RBX", 0x842F8FF5B0ull } }),
    }, Image(), memory, kDumpTids);
    Require(waits[0].candidates[0].owner_waiting && FindEngineLockCycle(waits).empty(), "A chain is no cycle");
  }
  // A stale pointer: the lock it points at is free now.
  {
    Memory freed;
    freed.words = { { kLockA, { 0u, 0u } }, { kLockB, { 18456u, 0u } } };
    const auto waits = FindGameLockWaits({
      Sleeper(5096, { 0x132C5A }, { { L"RBX", kLockB } }),
      Sleeper(18456, { 0x132C5A }, { { L"RBX", kLockA } }),
    }, Image(), freed, kDumpTids);
    Require(waits[0].candidates.empty() && waits[1].candidates.empty() && FindEngineLockCycle(waits).empty(),
      "Free locks are no candidates");
  }
  // A lock the thread holds itself (BSSpinLock is recursive) and an owner
  // that is not a thread of the dump.
  {
    Memory own;
    own.words = { { kLockA, { 5096u, 2u } }, { kLockB, { 4242u, 1u } } };
    const auto waits = FindGameLockWaits({ Sleeper(5096, { 0x132C5A }, { { L"RBX", kLockA }, { L"RDI", kLockB } }) },
      Image(), own, kDumpTids);
    Require(waits[0].candidates.empty(), "Self-held and unknown owners are no candidates");
  }
  // The read/write lock held for read by the waiting thread itself (the
  // cell-load self-deadlock) names no owner: no generic cycle.
  {
    Memory read;
    read.words = { { kRwLock, { 0u, 1u } } };
    const auto waits = FindGameLockWaits({ Sleeper(42, { 0xC073AB, 0x6D356B }, { { L"RBX", kRwLock } }) }, Image(),
      read, { 42u });
    Require(waits.size() == 1u && waits[0].candidates.empty() && FindEngineLockCycle(waits).empty(),
      "Read-held: the signature's to judge");
    EngineLockDeadlockInfo none{};
    Require(!skydiag::dump_tool::IsReportedGameLockWait(waits[0], none), "Nothing to report without a candidate");
    EngineLockDeadlockInfo known{};
    known.detected = true;
    known.pattern_tids = { 42u };
    Require(skydiag::dump_tool::IsReportedGameLockWait(waits[0], known), "A known pattern's thread is reported");
  }
  // Without the game's data in the dump (older dumps), no candidates at all.
  {
    auto image = Image();
    image.writable.clear();
    const auto waits = FindGameLockWaits({ Sleeper(5096, { 0x132C5A }, { { L"RBX", kLockB } }) }, image, memory,
      kDumpTids);
    Require(waits.size() == 1u && waits[0].candidates.empty(), "No writable data, no locks");
  }
}

void TestOnlyDelayWaitsUnderGameCode()
{
  Memory memory;
  memory.words = { { kLockB, { 18456u, 1u } } };
  // A plugin's own sleep, a sync wait in the engine and a running thread are
  // not engine lock waits.
  auto plugin = Sleeper(1, {}, { { L"RBX", kLockB } });
  ModalStackFrame pluginFrame{};
  pluginFrame.has_module = true;
  pluginFrame.module_filename = L"SomePlugin.dll";
  plugin.top_frames.push_back(pluginFrame);
  plugin.frames.push_back(GameFrameRef{ L"SomePlugin.dll", 0x1234 });
  auto sync = Sleeper(2, { 0x5765FF }, { { L"RBX", kLockB } });
  sync.top_frames[0] = System(L"ntdll.dll", L"NtWaitForSingleObject", 0x14);
  sync.top_frames[1] = System(L"KERNELBASE.dll", L"WaitForSingleObjectEx", 0xAF);
  sync.top_frames[2] = System(L"KERNELBASE.dll", L"WaitForSingleObjectEx", 0xAF);
  GameLockThreadInput running{};
  running.tid = 3;
  running.top_frames = { Exe() };
  running.frames = { { L"SkyrimSE.exe", 0xD17465 } };
  running.roots = { { L"RBX", kLockB } };
  const auto waits = FindGameLockWaits({ plugin, sync, running }, Image(), memory, { 1u, 2u, 3u, 18456u });
  Require(waits.empty(), "Only sleeps called by the game executable");
}

void TestThreeThreadCycleAndDeterminism()
{
  const std::uint64_t lockC = kBase + 0x2F40000ull;
  Memory memory;
  memory.words = { { kLockA, { 1u, 1u } }, { kLockB, { 2u, 1u } }, { lockC, { 3u, 1u } } };
  // 1 waits on B (held by 2), 2 on C (held by 3), 3 on A (held by 1); thread
  // 4 waits on A too but is outside the cycle.
  const std::vector<GameLockThreadInput> threads = {
    Sleeper(4, { 0x132C5A }, { { L"RBX", kLockA } }),
    Sleeper(1, { 0x132C5A }, { { L"RBX", kLockB } }),
    Sleeper(2, { 0x132C5A }, { { L"RBX", lockC } }),
    Sleeper(3, { 0x132C5A }, { { L"RBX", kLockA } }),
  };
  const auto waits = FindGameLockWaits(threads, Image(), memory, { 1u, 2u, 3u, 4u });
  const auto cycle = FindEngineLockCycle(waits);
  Require(cycle.size() == 3u, "Three-thread cycle");
  Require(cycle[0].tid == 1u && cycle[1].tid == 2u && cycle[2].tid == 3u && cycle[2].owner_tid == 1u,
    "From the first thread in it, in order");
}

}  // namespace

int main()
{
  try {
    TestOnlyTheLockFunctionsFrame();
    TestWritableSections();
    TestSelectWindows();
    TestDecodeLocks();
    TestAbbaCycle();
    TestNoCycle();
    TestOnlyDelayWaitsUnderGameCode();
    TestThreeThreadCycleAndDeterminism();
  } catch (const std::exception&) {
    return 1;
  }
  return 0;
}
