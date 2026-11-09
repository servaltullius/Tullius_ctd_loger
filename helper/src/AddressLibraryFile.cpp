#include "AddressLibraryFile.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <system_error>

#include "SkyrimDiagHelper/PluginScanner.h"

namespace skydiag::helper::internal {
namespace {

// The Address Library file beside the process's executable, for its version.
std::string ReadAddressLibrary(std::uint32_t pid)
{
  std::filesystem::path exePath;
  const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  const bool resolved = process && skydiag::helper::TryResolveProcessImagePath(process, exePath);
  if (process) {
    CloseHandle(process);
  }
  if (!resolved) {
    return {};
  }
  const auto version = skydiag::helper::QueryFileVersion(exePath);
  if (!version) {
    return {};
  }
  const std::filesystem::path file = exePath.parent_path() / L"Data" / L"SKSE" / L"Plugins" /
    AddressLibraryFileName(version->major, version->minor, version->build, version->revision);
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
  // By process id, so a later dump needs nothing from the process again: the
  // helper serves one game, and a crash dump must not depend on opening it.
  static std::mutex mutex;
  static std::map<std::uint32_t, std::string> byPid;
  std::lock_guard lock(mutex);
  const auto it = byPid.find(pid);
  if (it != byPid.end()) {
    return it->second;
  }
  return byPid.emplace(pid, ReadAddressLibrary(pid)).first->second;
}

}  // namespace skydiag::helper::internal
