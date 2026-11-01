#pragma once

#include <cstdint>
#include <string_view>

namespace skydiag {

// ADR-0025: a SaveLoadStart event names the save a load started with:
// payload.a holds SaveNameKey of the whole name, b..d its first 23 bytes.
// SKSE hands over the name the game loads (no folder, no extension); a folder
// or ".ess" is dropped anyway so a key never depends on how it was written.
constexpr std::string_view SaveNameForEvent(std::string_view name) noexcept
{
  if (const auto slash = name.find_last_of("\\/"); slash != std::string_view::npos) {
    name.remove_prefix(slash + 1);
  }
  constexpr std::string_view kExtension = ".ess";
  if (name.size() > kExtension.size()) {
    bool extension = true;
    const auto tail = name.substr(name.size() - kExtension.size());
    for (std::size_t i = 0; i < kExtension.size(); ++i) {
      const char c = tail[i];
      if (((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c) != kExtension[i]) {
        extension = false;
        break;
      }
    }
    if (extension) {
      name.remove_suffix(kExtension.size());
    }
  }
  return name;
}

// FNV-1a 64 of the name with ASCII case folded, as Windows matches file names.
// Never 0, which a reader takes as "no save".
constexpr std::uint64_t SaveNameKey(std::string_view name) noexcept
{
  std::uint64_t hash = 14695981039346656037ull;
  for (const char ch : SaveNameForEvent(name)) {
    const auto c = static_cast<unsigned char>((ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch);
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash != 0u ? hash : 1u;
}

}  // namespace skydiag
