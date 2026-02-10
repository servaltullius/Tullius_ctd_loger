#pragma once

#include <Windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace skydiag::helper {
struct HelperConfig;
}

namespace skydiag::helper::internal {

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

void StartDumpToolViewer(
  const skydiag::helper::HelperConfig& cfg,
  const std::wstring& dumpPath,
  const std::filesystem::path& outBase,
  std::wstring_view reason);

}  // namespace skydiag::helper::internal
