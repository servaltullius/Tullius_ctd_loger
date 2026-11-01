#pragma once

#include <cstdint>
#include <string_view>

// FNV-1a 64, the one hash the plugin, the analyzer and the save keys share
// (menu and module labels, resource paths, SkyrimDiagSaveLoad.h).
namespace skydiag::hash {

inline constexpr std::uint64_t kFnv1a64Offset = 14695981039346656037ull;
inline constexpr std::uint64_t kFnv1a64Prime = 1099511628211ull;

constexpr std::uint64_t Fnv1a64Byte(std::uint64_t hash, unsigned char c) noexcept
{
  return (hash ^ c) * kFnv1a64Prime;
}

constexpr std::uint64_t Fnv1a64(std::string_view s) noexcept
{
  std::uint64_t hash = kFnv1a64Offset;
  for (const unsigned char c : s) {
    hash = Fnv1a64Byte(hash, c);
  }
  return hash;
}

}  // namespace skydiag::hash
