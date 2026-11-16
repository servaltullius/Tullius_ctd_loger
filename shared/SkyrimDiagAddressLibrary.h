#pragma once

// ADR-0027: an Address Library file (Data\SKSE\Plugins\versionlib-*.bin or
// version-*.bin), turned into an offset <-> ID lookup. The helper reads it
// for the game it watches and puts the bytes in the dump
// (kMinidumpUserStream_AddressLibrary); the analyzer reads them back.
// Formats 1 (1.5.x) and 2 (1.6.x) are delta encoded; format 5 (1.7.x) is a
// table of 32-bit offsets indexed by ID. Only exact offsets map to an ID.
// Header-only and free of <Windows.h>: shared by the helper and the analyzer.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace skydiag::address_library {

namespace detail {

// Reads little-endian values from the file, failing (not throwing) at its end.
class Reader
{
public:
  explicit Reader(std::string_view bytes) : m_bytes(bytes) {}

  template <class T>
  bool Read(T* out)
  {
    if (m_bytes.size() - m_pos < sizeof(T)) {
      return false;
    }
    std::memcpy(out, m_bytes.data() + m_pos, sizeof(T));
    m_pos += sizeof(T);
    return true;
  }

  bool Skip(std::size_t n)
  {
    if (m_bytes.size() - m_pos < n) {
      return false;
    }
    m_pos += n;
    return true;
  }

  std::size_t Remaining() const { return m_bytes.size() - m_pos; }

private:
  std::string_view m_bytes;
  std::size_t m_pos = 0;
};

// A count no real file reaches (1.7.104 has under a million IDs); a bigger
// one is a damaged file, not a reason to allocate gigabytes.
inline constexpr std::uint64_t kMaxEntries = 8'000'000;

// Formats 1 and 2: format, version[4], name length, name, pointer size,
// count, then per entry a type byte whose low nibble encodes the ID and high
// nibble the offset, each relative to the previous entry.
inline bool ParseDeltaEncoded(Reader& in, std::vector<std::pair<std::uint64_t, std::uint64_t>>& out)
{
  std::uint32_t version[4]{};
  std::int32_t nameLength = 0;
  if (!in.Read(&version[0]) || !in.Read(&version[1]) || !in.Read(&version[2]) || !in.Read(&version[3]) ||
      !in.Read(&nameLength) || nameLength < 0 || !in.Skip(static_cast<std::size_t>(nameLength))) {
    return false;
  }
  std::int32_t pointerSize = 0;
  std::int32_t count = 0;
  // Every entry takes at least its type byte, so a count beyond the bytes
  // left is a damaged file (and is refused before anything is reserved).
  if (!in.Read(&pointerSize) || !in.Read(&count) || pointerSize <= 0 || count < 0 ||
      static_cast<std::uint64_t>(count) > kMaxEntries || static_cast<std::uint64_t>(count) > in.Remaining()) {
    return false;
  }
  out.reserve(static_cast<std::size_t>(count));
  std::uint64_t prevId = 0;
  std::uint64_t prevOffset = 0;
  const auto ptr = static_cast<std::uint64_t>(pointerSize);
  for (std::int32_t i = 0; i < count; i++) {
    std::uint8_t type = 0;
    if (!in.Read(&type)) {
      return false;
    }
    const std::uint8_t low = type & 0xFu;
    const std::uint8_t high = type >> 4u;
    std::uint64_t id = 0;
    std::uint8_t b8 = 0;
    std::uint16_t b16 = 0;
    std::uint32_t b32 = 0;
    switch (low) {
      case 0: if (!in.Read(&id)) return false; break;
      case 1: id = prevId + 1u; break;
      case 2: if (!in.Read(&b8)) return false; id = prevId + b8; break;
      case 3: if (!in.Read(&b8)) return false; id = prevId - b8; break;
      case 4: if (!in.Read(&b16)) return false; id = prevId + b16; break;
      case 5: if (!in.Read(&b16)) return false; id = prevId - b16; break;
      case 6: if (!in.Read(&b16)) return false; id = b16; break;
      case 7: if (!in.Read(&b32)) return false; id = b32; break;
      default: return false;
    }
    const bool scaled = (high & 8u) != 0u;
    const std::uint64_t base = scaled ? prevOffset / ptr : prevOffset;
    std::uint64_t offset = 0;
    switch (high & 7u) {
      case 0: if (!in.Read(&offset)) return false; break;
      case 1: offset = base + 1u; break;
      case 2: if (!in.Read(&b8)) return false; offset = base + b8; break;
      case 3: if (!in.Read(&b8)) return false; offset = base - b8; break;
      case 4: if (!in.Read(&b16)) return false; offset = base + b16; break;
      case 5: if (!in.Read(&b16)) return false; offset = base - b16; break;
      case 6: if (!in.Read(&b16)) return false; offset = b16; break;
      case 7: if (!in.Read(&b32)) return false; offset = b32; break;
      default: return false;
    }
    if (scaled) {
      offset *= ptr;
    }
    out.emplace_back(offset, id);
    prevId = id;
    prevOffset = offset;
  }
  return true;
}

// Format 5: format, version[4], name[64], pointer size, data format, count,
// then count 32-bit offsets indexed by ID (0: no address for that ID).
inline bool ParseDense(Reader& in, std::vector<std::pair<std::uint64_t, std::uint64_t>>& out)
{
  std::uint32_t version[4]{};
  std::int32_t pointerSize = 0;
  std::int32_t dataFormat = 0;
  std::int32_t count = 0;
  if (!in.Read(&version[0]) || !in.Read(&version[1]) || !in.Read(&version[2]) || !in.Read(&version[3]) ||
      !in.Skip(64) || !in.Read(&pointerSize) || !in.Read(&dataFormat) || !in.Read(&count) || count < 0 ||
      static_cast<std::uint64_t>(count) > kMaxEntries ||
      in.Remaining() < static_cast<std::size_t>(count) * sizeof(std::uint32_t)) {
    return false;
  }
  for (std::int32_t id = 0; id < count; id++) {
    std::uint32_t offset = 0;
    if (!in.Read(&offset)) {
      return false;
    }
    if (offset != 0u) {
      out.emplace_back(offset, static_cast<std::uint64_t>(id));
    }
  }
  return true;
}

}  // namespace detail

class AddressLibrary
{
public:
  // False (and empty) for a file it does not understand or that is cut short.
  bool Parse(std::string_view bytes)
  {
    m_byOffset.clear();
    m_format = 0;
    detail::Reader in(bytes);
    std::int32_t format = 0;
    if (!in.Read(&format)) {
      return false;
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> entries;
    const bool ok = (format == 1 || format == 2) ? detail::ParseDeltaEncoded(in, entries)
                    : format == 5                ? detail::ParseDense(in, entries)
                                                 : false;
    if (!ok || entries.empty()) {
      return false;
    }
    std::sort(entries.begin(), entries.end());
    m_byOffset = std::move(entries);
    m_format = format;
    return true;
  }

  // The Address Library ID whose address is this offset (RVA) in the game
  // executable; the lowest one when several share it.
  std::optional<std::uint64_t> IdAt(std::uint64_t offset) const
  {
    const auto it = std::lower_bound(
      m_byOffset.begin(), m_byOffset.end(), offset,
      [](const std::pair<std::uint64_t, std::uint64_t>& entry, std::uint64_t value) { return entry.first < value; });
    if (it == m_byOffset.end() || it->first != offset) {
      return std::nullopt;
    }
    return it->second;
  }

  // The offset (RVA) of an Address Library ID; nullopt when the file has no
  // address for it. A linear search: for the few IDs looked up per dump.
  std::optional<std::uint64_t> OffsetOf(std::uint64_t id) const
  {
    for (const auto& [offset, entryId] : m_byOffset) {
      if (entryId == id) {
        return offset;
      }
    }
    return std::nullopt;
  }

  std::size_t Size() const { return m_byOffset.size(); }
  // 1: Skyrim SE 1.5.x; 2: AE 1.6.x; 5: 1.7.x (AE IDs).
  std::int32_t Format() const { return m_format; }

private:
  // (offset, id), sorted by offset then id.
  std::vector<std::pair<std::uint64_t, std::uint64_t>> m_byOffset;
  std::int32_t m_format = 0;
};

}  // namespace skydiag::address_library
