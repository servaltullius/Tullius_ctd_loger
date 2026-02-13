#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include "SkyrimDiagHelper/HangSuppression.h"

namespace skydiag::helper {
struct AttachedProcess;
struct HelperConfig;
class LoadStats;
}

namespace skydiag::helper::internal {

struct HangCaptureState
{
  bool hangCapturedThisEpisode = false;
  bool warnedHeartbeatNotInitialized = false;

  bool hangSuppressedNotForegroundThisEpisode = false;
  bool hangSuppressedForegroundGraceThisEpisode = false;
  bool hangSuppressedForegroundResponsiveThisEpisode = false;

  skydiag::helper::HangSuppressionState hangSuppressionState{};
  HWND targetWindow = nullptr;

  bool wasLoading = false;
  std::uint64_t loadStartQpc = 0;
};

enum class HangTickResult : std::uint8_t {
  kContinue = 0,
  kBreak = 1,
};

HangTickResult HandleHangTick(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  skydiag::helper::LoadStats* loadStats,
  const std::filesystem::path& loadStatsPath,
  std::uint32_t* adaptiveLoadingThresholdSec,
  std::uint64_t attachNowQpc,
  std::wstring* pendingHangViewerDumpPath,
  HangCaptureState* state);

}  // namespace skydiag::helper::internal
