#include "DumpIdentity.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

namespace skydiag::dump_tool {
namespace {

class BCryptAlgorithm final
{
public:
  ~BCryptAlgorithm()
  {
    if (handle_) {
      BCryptCloseAlgorithmProvider(handle_, 0);
    }
  }

  BCRYPT_ALG_HANDLE* Put() noexcept { return &handle_; }
  BCRYPT_ALG_HANDLE Get() const noexcept { return handle_; }

private:
  BCRYPT_ALG_HANDLE handle_ = nullptr;
};

class BCryptHash final
{
public:
  ~BCryptHash()
  {
    if (handle_) {
      BCryptDestroyHash(handle_);
    }
  }

  BCRYPT_HASH_HANDLE* Put() noexcept { return &handle_; }
  BCRYPT_HASH_HANDLE Get() const noexcept { return handle_; }

private:
  BCRYPT_HASH_HANDLE handle_ = nullptr;
};

bool BCryptSucceeded(NTSTATUS status) noexcept
{
  return status >= 0;
}

std::string HexLower(const std::vector<std::uint8_t>& bytes)
{
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.resize(bytes.size() * 2u);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    out[i * 2u] = kHex[(bytes[i] >> 4u) & 0x0fu];
    out[i * 2u + 1u] = kHex[bytes[i] & 0x0fu];
  }
  return out;
}

void SetError(std::wstring* err, std::wstring message)
{
  if (err) {
    *err = std::move(message);
  }
}

struct DumpHandleMetadata
{
  std::uint64_t size_bytes = 0;
  std::uint64_t last_write_time_utc_100ns = 0;
  std::uint64_t file_index = 0;
  DWORD volume_serial_number = 0;
};

bool ReadDumpHandleMetadata(
  void* dumpFileHandle,
  DumpHandleMetadata* metadata,
  std::wstring* err)
{
  const HANDLE dumpFile = static_cast<HANDLE>(dumpFileHandle);
  if (!metadata || !dumpFile || dumpFile == INVALID_HANDLE_VALUE) {
    SetError(err, L"Invalid file handle while reading dump identity");
    return false;
  }

  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(dumpFile, &info)) {
    SetError(err, L"GetFileInformationByHandle failed while reading dump identity: " +
                    std::to_wstring(GetLastError()));
    return false;
  }

  ULARGE_INTEGER size{};
  size.HighPart = info.nFileSizeHigh;
  size.LowPart = info.nFileSizeLow;
  ULARGE_INTEGER modified{};
  modified.HighPart = info.ftLastWriteTime.dwHighDateTime;
  modified.LowPart = info.ftLastWriteTime.dwLowDateTime;
  ULARGE_INTEGER fileIndex{};
  fileIndex.HighPart = info.nFileIndexHigh;
  fileIndex.LowPart = info.nFileIndexLow;
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u ||
      size.QuadPart == 0u ||
      modified.QuadPart == 0u) {
    SetError(err, L"Dump identity metadata is incomplete");
    return false;
  }

  metadata->size_bytes = size.QuadPart;
  metadata->last_write_time_utc_100ns = modified.QuadPart;
  metadata->file_index = fileIndex.QuadPart;
  metadata->volume_serial_number = info.dwVolumeSerialNumber;
  if (err) {
    err->clear();
  }
  return true;
}

bool IsSameFileGeneration(
  const DumpHandleMetadata& before,
  const DumpHandleMetadata& after) noexcept
{
  return before.volume_serial_number == after.volume_serial_number &&
         before.file_index == after.file_index &&
         before.size_bytes == after.size_bytes &&
         before.last_write_time_utc_100ns == after.last_write_time_utc_100ns;
}

}  // namespace

std::string DumpIdentity::StorageMetadataKey() const
{
  if (!IsValid()) {
    return {};
  }

  std::ostringstream out;
  out << std::hex << std::nouppercase << std::setfill('0')
      << std::setw(16) << size_bytes
      << '.'
      << std::setw(16) << last_write_time_utc_100ns;
  return out.str();
}

bool ReadDumpFileMetadata(
  void* dumpFileHandle,
  std::uint64_t* sizeBytes,
  std::uint64_t* lastWriteTimeUtc100ns,
  std::wstring* err)
{
  DumpHandleMetadata metadata{};
  if (!ReadDumpHandleMetadata(dumpFileHandle, &metadata, err)) {
    return false;
  }

  if (sizeBytes) {
    *sizeBytes = metadata.size_bytes;
  }
  if (lastWriteTimeUtc100ns) {
    *lastWriteTimeUtc100ns = metadata.last_write_time_utc_100ns;
  }
  if (err) {
    err->clear();
  }
  return true;
}

bool ComputeDumpIdentity(
  void* dumpFileHandle,
  const void* mappedBytes,
  std::uint64_t mappedSize,
  DumpIdentity* out,
  std::wstring* err)
{
  const HANDLE dumpFile = static_cast<HANDLE>(dumpFileHandle);
  if (!out || !dumpFile || dumpFile == INVALID_HANDLE_VALUE ||
      !mappedBytes || mappedSize == 0u ||
      mappedSize > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
    SetError(err, L"Invalid mapped dump while computing identity");
    return false;
  }

  *out = DumpIdentity{};
  DumpHandleMetadata metadataBefore{};
  if (!ReadDumpHandleMetadata(dumpFileHandle, &metadataBefore, err)) {
    return false;
  }
  if (metadataBefore.size_bytes != mappedSize) {
    SetError(err, L"Dump size changed before identity hashing");
    return false;
  }

  BCryptAlgorithm algorithm;
  if (!BCryptSucceeded(BCryptOpenAlgorithmProvider(
        algorithm.Put(),
        BCRYPT_SHA256_ALGORITHM,
        nullptr,
        0))) {
    SetError(err, L"BCryptOpenAlgorithmProvider(SHA256) failed");
    return false;
  }

  DWORD objectLength = 0;
  DWORD resultLength = 0;
  if (!BCryptSucceeded(BCryptGetProperty(
        algorithm.Get(),
        BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectLength),
        sizeof(objectLength),
        &resultLength,
        0)) ||
      objectLength == 0u) {
    SetError(err, L"BCrypt SHA256 object length query failed");
    return false;
  }

  DWORD hashLength = 0;
  if (!BCryptSucceeded(BCryptGetProperty(
        algorithm.Get(),
        BCRYPT_HASH_LENGTH,
        reinterpret_cast<PUCHAR>(&hashLength),
        sizeof(hashLength),
        &resultLength,
        0)) ||
      hashLength != 32u) {
    SetError(err, L"BCrypt SHA256 hash length query failed");
    return false;
  }

  std::vector<std::uint8_t> hashObject(objectLength);
  BCryptHash hash;
  if (!BCryptSucceeded(BCryptCreateHash(
        algorithm.Get(),
        hash.Put(),
        hashObject.data(),
        static_cast<ULONG>(hashObject.size()),
        nullptr,
        0,
        0))) {
    SetError(err, L"BCryptCreateHash(SHA256) failed");
    return false;
  }

  const auto* bytes = static_cast<const std::uint8_t*>(mappedBytes);
  std::uint64_t offset = 0;
  constexpr std::uint64_t kHashChunkBytes = 64ull * 1024ull * 1024ull;
  while (offset < mappedSize) {
    const auto take64 = (std::min)(kHashChunkBytes, mappedSize - offset);
    const auto take = static_cast<ULONG>(take64);
    if (!BCryptSucceeded(BCryptHashData(
          hash.Get(),
          const_cast<PUCHAR>(bytes + static_cast<std::size_t>(offset)),
          take,
          0))) {
      SetError(err, L"BCryptHashData(SHA256) failed");
      return false;
    }
    offset += take64;
  }

  std::vector<std::uint8_t> digest(hashLength);
  if (!BCryptSucceeded(BCryptFinishHash(
        hash.Get(),
        digest.data(),
        static_cast<ULONG>(digest.size()),
        0))) {
    SetError(err, L"BCryptFinishHash(SHA256) failed");
    return false;
  }

  DumpHandleMetadata metadataAfter{};
  if (!ReadDumpHandleMetadata(dumpFileHandle, &metadataAfter, err)) {
    return false;
  }
  if (!IsSameFileGeneration(metadataBefore, metadataAfter)) {
    SetError(err, L"Dump changed while computing identity");
    return false;
  }

  out->sha256 = HexLower(digest);
  out->size_bytes = metadataBefore.size_bytes;
  out->last_write_time_utc_100ns = metadataBefore.last_write_time_utc_100ns;
  if (err) {
    err->clear();
  }
  return true;
}

}  // namespace skydiag::dump_tool
