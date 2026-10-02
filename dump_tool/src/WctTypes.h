#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skydiag::dump_tool::internal {

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
  std::uint32_t longest_wait_tid = 0;
  std::uint64_t longest_wait_ms = 0;
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

std::vector<std::uint32_t> ExtractWctCandidateThreadIds(std::string_view wctJsonUtf8, std::size_t maxN);

std::uint32_t CountWctThreadsWithStableContextSwitches(
  std::string_view wctJsonUtf8,
  const std::vector<std::uint32_t>& targetTids);

std::optional<WctCaptureDecision> TryParseWctCaptureDecision(std::string_view wctJsonUtf8);
std::optional<WctFreezeSummary> TryParseWctFreezeSummary(std::string_view wctJsonUtf8);

}  // namespace skydiag::dump_tool::internal
