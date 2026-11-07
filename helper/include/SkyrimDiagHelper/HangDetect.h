#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace skydiag::helper {

struct HelperConfig;

struct HangDecision {
  bool isHang = false;
  double secondsSinceHeartbeat = 0.0;
  std::uint32_t thresholdSec = 0;
  bool isLoading = false;
};

HangDecision EvaluateHang(
  std::uint64_t nowQpc,
  std::uint64_t lastHeartbeatQpc,
  std::uint64_t qpcFreq,
  std::uint32_t stateFlags,
  std::uint32_t thresholdInGameSec,
  std::uint32_t thresholdLoadingSec);

struct HangThresholds {
  std::uint32_t inGameSec = 0;
  std::uint32_t loadingSec = 0;
};

// The thresholds EvaluateHang takes for these state flags: an open menu raises
// the in-game one to the menu threshold, and the loading one is the adaptive
// threshold when that is enabled and the helper has loading samples.
HangThresholds ComputeHangThresholds(
  const HelperConfig& cfg,
  std::uint32_t stateFlags,
  bool haveLoadingSamples,
  std::uint32_t adaptiveLoadingThresholdSec);

// Whether a game process with this many threads is closing. ExitProcess ends
// every other thread before the DLLs detach, so one thread left (or none)
// means the game loop is over: the heartbeat stopped because the game is
// shutting down, not because it froze, and a dump of an address space being
// torn down fails part way (2026-11-01: 32 s without a heartbeat in a menu,
// one thread left, MiniDumpWriteDump 0x8007012B, then exit code 0). A running
// game always has many threads, and before its heartbeat starts no hang is
// judged at all. An unknown count (the thread snapshot failed) is not taken
// as closing.
constexpr bool IsGameProcessClosing(std::optional<std::size_t> threadCount) noexcept
{
  return threadCount.has_value() && *threadCount <= 1u;
}

}  // namespace skydiag::helper

