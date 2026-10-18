#pragma once

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace skydiag {

namespace resource_path_detail {

constexpr char ToLowerAscii(char c) noexcept
{
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

constexpr bool IsSeparator(char c) noexcept
{
  return c == '\\' || c == '/';
}

// Length of the UTF-8 sequence a lead byte starts (1 for a stray byte).
constexpr std::size_t Utf8SequenceLength(unsigned char lead) noexcept
{
  return lead >= 0xF0u ? 4u : lead >= 0xE0u ? 3u : lead >= 0xC0u ? 2u : 1u;
}

}  // namespace resource_path_detail

// The game names loose files in the ANSI code page (CP949 on Korean Windows),
// not UTF-8. Writes the path relative to Data as UTF-8, lower-cased ASCII
// with backslashes, cut at a character boundary, and returns its FNV-1a hash.
// Pure-ASCII paths, nearly all of them, skip the code page conversion. Before
// v0.2.68 the bytes were stored as they came and lower-casing bent CP949
// trail bytes ('A'-'Z') into other characters.
inline std::uint64_t NormalizeResourcePath(
  std::string_view in,
  unsigned codePage,
  char* out,
  std::size_t outCap,
  std::size_t& outLen) noexcept
{
  using namespace resource_path_detail;
  outLen = 0;
  if (!out || outCap == 0) {
    return 0;
  }

  char utf8[1024];
  const bool hasHighByte =
    std::any_of(in.begin(), in.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x80u; });
  if (hasHighByte && codePage != CP_UTF8) {
    wchar_t wide[520];
    const int inLen = static_cast<int>(std::min<std::size_t>(in.size(), 512u));
    const int wideLen = MultiByteToWideChar(codePage, 0, in.data(), inLen, wide, 520);
    const int utf8Len = wideLen > 0
      ? WideCharToMultiByte(CP_UTF8, 0, wide, wideLen, utf8, static_cast<int>(sizeof(utf8)), nullptr, nullptr)
      : 0;
    if (utf8Len > 0) {
      in = std::string_view(utf8, static_cast<std::size_t>(utf8Len));
    }
  }

  std::size_t i = 0;

  // Trim leading whitespace (rare, but safe).
  while (i < in.size() && static_cast<unsigned char>(in[i]) <= 0x20) {
    i++;
  }

  // If an absolute path is observed (e.g. "...\\Skyrim Special Edition\\Data\\meshes\\..."), strip everything up to
  // the last "\data\" component. This keeps paths relative for MO2 provider lookup.
  for (std::size_t pos = in.size(); pos-- > i;) {
    if (!IsSeparator(in[pos])) {
      continue;
    }
    if (pos + 5 >= in.size()) {  // need: sep + "data" + sep
      continue;
    }
    if (ToLowerAscii(in[pos + 1]) == 'd' && ToLowerAscii(in[pos + 2]) == 'a' && ToLowerAscii(in[pos + 3]) == 't' &&
        ToLowerAscii(in[pos + 4]) == 'a' && IsSeparator(in[pos + 5])) {
      i = pos + 6;  // after "\data\"
      break;
    }
  }

  // Strip "data\\" prefix if present (case-insensitive).
  if (i + 5 <= in.size() && ToLowerAscii(in[i]) == 'd' && ToLowerAscii(in[i + 1]) == 'a' &&
      ToLowerAscii(in[i + 2]) == 't' && ToLowerAscii(in[i + 3]) == 'a' && IsSeparator(in[i + 4])) {
    i += 5;
  }

  // Strip leading slashes.
  while (i < in.size() && IsSeparator(in[i])) {
    i++;
  }

  // Normalize into out buffer: lower-case ASCII + backslashes. In UTF-8
  // every byte of a non-ASCII character is >= 0x80, so neither touches it.
  for (; i < in.size() && (outLen + 1) < outCap; i++) {
    char c = in[i];
    if (c == '/') {
      c = '\\';
    }
    out[outLen++] = ToLowerAscii(c);
  }
  if (i < in.size()) {
    // Cut short: drop a character the buffer could not hold whole.
    std::size_t start = outLen;
    while (start > 0 && (static_cast<unsigned char>(out[start - 1]) & 0xC0u) == 0x80u) {
      --start;
    }
    if (start > 0 && static_cast<unsigned char>(out[start - 1]) >= 0xC0u &&
        outLen - (start - 1) < Utf8SequenceLength(static_cast<unsigned char>(out[start - 1]))) {
      outLen = start - 1;
    }
  }
  out[outLen] = '\0';

  std::uint64_t hash = 14695981039346656037ull;  // FNV-1a 64 offset basis
  for (std::size_t k = 0; k < outLen; ++k) {
    hash ^= static_cast<unsigned char>(out[k]);
    hash *= 1099511628211ull;
  }
  return hash;
}

}  // namespace skydiag
