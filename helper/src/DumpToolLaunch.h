#pragma once

#include <Windows.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <cstdint>

namespace skydiag::helper {
struct HelperConfig;
}

namespace skydiag::helper::internal {

enum class DumpToolViewerLaunchResult : std::uint8_t {
  kLaunched = 0,
  kLaunchFailed = 1,
  kExitedImmediately = 2,
};

void StartDumpToolHeadlessIfConfigured(
  const skydiag::helper::HelperConfig& cfg,
  const std::wstring& dumpPath,
  const std::filesystem::path& outBase);

bool StartDumpToolHeadlessAsync(
  const skydiag::helper::HelperConfig& cfg,
  const std::wstring& dumpPath,
  const std::filesystem::path& outBase,
  HANDLE* outProcess,
  std::wstring* err);

DumpToolViewerLaunchResult StartDumpToolViewer(
  const skydiag::helper::HelperConfig& cfg,
  const std::wstring& dumpPath,
  const std::filesystem::path& outBase,
  std::wstring_view reason);

}  // namespace skydiag::helper::internal
