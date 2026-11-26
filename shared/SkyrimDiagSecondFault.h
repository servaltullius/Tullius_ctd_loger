#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace skydiag::second_fault {

inline constexpr wchar_t kMappingSuffix[] = L"_FAULT2";
inline constexpr wchar_t kPostedSuffix[] = L"_FAULT2_POSTED";
inline constexpr wchar_t kSettledSuffix[] = L"_FAULT2_SETTLED";
inline constexpr std::uint32_t kMagic = 0x32464453;  // SDF2
inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::uint32_t kStreamType = 0x10000u + 0x4632u;
inline constexpr std::uint32_t kDefaultMaxDumps = 3;
inline constexpr std::uint32_t kMaxDumps = 20;
inline constexpr char kFilePrefix[] = "SkyrimDiag_SecondFault_";
// The plugin waits for the helper in slices this long and looks at the slot
// after each, so a wake another waiter took cannot hold it to the limit.
inline constexpr std::uint32_t kHoldSliceMs = 50;

// A fatal exception the vectored handler could not publish is a candidate for
// B only when it lost Frozen (no other failure), it is not Crash Logger
// reporting a kept exception, and A's committed generation is the same before
// and after the try.
constexpr bool IsSecondFaultCandidate(
  bool lostFrozen,
  bool recognizedKept,
  std::uint32_t seqBefore,
  std::uint32_t seqAfter) noexcept
{
  return lostFrozen && !recognizedKept && seqBefore != 0u && (seqBefore & 1u) == 0u && seqAfter == seqBefore;
}

#ifdef _WIN32
inline constexpr LONG kEmpty = 0;
inline constexpr LONG kWriting = 1;
inline constexpr LONG kPosted = 2;
inline constexpr LONG kSettled = 3;

struct Slot
{
  std::uint32_t magic;
  std::uint32_t version;
  std::uint32_t size;
  volatile LONG state;

  // The little-endian pair is written atomically by the plugin so a filter
  // that resumes after a thaw cannot mark a newer generation as resumed.
  alignas(LONG64) std::uint32_t a_crash_seq;
  volatile LONG resumed;
  // dropped counts for the A generation in dropped_seq, and the pair changes
  // in one exchange, so a late count of one A never carries into the next.
  alignas(LONG64) std::uint32_t dropped_seq;
  volatile LONG dropped;
  std::uint32_t tid;
  std::uint32_t code;
  std::uint64_t address;
  std::uint64_t qpc;
  std::uint64_t hold_until_qpc;
  EXCEPTION_RECORD exception_record;
  CONTEXT context;
};

static_assert(offsetof(Slot, resumed) == offsetof(Slot, a_crash_seq) + sizeof(std::uint32_t));
static_assert(offsetof(Slot, dropped) == offsetof(Slot, dropped_seq) + sizeof(std::uint32_t));

inline LONG ReadState(const Slot& slot) noexcept
{
  return InterlockedCompareExchange(const_cast<volatile LONG*>(&slot.state), kEmpty, kEmpty);
}

inline void SetOwner(Slot& slot, std::uint32_t seq) noexcept
{
  InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&slot.a_crash_seq), seq);
}

inline std::uint32_t ReadOwner(Slot& slot) noexcept
{
  return static_cast<std::uint32_t>(
    InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&slot.a_crash_seq), 0, 0));
}

inline void MarkResumed(Slot& slot, std::uint32_t seq) noexcept
{
  constexpr auto kResumedBit = LONG64{1} << 32;
  InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&slot.a_crash_seq), kResumedBit | seq, seq);
}

// One more B of generation seq found the slot taken by that generation's B.
inline void CountDropped(Slot& slot, std::uint32_t seq) noexcept
{
  auto* const word = reinterpret_cast<volatile LONG64*>(&slot.dropped_seq);
  LONG64 seen = InterlockedCompareExchange64(word, 0, 0);
  // Only other B threads compete here; a bounded loop never spins forever.
  for (int attempt = 0; attempt < 64; ++attempt) {
    const auto seenBits = static_cast<std::uint64_t>(seen);
    const std::uint64_t count = static_cast<std::uint32_t>(seenBits) == seq ? (seenBits >> 32) : 0u;
    const auto next = static_cast<LONG64>(((count + 1u) << 32) | seq);
    const LONG64 prior = InterlockedCompareExchange64(word, next, seen);
    if (prior == seen) {
      return;
    }
    seen = prior;
  }
}

inline LONG ReadDropped(Slot& slot, std::uint32_t seq) noexcept
{
  const auto seen = static_cast<std::uint64_t>(
    InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&slot.dropped_seq), 0, 0));
  return static_cast<std::uint32_t>(seen) == seq ? static_cast<LONG>(seen >> 32) : 0;
}

enum class SlotClaim
{
  kClaimed,          // the caller writes B and posts it
  kTaken,            // another B has it; counted as dropped when it is this A's
  kGenerationEnded,  // A was let go meanwhile; left settled for the helper to empty
};

// Claims the empty slot for generation seq. ownsGeneration() says whether A
// still holds Frozen at seq. The owner is written right after the claim so a
// competing B of the same A sees whose slot it is.
template <class OwnsGeneration>
SlotClaim ClaimSlot(Slot& slot, std::uint32_t seq, OwnsGeneration ownsGeneration) noexcept
{
  if (InterlockedCompareExchange(&slot.state, kWriting, kEmpty) != kEmpty) {
    if (ReadOwner(slot) == seq && ownsGeneration()) {
      CountDropped(slot, seq);
    }
    return SlotClaim::kTaken;
  }
  SetOwner(slot, seq);
  if (!ownsGeneration()) {
    InterlockedExchange(&slot.state, kSettled);
    return SlotClaim::kGenerationEnded;
  }
  return SlotClaim::kClaimed;
}

// ADR-0036: the latest candidate of each recent thread, a fixed table. An
// entry, {tid | seq << 32, record}, changes only in one 16-byte exchange: two
// threads never share an entry and nobody reads half of one. A thread uses its
// own entry, else claims a free one; with none free it may take an entry whose
// generation is over (isLive false), never a live one. Nothing here waits.
class CandidateTable
{
public:
  static constexpr std::size_t kCount = 8;

  // Notes (record, seq) for tid. seq == 0 forgets tid's candidate for record
  // only. False when nothing was stored.
  template <class IsLive>
  bool Note(DWORD tid, const void* record, std::uint32_t seq, IsLive isLive) noexcept
  {
    const Value next{ Pack(tid, seq), reinterpret_cast<LONG64>(record) };
    for (auto& entry : entries_) {
      const Value seen = Read(entry);
      if (TidOf(seen) != tid) {
        continue;
      }
      if (seq == 0u) {
        return seen.record == next.record && Exchange(entry, seen, Value{});
      }
      if (Exchange(entry, seen, next)) {
        return true;
      }
      break;  // reclaimed as over; claim another below
    }
    if (seq == 0u) {
      return false;
    }
    for (const bool reclaim : { false, true }) {
      for (auto& entry : entries_) {
        const Value seen = Read(entry);
        const bool free = TidOf(seen) == 0u;
        if (reclaim ? (free || isLive(SeqOf(seen))) : !free) {
          continue;
        }
        if (Exchange(entry, seen, next)) {
          return true;
        }
      }
    }
    return false;
  }

  // The generation tid noted for record, taking the entry; 0 when none.
  std::uint32_t Take(DWORD tid, const void* record) noexcept
  {
    for (auto& entry : entries_) {
      const Value seen = Read(entry);
      if (TidOf(seen) == tid && seen.record == reinterpret_cast<LONG64>(record) && Exchange(entry, seen, Value{})) {
        return SeqOf(seen);
      }
    }
    return 0u;
  }

private:
  struct Value
  {
    LONG64 key = 0;     // tid | seq << 32
    LONG64 record = 0;  // EXCEPTION_RECORD address
  };
  struct alignas(16) Entry
  {
    volatile LONG64 words[2]{};
  };

  static LONG64 Pack(DWORD tid, std::uint32_t seq) noexcept
  {
    return static_cast<LONG64>((static_cast<std::uint64_t>(seq) << 32) | tid);
  }
  static DWORD TidOf(const Value& value) noexcept
  {
    return static_cast<DWORD>(static_cast<std::uint64_t>(value.key));
  }
  static std::uint32_t SeqOf(const Value& value) noexcept
  {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(value.key) >> 32);
  }
  static Value Read(Entry& entry) noexcept
  {
    LONG64 seen[2]{};
    // Compares with zero and stores zero only over zero: a read either way.
    InterlockedCompareExchange128(entry.words, 0, 0, seen);
    return { seen[0], seen[1] };
  }
  static bool Exchange(Entry& entry, const Value& expected, const Value& desired) noexcept
  {
    LONG64 comparand[2]{ expected.key, expected.record };
    return InterlockedCompareExchange128(entry.words, desired.record, desired.key, comparand) != 0;
  }

  Entry entries_[kCount]{};
};
#endif

struct Stream
{
  std::uint32_t version;
  std::uint32_t size;
  std::uint32_t tid;
  std::uint32_t code;
  std::uint64_t address;
  std::uint64_t qpc;
  std::uint32_t a_crash_seq;
  std::uint32_t a_code;
  std::uint64_t a_address;
  std::uint32_t a_tid;
  std::uint32_t reserved;
};

static_assert(sizeof(Stream) == 56);

}  // namespace skydiag::second_fault
