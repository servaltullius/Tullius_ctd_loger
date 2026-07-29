#pragma once

#include <cstdint>
#include <string>

namespace skydiag::dump_tool {

struct DumpIdentity
{
  std::string sha256;
  std::uint64_t size_bytes = 0;
  std::uint64_t last_write_time_utc_100ns = 0;

  [[nodiscard]] bool IsValid() const noexcept
  {
    return sha256.size() == 64u && size_bytes > 0u && last_write_time_utc_100ns > 0u;
  }

  // The SHA-256 alone identifies the bytes, but SkyrimDiag deliberately binds
  // persisted analysis state to the file generation that was analyzed too.
  // This compact metadata component is combined with sha256 as two nested path
  // components so same-content files with different FILETIME values never
  // share authoritative summary or triage state.
  [[nodiscard]] std::string StorageMetadataKey() const;
};

// dumpFileHandle must be the same still-open file object that owns mappedBytes.
// Metadata and the SHA-256 are read from that single generation even if the
// original path is renamed or replaced while analysis is running.
bool ComputeDumpIdentity(
  void* dumpFileHandle,
  const void* mappedBytes,
  std::uint64_t mappedSize,
  DumpIdentity* out,
  std::wstring* err);

bool ReadDumpFileMetadata(
  void* dumpFileHandle,
  std::uint64_t* sizeBytes,
  std::uint64_t* lastWriteTimeUtc100ns,
  std::wstring* err);

}  // namespace skydiag::dump_tool
