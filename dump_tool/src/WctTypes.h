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

std::vector<std::uint32_t> ExtractWctCandidateThreadIds(std::string_view wctJsonUtf8, std::size_t maxN);

std::optional<WctCaptureDecision> TryParseWctCaptureDecision(std::string_view wctJsonUtf8);

}  // namespace skydiag::dump_tool::internal
