#include "AnalyzerInternals.h"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace skydiag::dump_tool::internal {

std::vector<std::uint32_t> ExtractWctCandidateThreadIds(std::string_view wctJsonUtf8, std::size_t maxN)
{
  std::vector<std::uint32_t> tids;
  if (wctJsonUtf8.empty()) {
    return tids;
  }

  try {
    const auto j = nlohmann::json::parse(wctJsonUtf8, nullptr, /*allow_exceptions=*/true);
    if (!j.is_object()) {
      return tids;
    }
    const auto it = j.find("threads");
    if (it == j.end() || !it->is_array()) {
      return tids;
    }
    struct Row
    {
      std::uint32_t tid = 0;
      std::uint64_t waitTime = 0;
    };
    std::vector<Row> nonCycle;

    for (const auto& t : *it) {
      if (!t.is_object()) {
        continue;
      }
      const auto tid = t.value("tid", 0u);
      if (tid == 0u) {
        continue;
      }
      const bool isCycle = t.value("isCycle", false);
      if (isCycle) {
        tids.push_back(tid);
        continue;
      }

      std::uint64_t waitTime = 0;
      const auto nodesIt = t.find("nodes");
      if (nodesIt != t.end() && nodesIt->is_array()) {
        for (const auto& node : *nodesIt) {
          if (!node.is_object()) {
            continue;
          }
          const auto thIt = node.find("thread");
          if (thIt == node.end() || !thIt->is_object()) {
            continue;
          }
          waitTime = std::max<std::uint64_t>(waitTime, thIt->value("waitTime", 0ull));
        }
      }
      nonCycle.push_back(Row{ tid, waitTime });
    }

    // If cycles exist, prioritize them (deadlock likely).
    if (!tids.empty()) {
      return tids;
    }

    // Otherwise pick the longest-waiting threads (best-effort).
    if (maxN == 0) {
      return tids;
    }
    std::sort(nonCycle.begin(), nonCycle.end(), [](const Row& a, const Row& b) {
      if (a.waitTime != b.waitTime) {
        return a.waitTime > b.waitTime;
      }
      return a.tid < b.tid;
    });
    for (const auto& r : nonCycle) {
      if (r.tid == 0u) {
        continue;
      }
      tids.push_back(r.tid);
      if (tids.size() >= maxN) {
        break;
      }
    }
  } catch (...) {
    return tids;
  }
  return tids;
}

std::optional<WctCaptureDecision> TryParseWctCaptureDecision(std::string_view wctJsonUtf8)
{
  if (wctJsonUtf8.empty()) {
    return std::nullopt;
  }
  try {
    const auto j = nlohmann::json::parse(wctJsonUtf8, nullptr, /*allow_exceptions=*/true);
    if (!j.is_object()) {
      return std::nullopt;
    }
    const auto it = j.find("capture");
    if (it == j.end() || !it->is_object()) {
      return std::nullopt;
    }

    WctCaptureDecision d{};
    d.has = true;
    d.kind = it->value("kind", std::string{});
    d.secondsSinceHeartbeat = it->value("secondsSinceHeartbeat", 0.0);
    d.thresholdSec = it->value("thresholdSec", 0u);
    d.isLoading = it->value("isLoading", false);
    return d;
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace skydiag::dump_tool::internal

