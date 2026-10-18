#include "WctTypes.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "SkyrimDiagWctWait.h"

namespace skydiag::dump_tool::internal {
namespace {

struct ThreadWaitState
{
  std::uint32_t object_status = 0;
  std::uint64_t wait_start_tick = 0;
};

// The queried thread's own node: WCT lists it first, before the objects it
// waits on and their owner threads.
ThreadWaitState ReadOwnWaitState(const nlohmann::json& thread, std::uint32_t tid)
{
  const auto nodesIt = thread.find("nodes");
  if (nodesIt == thread.end() || !nodesIt->is_array() || nodesIt->empty() || !nodesIt->front().is_object()) {
    return {};
  }
  const auto& node = nodesIt->front();
  const auto thIt = node.find("thread");
  if (thIt == node.end() || !thIt->is_object()) {
    return {};
  }
  const auto idIt = thIt->find("threadId");
  if (idIt != thIt->end() && idIt->is_number_unsigned() && idIt->get<std::uint32_t>() != tid) {
    return {};
  }
  ThreadWaitState state{};
  state.object_status = node.value("objectStatus", 0u);
  state.wait_start_tick = thIt->value("waitTime", 0ull);
  return state;
}

WctLongestWaiter FindLongestWaiter(const nlohmann::json& threads)
{
  WctLongestWaiter waiter;
  if (!threads.is_array()) {
    return waiter;
  }
  for (const auto& t : threads) {
    if (!t.is_object()) {
      continue;
    }
    const auto tid = t.value("tid", 0u);
    const auto state = ReadOwnWaitState(t, tid);
    waiter.Observe(tid, state.object_status, state.wait_start_tick);
  }
  return waiter;
}

// The same longest waiter in both passes. Helpers before this rule picked the
// thread with the latest wait start, so the flag they wrote is recomputed from
// the passes they recorded; nullopt when the capture has no two passes.
std::optional<bool> LongestWaiterConsensusFromPasses(const nlohmann::json& j)
{
  const auto passesIt = j.find("passes");
  if (passesIt == j.end() || !passesIt->is_array() || passesIt->size() < 2u) {
    return std::nullopt;
  }
  WctLongestWaiter waiters[2];
  for (std::size_t i = 0; i < 2u; ++i) {
    const auto& pass = (*passesIt)[i];
    if (!pass.is_object() || !pass.value("capture_usable", false)) {
      return false;
    }
    const auto threadsIt = pass.find("threads");
    if (threadsIt == pass.end()) {
      return false;
    }
    waiters[i] = FindLongestWaiter(*threadsIt);
  }
  return waiters[0].tid() != 0u && waiters[0].tid() == waiters[1].tid();
}

}  // namespace

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
    summary.longest_wait_tid_consensus =
      LongestWaiterConsensusFromPasses(j).value_or(j.value("longest_wait_tid_consensus", false));

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
      }
      const auto waiter = FindLongestWaiter(*threadsIt);
      summary.longest_wait_tid = waiter.tid();
      summary.longest_wait_start_tick = waiter.wait_start_tick();
    }

    const auto dialogsIt = j.find("modal_dialogs");
    if (dialogsIt != j.end() && dialogsIt->is_array()) {
      summary.modal_dialogs_captured = true;
      for (const auto& d : *dialogsIt) {
        if (!d.is_object()) {
          continue;
        }
        const auto tidIt = d.find("tid");
        if (tidIt == d.end() || !tidIt->is_number_unsigned()) {
          continue;
        }
        WctModalDialog dialog{};
        dialog.tid = tidIt->get<std::uint32_t>();
        if (dialog.tid == 0u) {
          continue;
        }
        if (const auto it = d.find("title"); it != d.end() && it->is_string()) {
          dialog.title = it->get<std::string>();
        }
        if (const auto it = d.find("text"); it != d.end() && it->is_string()) {
          dialog.text = it->get<std::string>();
        }
        if (const auto it = d.find("has_owner"); it != d.end() && it->is_boolean()) {
          dialog.has_owner = it->get<bool>();
        }
        if (const auto it = d.find("owner_disabled"); it != d.end() && it->is_boolean()) {
          dialog.owner_disabled = it->get<bool>();
        }
        summary.modal_dialogs.push_back(std::move(dialog));
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
      std::uint64_t waitStartTick = 0;  // max: not blocked, or no wait start
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

      const auto state = ReadOwnWaitState(t, tid);
      const bool blocked = state.object_status == kWctObjectStatusBlocked && state.wait_start_tick != 0u;
      nonCycle.push_back(Row{ tid, blocked ? state.wait_start_tick : std::numeric_limits<std::uint64_t>::max() });
    }

    // Otherwise pick the longest-waiting threads (best-effort): the earliest
    // wait start first (SkyrimDiagWctWait.h), then the threads not blocked.
    if (maxN == 0) {
      return tids;
    }
    std::sort(nonCycle.begin(), nonCycle.end(), [](const Row& a, const Row& b) {
      if (a.waitStartTick != b.waitStartTick) {
        return a.waitStartTick < b.waitStartTick;
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

std::uint32_t CountWctThreadsWithStableContextSwitches(
  std::string_view wctJsonUtf8,
  const std::vector<std::uint32_t>& targetTids)
{
  if (wctJsonUtf8.empty() || targetTids.empty()) {
    return 0u;
  }

  try {
    const auto json = nlohmann::json::parse(wctJsonUtf8, nullptr, /*allow_exceptions=*/true);
    const auto passesIt = json.find("passes");
    if (passesIt == json.end() || !passesIt->is_array() || passesIt->size() < 2u) {
      return 0u;
    }

    const std::unordered_set<std::uint32_t> targets(targetTids.begin(), targetTids.end());
    const auto readPass = [&](const nlohmann::json& pass) {
      std::unordered_map<std::uint32_t, std::uint64_t> switchesByTid;
      if (!pass.is_object() || !pass.value("capture_usable", false)) {
        return switchesByTid;
      }
      const auto threadsIt = pass.find("threads");
      if (threadsIt == pass.end() || !threadsIt->is_array()) {
        return switchesByTid;
      }
      for (const auto& thread : *threadsIt) {
        if (!thread.is_object()) {
          continue;
        }
        const std::uint32_t tid = thread.value("tid", 0u);
        if (tid == 0u || !targets.contains(tid)) {
          continue;
        }
        const auto nodesIt = thread.find("nodes");
        if (nodesIt == thread.end() || !nodesIt->is_array()) {
          continue;
        }
        for (const auto& node : *nodesIt) {
          if (!node.is_object()) {
            continue;
          }
          const auto threadIt = node.find("thread");
          if (threadIt == node.end() || !threadIt->is_object()) {
            continue;
          }
          const auto threadIdIt = threadIt->find("threadId");
          const auto switchesIt = threadIt->find("contextSwitches");
          if (threadIdIt == threadIt->end() || !threadIdIt->is_number_unsigned() ||
              switchesIt == threadIt->end() || !switchesIt->is_number_unsigned() ||
              threadIdIt->get<std::uint32_t>() != tid) {
            continue;
          }
          switchesByTid[tid] = switchesIt->get<std::uint64_t>();
          break;
        }
      }
      return switchesByTid;
    };

    const auto first = readPass(passesIt->front());
    const auto last = readPass(passesIt->back());
    std::uint32_t stable = 0u;
    for (const auto tid : targets) {
      const auto firstIt = first.find(tid);
      const auto lastIt = last.find(tid);
      if (firstIt != first.end() && lastIt != last.end() && firstIt->second == lastIt->second) {
        ++stable;
      }
    }
    return stable;
  } catch (...) {
    return 0u;
  }
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
