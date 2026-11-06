#pragma once

#include <Windows.h>

#include <cstdint>
#include <cstring>

namespace skydiag::helper::internal {

// A blackbox ring entry (BlackboxEvent, ResourceEntry) is a seqlock: seq is
// odd while the game writes it, even once committed.
template <class Entry>
std::uint32_t ReadEntrySequence(const Entry* entry) noexcept
{
  auto* const sequence = reinterpret_cast<volatile LONG*>(const_cast<volatile std::uint32_t*>(&entry->seq));
  return static_cast<std::uint32_t>(InterlockedCompareExchange(sequence, 0, 0));
}

// Copies a committed entry; on a writer that keeps it busy, the copy is
// zeroed with an odd seq (invalid) and false is returned.
template <class Entry>
bool CopyStableSeqlockEntry(const Entry* source, Entry* destination) noexcept
{
  constexpr int kRingEntrySnapshotAttempts = 8;
  if (!source || !destination) {
    return false;
  }
  for (int attempt = 0; attempt < kRingEntrySnapshotAttempts; ++attempt) {
    const std::uint32_t before = ReadEntrySequence(source);
    if ((before & 1u) != 0u) {
      SwitchToThread();
      continue;
    }

    Entry local{};
    std::memcpy(&local, source, sizeof(local));
    MemoryBarrier();
    const std::uint32_t after = ReadEntrySequence(source);
    if (before == after && (after & 1u) == 0u) {
      std::memcpy(destination, &local, sizeof(local));
      return true;
    }
    SwitchToThread();
  }

  std::memset(destination, 0, sizeof(*destination));
  destination->seq = 1u;  // odd => intentionally invalid/unstable
  return false;
}

}  // namespace skydiag::helper::internal
