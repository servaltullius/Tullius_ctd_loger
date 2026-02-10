#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace skydiag::helper {
struct HelperConfig;
}

namespace skydiag::helper::internal {

bool StartEtwCaptureWithProfile(
  const skydiag::helper::HelperConfig& cfg,
  const std::filesystem::path& outBase,
  std::wstring_view profile,
  std::wstring* err);

bool StartEtwCaptureForHang(
  const skydiag::helper::HelperConfig& cfg,
  const std::filesystem::path& outBase,
  std::wstring* outUsedProfile,
  std::wstring* err);

bool StopEtwCaptureToPath(
  const skydiag::helper::HelperConfig& cfg,
  const std::filesystem::path& outBase,
  const std::filesystem::path& etlPath,
  std::wstring* err);

}  // namespace skydiag::helper::internal

