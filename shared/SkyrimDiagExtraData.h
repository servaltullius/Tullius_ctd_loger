#pragma once

// What NPC a reference spawned from a leveled list was rolled from. Such an
// actor's base is a temporary NPC the game made (FormID FFxxxxxx) with no
// plugin; ExtraLeveledCreature in the reference's extra data keeps the NPC
// it came from, which names the plugin.
//
// Shared by the helper and the analyzer, as SkyrimDiagRtti.h is: the helper
// runs it over the live game for such references and adds every range read
// to the dump; the analyzer runs it over the dump. The reader is
// all-or-nothing: bool read(std::uint64_t address, void* destination, std::size_t size).
//
// Layouts (CommonLibSSE-NG):
// - TESObjectREFR: ExtraDataList extraList at 0x70. Its BaseExtraList starts
//   with BSExtraData* data on SE and on AE before 1.6.629; from 1.6.629 it has
//   a vtable first and the data pointer is at +0x08 (0x78). Which one is told
//   from the RTTI: a BaseExtraList object at 0x70 means the AE layout.
// - BSExtraData: vtable, BSExtraData* next at 0x08 (a singly linked list).
// - ExtraLeveledCreature: TESActorBase* originalBase at 0x10.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_set>

#include "SkyrimDiagRtti.h"

namespace skydiag::extra_data {

inline constexpr std::uint64_t kRefrExtraListOffset = 0x70;
inline constexpr std::string_view kBaseExtraListTypeName = ".?AVBaseExtraList@@";
inline constexpr std::string_view kLeveledCreatureTypeName = ".?AVExtraLeveledCreature@@";
inline constexpr std::uint64_t kNextOffset = 0x08;
inline constexpr std::uint64_t kLeveledOriginalOffset = 0x10;
// More extra data than a reference carries in practice; past it the list is
// taken as damaged.
inline constexpr std::size_t kMaxExtraData = 64;

// FormIDs the game makes while it runs.
inline bool IsRuntimeFormId(std::uint32_t formId)
{
  return (formId & 0xFF000000u) == 0xFF000000u;
}

// The first BSExtraData of a reference's extra data list, 0 when none.
template <class Reader>
std::uint64_t FirstExtraData(const Reader& read, std::uint64_t refr)
{
  using rtti::detail::PlausiblePointer;
  using rtti::detail::ReadValue;
  const std::uint64_t list = refr + kRefrExtraListOffset;
  std::uint64_t first = 0;
  if (!ReadValue(read, list, first)) {
    return 0;
  }
  // AE 1.6.629+: the BaseExtraList's own vtable comes first.
  if (const auto self = rtti::ReadObject(read, list); self && self->type_name == kBaseExtraListTypeName) {
    if (!ReadValue(read, list + 8, first)) {
      return 0;
    }
  }
  return PlausiblePointer(first) ? first : 0;
}

// The NPC (or other actor base) a leveled reference was rolled from: none
// when the reference has no ExtraLeveledCreature, or it cannot be read.
template <class Reader>
std::optional<rtti::ObjectInfo> ReadLeveledOriginal(const Reader& read, std::uint64_t refr)
{
  using rtti::detail::PlausiblePointer;
  using rtti::detail::ReadValue;
  std::unordered_set<std::uint64_t> seen;
  for (std::uint64_t node = FirstExtraData(read, refr); PlausiblePointer(node) && seen.size() < kMaxExtraData;) {
    if (!seen.insert(node).second) {
      break;  // a loop: damaged memory
    }
    const auto extra = rtti::ReadObject(read, node);
    if (!extra) {
      break;
    }
    if (extra->type_name == kLeveledCreatureTypeName) {
      std::uint64_t original = 0;
      if (!ReadValue(read, node + kLeveledOriginalOffset, original) || !PlausiblePointer(original)) {
        return std::nullopt;
      }
      auto object = rtti::ReadObject(read, original);
      return object && object->is_form ? object : std::nullopt;
    }
    if (!ReadValue(read, node + kNextOffset, node)) {
      break;
    }
  }
  return std::nullopt;
}

}  // namespace skydiag::extra_data
