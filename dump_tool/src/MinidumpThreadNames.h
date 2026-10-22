#pragma once

// Thread names from a minidump's ThreadNamesStream (SetThreadDescription
// names, which MiniDumpWriteDump records). Free of <Windows.h> and DbgHelp
// types so the host-independent tests can feed it hand-built bytes; every
// offset and length read from the dump is bounds-checked.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace skydiag::dump_tool::minidump {

inline constexpr std::uint32_t kMinidumpSignature = 0x504D444Du;  // "MDMP"
inline constexpr std::uint32_t kThreadNamesStreamType = 24u;

namespace thread_names_detail {

inline bool ReadU32(const std::uint8_t* base, std::uint64_t size, std::uint64_t off, std::uint32_t& out)
{
  if (off > size || size - off < sizeof(out)) {
    return false;
  }
  std::memcpy(&out, base + off, sizeof(out));
  return true;
}

inline bool ReadU64(const std::uint8_t* base, std::uint64_t size, std::uint64_t off, std::uint64_t& out)
{
  if (off > size || size - off < sizeof(out)) {
    return false;
  }
  std::memcpy(&out, base + off, sizeof(out));
  return true;
}

}  // namespace thread_names_detail

// The name of one thread (UTF-16, as the dump stores it): the first entry for
// that thread id whose name fits in the dump. Empty without the stream, when
// the stream does not fit in the dump, or when the thread has no name. Only
// that one name is copied: a dump's entries can all point at one 64 KiB name,
// and copying every entry would let a crafted stream ask for tens of GB.
inline std::u16string FindThreadName(const void* dumpBase, std::uint64_t dumpSize, std::uint32_t threadId)
{
  using thread_names_detail::ReadU32;
  using thread_names_detail::ReadU64;
  std::u16string out;
  const auto* base = static_cast<const std::uint8_t*>(dumpBase);
  if (threadId == 0u) {
    return out;
  }
  // MINIDUMP_HEADER: Signature, Version, NumberOfStreams, StreamDirectoryRva, ...
  std::uint32_t signature = 0;
  std::uint32_t streamCount = 0;
  std::uint32_t directoryRva = 0;
  if (!base || !ReadU32(base, dumpSize, 0, signature) || signature != kMinidumpSignature ||
      !ReadU32(base, dumpSize, 8, streamCount) || !ReadU32(base, dumpSize, 12, directoryRva)) {
    return out;
  }
  // MINIDUMP_DIRECTORY: StreamType, Location.DataSize, Location.Rva (12 bytes).
  constexpr std::uint64_t kDirectoryEntrySize = 12u;
  if (directoryRva > dumpSize || streamCount > (dumpSize - directoryRva) / kDirectoryEntrySize) {
    return out;
  }
  std::uint64_t streamRva = 0;
  std::uint64_t streamSize = 0;
  bool found = false;
  for (std::uint32_t i = 0; i < streamCount && !found; ++i) {
    const std::uint64_t entry = directoryRva + i * kDirectoryEntrySize;
    std::uint32_t type = 0;
    std::uint32_t dataSize = 0;
    std::uint32_t rva = 0;
    if (!ReadU32(base, dumpSize, entry, type) || !ReadU32(base, dumpSize, entry + 4u, dataSize) ||
        !ReadU32(base, dumpSize, entry + 8u, rva)) {
      return out;
    }
    if (type == kThreadNamesStreamType) {
      streamRva = rva;
      streamSize = dataSize;
      found = true;
    }
  }
  if (!found || streamRva > dumpSize || streamSize > dumpSize - streamRva) {
    return out;
  }
  // MINIDUMP_THREAD_NAME_LIST: NumberOfThreadNames, then packed
  // MINIDUMP_THREAD_NAME entries { ThreadId, RVA64 RvaOfThreadName } (12 bytes).
  constexpr std::uint64_t kNameEntrySize = 12u;
  std::uint32_t nameCount = 0;
  if (!ReadU32(base, dumpSize, streamRva, nameCount) || streamSize < 4u ||
      nameCount > (streamSize - 4u) / kNameEntrySize) {
    return out;
  }
  // A Windows thread description is at most 32767 UTF-16 code units.
  constexpr std::uint32_t kMaxNameBytes = 32767u * 2u;
  for (std::uint32_t i = 0; i < nameCount; ++i) {
    const std::uint64_t entry = streamRva + 4u + i * kNameEntrySize;
    std::uint32_t tid = 0;
    std::uint64_t nameRva = 0;
    std::uint32_t lengthBytes = 0;
    if (!ReadU32(base, dumpSize, entry, tid) || !ReadU64(base, dumpSize, entry + 4u, nameRva)) {
      break;
    }
    // MINIDUMP_STRING: Length in bytes (no terminator), then UTF-16 code units.
    if (tid != threadId || nameRva == 0u || !ReadU32(base, dumpSize, nameRva, lengthBytes) || lengthBytes == 0u ||
        (lengthBytes % 2u) != 0u || lengthBytes > kMaxNameBytes || nameRva + 4u > dumpSize ||
        lengthBytes > dumpSize - (nameRva + 4u)) {
      continue;
    }
    out.assign(lengthBytes / 2u, u'\0');
    std::memcpy(out.data(), base + nameRva + 4u, lengthBytes);
    return out;
  }
  return out;
}

// A thread name for reports, JSON and bucket keys: a control character
// (CR, LF, NUL, ...) becomes U+FFFD so a name cannot break a report line.
inline std::wstring ThreadNameForDisplay(std::u16string_view name)
{
  std::wstring out;
  out.reserve(name.size());
  for (const char16_t c : name) {
    const bool control = c < 0x20u || (c >= 0x7Fu && c <= 0x9Fu);
    out.push_back(control ? static_cast<wchar_t>(0xFFFDu) : static_cast<wchar_t>(c));
  }
  return out;
}

}  // namespace skydiag::dump_tool::minidump
