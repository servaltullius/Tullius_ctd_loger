#pragma once

#include <cstdint>
#include <string_view>

namespace skydiag::hash {

constexpr std::uint64_t Fnv1a64(std::string_view s) noexcept
{
  std::uint64_t hash = 14695981039346656037ull;
  for (const unsigned char c : s) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash;
}

}  // namespace skydiag::hash

