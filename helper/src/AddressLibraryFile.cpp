#include "AddressLibraryFile.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace skydiag::helper::internal {
namespace {

std::wstring ProcessImagePath(std::uint32_t pid)
{
  const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) {
    return {};
  }
  DWORD size = 32768;
  std::wstring buf(size, L'\0');
  const BOOL ok = QueryFullProcessImageNameW(process, 0, buf.data(), &size);
  CloseHandle(process);
  if (!ok || size == 0) {
    return {};
  }
  buf.resize(size);
  return buf;
}

bool FileVersion(const std::wstring& path, std::uint32_t version[4])
{
  DWORD handle = 0;
  const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
  if (size == 0) {
    return false;
  }
  std::vector<BYTE> data(size);
  VS_FIXEDFILEINFO* info = nullptr;
  UINT infoSize = 0;
  if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
      !VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &infoSize) || !info) {
    return false;
  }
  version[0] = HIWORD(info->dwFileVersionMS);
  version[1] = LOWORD(info->dwFileVersionMS);
  version[2] = HIWORD(info->dwFileVersionLS);
  version[3] = LOWORD(info->dwFileVersionLS);
  return true;
}

std::string ReadAddressLibrary(const std::wstring& exePath)
{
  std::uint32_t version[4]{};
  if (exePath.empty() || !FileVersion(exePath, version)) {
    return {};
  }
  const std::filesystem::path file = std::filesystem::path(exePath).parent_path() / L"Data" / L"SKSE" / L"Plugins" /
    AddressLibraryFileName(version[0], version[1], version[2], version[3]);
  std::error_code ec;
  const auto size = std::filesystem::file_size(file, ec);
  if (ec || size == 0 || size > kAddressLibraryMaxBytes) {
    return {};
  }
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return {};
  }
  std::string bytes(static_cast<std::size_t>(size), '\0');
  if (!in.read(bytes.data(), static_cast<std::streamsize>(size))) {
    return {};
  }
  return bytes;
}

}  // namespace

std::wstring AddressLibraryFileName(
  std::uint32_t major,
  std::uint32_t minor,
  std::uint32_t build,
  std::uint32_t revision)
{
  const std::wstring stem = (major == 1u && minor <= 5u) ? L"version-" : L"versionlib-";
  return stem + std::to_wstring(major) + L"-" + std::to_wstring(minor) + L"-" + std::to_wstring(build) + L"-" +
         std::to_wstring(revision) + L".bin";
}

std::string GameAddressLibraryBytes(std::uint32_t pid)
{
  static std::mutex mutex;
  static std::map<std::wstring, std::string> byExe;
  const std::wstring exePath = ProcessImagePath(pid);
  if (exePath.empty()) {
    return {};
  }
  std::lock_guard lock(mutex);
  const auto it = byExe.find(exePath);
  if (it != byExe.end()) {
    return it->second;
  }
  return byExe.emplace(exePath, ReadAddressLibrary(exePath)).first->second;
}

}  // namespace skydiag::helper::internal
