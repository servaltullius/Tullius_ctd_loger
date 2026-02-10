#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace skydiag::helper {
struct AttachedProcess;
struct HelperConfig;
class LoadStats;
}

namespace skydiag::helper::internal {

void DoManualCapture(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  const skydiag::helper::LoadStats& loadStats,
  std::uint32_t adaptiveLoadingThresholdSec,
  std::wstring_view trigger);

}  // namespace skydiag::helper::internal

