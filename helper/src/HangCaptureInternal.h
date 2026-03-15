#pragma once

#include "HangCapture.h"

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/HangDetect.h"
#include "SkyrimDiagHelper/ProcessAttach.h"

namespace skydiag::helper::internal {

void ResetHangCaptureEpisode(HangCaptureState* state);

void EnsureTargetWindowForPid(HangCaptureState* state, std::uint32_t pid);

bool SuppressHangAndLogIfNeeded(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  const skydiag::helper::HangDecision& decision,
  std::uint32_t stateFlags,
  std::uint64_t nowQpc,
  bool confirmedPhase,
  HangCaptureState* state);

HangTickResult ExecuteConfirmedHangCapture(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  const skydiag::helper::HangDecision& decision,
  std::uint32_t stateFlags,
  std::wstring* pendingHangViewerDumpPath,
  HangCaptureState* state);

}  // namespace skydiag::helper::internal
