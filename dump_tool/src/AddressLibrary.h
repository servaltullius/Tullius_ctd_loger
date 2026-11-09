#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace skydiag::dump_tool {

// ADR-0027: an Address Library file (the bytes the helper puts in the dump,
// kMinidumpUserStream_AddressLibrary), turned into an offset -> ID lookup.
// Formats 1 (1.5.x) and 2 (1.6.x) are delta encoded; format 5 (1.7.x) is a
// table of 32-bit offsets indexed by ID. Only exact offsets map to an ID.
class AddressLibrary
{
public:
  // False (and empty) for a file it does not understand or that is cut short.
  bool Parse(std::string_view bytes);

  // The Address Library ID whose address is this offset (RVA) in the game
  // executable; the lowest one when several share it.
  std::optional<std::uint64_t> IdAt(std::uint64_t offset) const;

  std::size_t Size() const { return m_byOffset.size(); }
  std::int32_t Format() const { return m_format; }

private:
  // (offset, id), sorted by offset then id.
  std::vector<std::pair<std::uint64_t, std::uint64_t>> m_byOffset;
  std::int32_t m_format = 0;
};

}  // namespace skydiag::dump_tool
