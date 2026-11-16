#pragma once

// A FormID where a pointer belongs (ADR-0033): when a crash register holds a
// small value that the game's own FormID table resolves to a form, the
// helper looks it up in the live game at the crash and adds every byte the
// lookup read to the dump; the analyzer runs the same lookup over the dump
// and gets the same form. Shared so both sides read the same places, through
// a reader like shared/SkyrimDiagRtti.h's (all or nothing):
//   bool read(std::uint64_t address, void* destination, std::size_t size)
// Free of <Windows.h> for the host-independent tests.
//
// Layouts (CommonLibSSE-NG):
// - TESForm::allForms, at Address Library ID 514351 (SE 1.5.x) / 400507
//   (AE 1.6 and later), holds a pointer to a BSTHashMap<FormID, TESForm*>.
// - BSTHashMap (BSTScatterTable, standard parent, heap allocator): capacity
//   at 0x0C (a power of two), free count at 0x10, the end-of-chain sentinel
//   at 0x18, the entry array at 0x28.
// - An entry is 0x18 bytes: the FormID at 0x00, the TESForm* at 0x08, the
//   next entry of its chain at 0x10 (null: the slot is empty; the sentinel:
//   the chain ends). A key's chain starts at entry hash & (capacity - 1).
// - The hash is CRC-32 (reflected, polynomial 0xEDB88320) over the FormID's
//   four bytes, starting at 0 and without the final inversion.
// - TESForm::formID is at 0x14 (shared/SkyrimDiagRtti.h).

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace skydiag::form_map {

inline constexpr std::uint64_t kAllFormsIdSe = 514351;
inline constexpr std::uint64_t kAllFormsIdAe = 400507;

// The Address Library ID of TESForm::allForms for a file of this format
// (1: SE 1.5.x; 2 and 5: AE and later).
inline constexpr std::uint64_t AllFormsId(std::int32_t addressLibraryFormat)
{
  return addressLibraryFormat == 1 ? kAllFormsIdSe : kAllFormsIdAe;
}

inline constexpr std::uint64_t kMapCapacityOffset = 0x0C;
inline constexpr std::uint64_t kMapFreeOffset = 0x10;
inline constexpr std::uint64_t kMapSentinelOffset = 0x18;
inline constexpr std::uint64_t kMapEntriesOffset = 0x28;
inline constexpr std::size_t kMapHeaderBytes = 0x30;
inline constexpr std::size_t kEntryBytes = 0x18;
inline constexpr std::uint64_t kFormIdOffset = 0x14;
// More slots than any load order fills (a large one has a few million forms).
inline constexpr std::uint32_t kMaxCapacity = 1u << 26;
// Longer chains than a power-of-two table with a fair hash ever builds.
inline constexpr std::uint32_t kMaxChain = 64;

// A register value worth looking up: above the first 64 KB, which is never
// mapped (a null pointer plus an offset there says enough), and no wider
// than a FormID.
inline constexpr bool IsFormIdCandidate(std::uint64_t value)
{
  return value >= 0x10000u && value <= 0xFFFFFFFFull;
}

namespace detail {

inline constexpr std::array<std::uint32_t, 256> MakeCrcTable()
{
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256u; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1u) != 0u ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    table[i] = c;
  }
  return table;
}

inline constexpr std::array<std::uint32_t, 256> kCrcTable = MakeCrcTable();

inline bool PlausiblePointer(std::uint64_t p)
{
  return p >= 0x10000u && p < 0x7FFFFFFF0000ull && (p & 7u) == 0u;
}

template <class Reader, class T>
bool ReadValue(const Reader& read, std::uint64_t address, T& out)
{
  return read(address, &out, sizeof(T));
}

template <class T>
T Field(const std::uint8_t* bytes, std::uint64_t offset)
{
  T value{};
  std::memcpy(&value, bytes + offset, sizeof(T));
  return value;
}

}  // namespace detail

// The game's hash of a FormID (BSCRC32<FormID>).
inline constexpr std::uint32_t FormIdHash(std::uint32_t formId)
{
  std::uint32_t hash = 0;
  for (int i = 0; i < 4; ++i) {
    const std::uint32_t byte = (formId >> (8 * i)) & 0xFFu;
    hash = (hash >> 8) ^ detail::kCrcTable[(hash ^ byte) & 0xFFu];
  }
  return hash;
}

// The address of the form whose FormID is formId, from the table that
// allFormsSlot (the address of TESForm::allForms) points at; nullopt when
// the table has no such form, or anything read does not look like the
// layout above. A found form's own formID must be formId.
template <class Reader>
std::optional<std::uint64_t> FindForm(const Reader& read, std::uint64_t allFormsSlot, std::uint32_t formId)
{
  std::uint64_t map = 0;
  if (!detail::ReadValue(read, allFormsSlot, map) || !detail::PlausiblePointer(map)) {
    return std::nullopt;
  }
  // The header and each entry are read whole, so what the helper keeps is
  // one range each.
  std::array<std::uint8_t, kMapHeaderBytes> header{};
  if (!read(map, header.data(), header.size())) {
    return std::nullopt;
  }
  const auto capacity = detail::Field<std::uint32_t>(header.data(), kMapCapacityOffset);
  const auto free = detail::Field<std::uint32_t>(header.data(), kMapFreeOffset);
  const auto sentinel = detail::Field<std::uint64_t>(header.data(), kMapSentinelOffset);
  const auto entries = detail::Field<std::uint64_t>(header.data(), kMapEntriesOffset);
  if (capacity == 0u || capacity > kMaxCapacity || (capacity & (capacity - 1u)) != 0u || free >= capacity ||
      sentinel < 0x10000u || !detail::PlausiblePointer(entries)) {
    return std::nullopt;
  }
  const std::uint64_t entriesEnd = entries + std::uint64_t{ capacity } * kEntryBytes;
  std::uint64_t entry = entries + std::uint64_t{ FormIdHash(formId) & (capacity - 1u) } * kEntryBytes;
  for (std::uint32_t step = 0; step < kMaxChain; ++step) {
    std::array<std::uint8_t, kEntryBytes> bytes{};
    if (!read(entry, bytes.data(), bytes.size())) {
      return std::nullopt;
    }
    const auto key = detail::Field<std::uint32_t>(bytes.data(), 0x00);
    const auto form = detail::Field<std::uint64_t>(bytes.data(), 0x08);
    const auto next = detail::Field<std::uint64_t>(bytes.data(), 0x10);
    if (next == 0u) {
      return std::nullopt;  // an empty slot: no form hashes here
    }
    if (key == formId) {
      std::uint32_t ownId = 0;
      if (!detail::PlausiblePointer(form) || !detail::ReadValue(read, form + kFormIdOffset, ownId) || ownId != formId) {
        return std::nullopt;
      }
      return form;
    }
    if (next == sentinel) {
      return std::nullopt;
    }
    // Every chain stays inside the table's own entry array.
    if (next < entries || next >= entriesEnd || (next - entries) % kEntryBytes != 0u) {
      return std::nullopt;
    }
    entry = next;
  }
  return std::nullopt;
}

}  // namespace skydiag::form_map
