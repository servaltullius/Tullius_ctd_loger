#include "WctTypes.h"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace skydiag::dump_tool::internal {

std::optional<WctFreezeSummary> TryParseWctFreezeSummary(std::string_view wctJsonUtf8)
{
  if (wctJsonUtf8.empty()) {
    return std::nullopt;
  }

  try {
    const auto j = nlohmann::json::parse(wctJsonUtf8, nullptr, /*allow_exceptions=*/true);
    if (!j.is_object()) {
      return std::nullopt;
    }

    WctFreezeSummary summary{};
    summary.has = true;
    summary.capture_passes = j.value("capture_passes", 0u);
    summary.cycle_consensus = j.value("cycle_consensus", false);
    summary.consistent_loading_signal = j.value("consistent_loading_signal", false);
    summary.longest_wait_tid_consensus = j.value("longest_wait_tid_consensus", false);

    const auto repeatedCycleIt = j.find("repeated_cycle_tids");
    if (repeatedCycleIt != j.end() && repeatedCycleIt->is_array()) {
      for (const auto& tidValue : *repeatedCycleIt) {
        if (!tidValue.is_number_unsigned()) {
          continue;
        }
        const auto tid = tidValue.get<std::uint32_t>();
        if (tid != 0u) {
          summary.repeated_cycle_tids.push_back(tid);
        }
      }
    }

    const auto threadsIt = j.find("threads");
    if (threadsIt != j.end() && threadsIt->is_array()) {
      summary.threads = static_cast<int>(threadsIt->size());
      for (const auto& t : *threadsIt) {
        if (!t.is_object()) {
          continue;
        }
        const auto tid = t.value("tid", 0u);
        if (t.value("isCycle", false)) {
          summary.cycles++;
          if (tid != 0u) {
            summary.cycle_thread_ids.push_back(tid);
          }
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
        if (waitTime > summary.longest_wait_ms) {
          summary.longest_wait_ms = waitTime;
          summary.longest_wait_tid = tid;
        }
      }
    }

    const auto capIt = j.find("capture");
    if (capIt != j.end() && capIt->is_object()) {
      summary.has_capture = true;
      summary.capture_kind = capIt->value("kind", std::string{});
      summary.secondsSinceHeartbeat = capIt->value("secondsSinceHeartbeat", 0.0);
      summary.thresholdSec = capIt->value("thresholdSec", 0u);
      summary.isLoading = capIt->value("isLoading", false);
      summary.pss_snapshot_requested = capIt->value("pss_snapshot_requested", false);
      summary.pss_snapshot_used = capIt->value("pss_snapshot_used", false);
      summary.pss_snapshot_capture_ms = capIt->value("pss_snapshot_capture_ms", 0u);
      summary.pss_snapshot_status = capIt->value("pss_snapshot_status", std::string{});
      summary.dump_transport = capIt->value("dump_transport", std::string{});
      summary.suggestsHang =
        (summary.capture_kind == "hang") ||
        (summary.thresholdSec > 0u && summary.secondsSinceHeartbeat >= static_cast<double>(summary.thresholdSec));
    }

    return summary;
  } catch (...) {
    return std::nullopt;
  }
}

std::vector<std::uint32_t> ExtractWctCandidateThreadIds(std::string_view wctJsonUtf8, std::size_t maxN)
{
  std::vector<std::uint32_t> tids;
  if (wctJsonUtf8.empty()) {
    return tids;
  }

  try {
    const auto freeze = TryParseWctFreezeSummary(wctJsonUtf8);
    if (!freeze || !freeze->has) {
      return tids;
    }
    if (!freeze->cycle_thread_ids.empty()) {
      return freeze->cycle_thread_ids;
    }

    const auto j = nlohmann::json::parse(wctJsonUtf8, nullptr, /*allow_exceptions=*/true);
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
  const auto freeze = TryParseWctFreezeSummary(wctJsonUtf8);
  if (!freeze || !freeze->has_capture) {
    return std::nullopt;
  }

  WctCaptureDecision d{};
  d.has = true;
  d.kind = freeze->capture_kind;
  d.secondsSinceHeartbeat = freeze->secondsSinceHeartbeat;
  d.thresholdSec = freeze->thresholdSec;
  d.isLoading = freeze->isLoading;
  return d;
}

}  // namespace skydiag::dump_tool::internal
