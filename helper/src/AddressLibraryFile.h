#pragma once

#include <cstdint>
#include <string>

namespace skydiag::helper::internal {

// The Address Library file SKSE plugins load for this game version
// (ADR-0027): "version-1-5-97-0.bin" for 1.5.x, "versionlib-1-6-1170-0.bin"
// for 1.6 and later (1.7.104 too).
std::wstring AddressLibraryFileName(
  std::uint32_t major,
  std::uint32_t minor,
  std::uint32_t build,
  std::uint32_t revision);

// Largest Address Library file read (the 1.7.104 one is about 2.3 MB).
inline constexpr std::uint64_t kAddressLibraryMaxBytes = 16ull * 1024ull * 1024ull;

// The game's Address Library file from Data\SKSE\Plugins beside its
// executable, read through the same file system the game sees (MO2's VFS
// included, the helper being the game's child process). Read once per game
// executable; empty when the version, the file or the read is missing, or the
// file is over kAddressLibraryMaxBytes.
std::string GameAddressLibraryBytes(std::uint32_t pid);

}  // namespace skydiag::helper::internal
