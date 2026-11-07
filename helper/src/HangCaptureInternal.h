#pragma once

#include "HangCapture.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/HangDetect.h"
#include "SkyrimDiagHelper/ProcessAttach.h"

namespace skydiag::helper::internal {

void ResetHangCaptureEpisode(HangCaptureState* state);

void EnsureTargetWindowForPid(HangCaptureState* state, std::uint32_t pid);

// How many threads the process has; nullopt when the thread snapshot fails.
std::optional<std::size_t> CountProcessThreads(std::uint32_t pid);

// A confirmed hang of a game that is already closing (IsGameProcessClosing):
// logs it, marks the episode and returns true, so no hang dump is taken.
bool SkipHangIfGameIsClosing(
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  const skydiag::helper::HangDecision& decision,
  HangCaptureState* state);

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
