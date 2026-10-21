#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skydiag::dump_tool::internal {

// A main thread that answered within this many seconds of the capture was not
// frozen, nor stuck in a load, at that moment. The helper's own hang threshold
// (thresholdSec) is longer: a manual capture between the two still shows a
// stall in progress, so it is not called "not a hang".
inline constexpr double kNotHangHeartbeatAgeSec = 5.0;

struct WctCaptureDecision
{
  bool has = false;
  std::string kind;
  double secondsSinceHeartbeat = 0.0;
  std::uint32_t thresholdSec = 0;
  bool isLoading = false;
};

struct WctModalDialog
{
  std::uint32_t tid = 0;
  std::string title;  // UTF-8
  std::string text;   // UTF-8
  bool has_owner = false;
  bool owner_disabled = false;
};

struct WctFreezeSummary
{
  bool has = false;
  std::uint32_t capture_passes = 0;
  int threads = 0;
  int cycles = 0;
  bool cycle_consensus = false;
  std::vector<std::uint32_t> cycle_thread_ids;
  std::vector<std::uint32_t> repeated_cycle_tids;
  bool consistent_loading_signal = false;
  // The blocked thread with the earliest wait start (a tick count, not a
  // duration); 0 when none or several share it (SkyrimDiagWctWait.h).
  std::uint32_t longest_wait_tid = 0;
  std::uint64_t longest_wait_start_tick = 0;
  bool longest_wait_tid_consensus = false;
  bool has_capture = false;
  std::string capture_kind;
  double secondsSinceHeartbeat = 0.0;
  std::uint32_t thresholdSec = 0;
  bool isLoading = false;
  bool suggestsHang = false;
  bool pss_snapshot_requested = false;
  bool pss_snapshot_used = false;
  std::uint32_t pss_snapshot_capture_ms = 0;
  std::string pss_snapshot_status;
  std::string dump_transport;
  // Visible #32770 dialogs captured by the helper (absent in older captures).
  bool modal_dialogs_captured = false;
  std::vector<WctModalDialog> modal_dialogs;
};

// WCT_OBJECT_TYPE and WCT_OBJECT_STATUS values (wct.h) the lock-owner chain uses.
inline constexpr std::uint32_t kWctObjectTypeCriticalSection = 1u;
inline constexpr std::uint32_t kWctObjectTypeMutex = 3u;
inline constexpr std::uint32_t kWctObjectTypeThread = 8u;
inline constexpr std::uint32_t kWctObjectStatusOwned = 6u;

// The thread holding the lock a thread waits on, read from that thread's own
// wait chain: its thread node, an owned critical section or mutex, then the
// owner's thread node. Only chains that are not cycles (those are deadlocks).
struct WctLockOwner
{
  std::uint32_t owner_tid = 0;    // 0: the lock's owner was not reported
  std::uint32_t object_type = 0;  // kWctObjectTypeCriticalSection / kWctObjectTypeMutex
  // Two usable passes both named this owner; false with one pass, when they
  // disagree or the owner is unknown.
  bool confirmed = false;
  // The owner's own entry, in every pass that has it: blocked, with no wait
  // object after its thread node. A thread suspended from outside looks like
  // this (so does a wait WCT cannot see, such as an SRW lock).
  bool owner_blocked_without_wait_object = false;
  // The owner made no context switch between the first and last pass.
  bool owner_context_switches_stable = false;
};

// nullopt when the waiter's chain holds no owned critical section or mutex.
std::optional<WctLockOwner> FindWctLockOwner(std::string_view wctJsonUtf8, std::uint32_t waiterTid);

std::vector<std::uint32_t> ExtractWctCandidateThreadIds(std::string_view wctJsonUtf8, std::size_t maxN);

std::uint32_t CountWctThreadsWithStableContextSwitches(
  std::string_view wctJsonUtf8,
  const std::vector<std::uint32_t>& targetTids);

std::optional<WctCaptureDecision> TryParseWctCaptureDecision(std::string_view wctJsonUtf8);
std::optional<WctFreezeSummary> TryParseWctFreezeSummary(std::string_view wctJsonUtf8);

}  // namespace skydiag::dump_tool::internal
