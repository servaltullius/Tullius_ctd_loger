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

struct WctFreezeSummary
{
  bool has = false;
  int threads = 0;
  int cycles = 0;
  std::vector<std::uint32_t> cycle_thread_ids;
  std::uint32_t longest_wait_tid = 0;
  std::uint64_t longest_wait_ms = 0;
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
};

std::vector<std::uint32_t> ExtractWctCandidateThreadIds(std::string_view wctJsonUtf8, std::size_t maxN);

std::optional<WctCaptureDecision> TryParseWctCaptureDecision(std::string_view wctJsonUtf8);
std::optional<WctFreezeSummary> TryParseWctFreezeSummary(std::string_view wctJsonUtf8);

}  // namespace skydiag::dump_tool::internal
