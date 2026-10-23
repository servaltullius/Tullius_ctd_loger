#pragma once

// Engine-lock deadlocks (ADR-0021): the game executable's own locks
// (BSSpinLock, BSReadWriteLock) live in its writable data, which freeze dumps
// otherwise do not hold. For game threads waiting in a delay (the engine's
// lock spin sleeps between tries), the helper keeps small windows of that data
// around the values in their non-volatile registers and stack top; the
// analyzer reads the same values back and decodes the locks there. Shared so
// both sides look at the same places, and free of <Windows.h> for the
// host-independent tests.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace skydiag::game_locks {

// Stack bytes above RSP whose qwords are examined (the sleep's own frames and
// the lock call's frames above it).
inline constexpr std::size_t kStackBytes = 0x400;
// One aligned window of game data per value that points into it; a lock is
// 8 bytes, so a value near the end of a window takes the next one as well.
inline constexpr std::uint64_t kWindowBytes = 0x40;
inline constexpr std::size_t kMaxWindowsPerThread = 32;
inline constexpr std::size_t kMaxWindows = 128;
inline constexpr std::size_t kMaxBytes = std::size_t{ 256 } * 1024u;
// The image header the helper keeps with the windows, so the analyzer knows
// which sections are writable data.
inline constexpr std::size_t kHeaderBytes = 0x1000;
// A thread waiting in NtDelayExecution has its instruction pointer this close
// to the stub's start (just after its syscall).
inline constexpr std::uint64_t kDelayStubBytes = 0x20;

// Non-volatile registers (x64): what the lock call kept across the sleep.
// Order and names are the same in the helper and the analyzer.
inline constexpr std::array<const char*, 8> kRegisterNames = { "RBX", "RBP", "RSI", "RDI", "R12", "R13", "R14", "R15" };

struct AddressRange
{
  std::uint64_t begin = 0;
  std::uint64_t end = 0;
};

struct Window
{
  std::uint64_t base = 0;
  std::uint32_t size = 0;
};

namespace detail {

inline std::uint16_t U16(const std::uint8_t* p)
{
  std::uint16_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

inline std::uint32_t U32(const std::uint8_t* p)
{
  std::uint32_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

}  // namespace detail

// The writable, non-executable sections of a PE image (absolute addresses,
// rounded up to whole pages), read from the first bytes of the image as
// mapped. Empty when the bytes are not a PE header.
inline std::vector<AddressRange> WritableSections(const std::uint8_t* header, std::size_t size, std::uint64_t imageBase)
{
  constexpr std::uint32_t kWrite = 0x80000000u;    // IMAGE_SCN_MEM_WRITE
  constexpr std::uint32_t kExecute = 0x20000000u;  // IMAGE_SCN_MEM_EXECUTE
  constexpr std::uint64_t kPage = 0x1000;
  std::vector<AddressRange> out;
  if (!header || size < 0x40 || header[0] != 'M' || header[1] != 'Z') {
    return out;
  }
  const std::uint32_t nt = detail::U32(header + 0x3C);
  if (nt > size || size - nt < 24 || std::memcmp(header + nt, "PE\0\0", 4) != 0) {
    return out;
  }
  const std::uint16_t count = detail::U16(header + nt + 6);
  const std::uint16_t optionalSize = detail::U16(header + nt + 20);
  const std::size_t table = static_cast<std::size_t>(nt) + 24u + optionalSize;
  for (std::uint16_t i = 0; i < count; ++i) {
    const std::size_t at = table + static_cast<std::size_t>(i) * 40u;
    if (at > size || size - at < 40u) {
      break;
    }
    const std::uint32_t virtualSize = detail::U32(header + at + 8);
    const std::uint32_t virtualAddress = detail::U32(header + at + 12);
    const std::uint32_t characteristics = detail::U32(header + at + 36);
    if ((characteristics & kWrite) == 0u || (characteristics & kExecute) != 0u || virtualSize == 0u) {
      continue;
    }
    const std::uint64_t begin = imageBase + virtualAddress;
    const std::uint64_t end = begin + ((static_cast<std::uint64_t>(virtualSize) + kPage - 1u) & ~(kPage - 1u));
    out.push_back(AddressRange{ begin, end });
  }
  return out;
}

// A value that can be the address of a lock in writable game data: 4-byte
// aligned, with all 8 bytes inside one writable section.
inline bool PointsIntoWritable(const std::vector<AddressRange>& writable, std::uint64_t value)
{
  if ((value & 3u) != 0u || value > UINT64_MAX - 8u) {
    return false;
  }
  return std::any_of(writable.begin(), writable.end(), [value](const AddressRange& r) {
    return value >= r.begin && value + 8u <= r.end;
  });
}

struct WindowStats
{
  std::uint32_t pointers = 0;  // values that pointed into writable data
  std::uint32_t windows = 0;
  std::uint64_t bytes = 0;
  bool budget_reached = false;
};

// The windows to keep for the threads' values (each thread's registers, then
// its stack slots), in order: at most kMaxWindowsPerThread new windows per
// thread and kMaxWindows / kMaxBytes in all. Windows are aligned, never
// repeated, and never leave their section.
inline std::vector<Window> SelectWindows(
  const std::vector<AddressRange>& writable,
  const std::vector<std::vector<std::uint64_t>>& threadValues,
  WindowStats* stats = nullptr)
{
  std::vector<Window> windows;
  WindowStats local{};
  const auto has = [&windows](std::uint64_t base) {
    return std::any_of(windows.begin(), windows.end(), [base](const Window& w) { return w.base == base; });
  };
  for (const auto& values : threadValues) {
    std::size_t threadWindows = 0;
    for (const auto value : values) {
      if (!PointsIntoWritable(writable, value)) {
        continue;
      }
      ++local.pointers;
      const std::uint64_t first = value & ~(kWindowBytes - 1u);
      const std::uint64_t last = (value + 7u) & ~(kWindowBytes - 1u);
      for (std::uint64_t base = first; base <= last; base += kWindowBytes) {
        if (has(base)) {
          continue;
        }
        if (threadWindows >= kMaxWindowsPerThread || windows.size() >= kMaxWindows ||
            local.bytes + kWindowBytes > kMaxBytes) {
          local.budget_reached = true;
          continue;
        }
        windows.push_back(Window{ base, static_cast<std::uint32_t>(kWindowBytes) });
        local.bytes += kWindowBytes;
        ++threadWindows;
      }
    }
  }
  local.windows = static_cast<std::uint32_t>(windows.size());
  if (stats) {
    *stats = local;
  }
  return windows;
}

inline bool InDelayStub(std::uint64_t rip, std::uint64_t delayStub)
{
  return delayStub != 0u && rip >= delayStub && rip - delayStub < kDelayStubBytes;
}

// Lock words, as CommonLibSSE-NG lays them out (RE/B/BSAtomic.h):
//   BSSpinLock      { uint32 owningThread; uint32 lockCount; }
//   BSReadWriteLock { uint32 writerThread; uint32 lock; }  lock: 0x80000000 =
//   held for write, low 28 bits = count (readers, or the writer's depth).
inline constexpr std::uint32_t kRwWriteFlag = 0x80000000u;
inline constexpr std::uint32_t kRwCountMask = 0x0FFFFFFFu;
// Larger counts are some other value, not a lock that is held.
inline constexpr std::uint32_t kMaxLockCount = 0xFFFFu;

enum class HeldKind
{
  kNone,
  kSpin,     // a BSSpinLock held by owner
  kRwWrite,  // a BSReadWriteLock held for write by owner
};

struct HeldLock
{
  HeldKind kind = HeldKind::kNone;
  std::uint32_t owner = 0;
  std::uint32_t count = 0;
};

// A lock held by a named thread. Whether that thread exists is the caller's
// to check. A read-held BSReadWriteLock names no thread, and {0, n} cannot be
// told apart from other data, so it is not decoded here (SpinCount /
// RwReaders read a lock whose kind is already known).
inline HeldLock DecodeHeldLock(std::uint32_t word0, std::uint32_t word1)
{
  HeldLock lock{};
  if (word0 == 0u) {
    return lock;
  }
  if ((word1 & kRwWriteFlag) != 0u) {
    const std::uint32_t count = word1 & kRwCountMask;
    if ((word1 & ~(kRwWriteFlag | kRwCountMask)) == 0u && count >= 1u && count <= kMaxLockCount) {
      lock = HeldLock{ HeldKind::kRwWrite, word0, count };
    }
    return lock;
  }
  if (word1 >= 1u && word1 <= kMaxLockCount) {
    lock = HeldLock{ HeldKind::kSpin, word0, word1 };
  }
  return lock;
}

// A BSReadWriteLock held for read: no writer, no write flag, 1+ readers.
inline std::optional<std::uint32_t> RwReaders(std::uint32_t word0, std::uint32_t word1)
{
  const std::uint32_t count = word1 & kRwCountMask;
  if (word0 != 0u || (word1 & ~kRwCountMask) != 0u || count < 1u || count > kMaxLockCount) {
    return std::nullopt;
  }
  return count;
}

}  // namespace skydiag::game_locks
