#include "WctTypes.h"
#include "SkyrimDiagWctWait.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using skydiag::dump_tool::internal::ExtractWctCandidateThreadIds;
using skydiag::dump_tool::internal::CountWctThreadsWithStableContextSwitches;
using skydiag::dump_tool::internal::TryParseWctCaptureDecision;
using skydiag::dump_tool::internal::TryParseWctFreezeSummary;

// ── ExtractWctCandidateThreadIds ────────────────────────

static void Test_EmptyInput_ReturnsEmpty()
{
  const auto tids = ExtractWctCandidateThreadIds("", 8);
  assert(tids.empty());
}

static void Test_InvalidJson_ReturnsEmpty()
{
  const auto tids = ExtractWctCandidateThreadIds("{bad json", 8);
  assert(tids.empty());
}

static void Test_NoThreadsKey_ReturnsEmpty()
{
  const auto tids = ExtractWctCandidateThreadIds(R"({"capture":{}})", 8);
  assert(tids.empty());
}

static void Test_CycleThreadsPrioritized()
{
  const std::string json = R"({
    "threads": [
      {"tid": 100, "isCycle": false, "nodes": [{"thread":{"waitTime":9999}}]},
      {"tid": 200, "isCycle": true},
      {"tid": 300, "isCycle": true}
    ]
  })";
  const auto tids = ExtractWctCandidateThreadIds(json, 8);
  // Only cycle threads should be returned when cycles exist
  assert(tids.size() == 2);
  assert(tids[0] == 200);
  assert(tids[1] == 300);
}

static void Test_NoCycle_SortedByWaitTime()
{
  const std::string json = R"({
    "threads": [
      {"tid": 10, "isCycle": false, "nodes": [{"thread":{"waitTime":100}}]},
      {"tid": 20, "isCycle": false, "nodes": [{"thread":{"waitTime":500}}]},
      {"tid": 30, "isCycle": false, "nodes": [{"thread":{"waitTime":300}}]}
    ]
  })";
  const auto tids = ExtractWctCandidateThreadIds(json, 8);
  assert(tids.size() == 3);
  assert(tids[0] == 20);  // waitTime=500 (highest)
  assert(tids[1] == 30);  // waitTime=300
  assert(tids[2] == 10);  // waitTime=100
}

static void Test_MaxN_Limit()
{
  const std::string json = R"({
    "threads": [
      {"tid": 1, "isCycle": false, "nodes": [{"thread":{"waitTime":100}}]},
      {"tid": 2, "isCycle": false, "nodes": [{"thread":{"waitTime":200}}]},
      {"tid": 3, "isCycle": false, "nodes": [{"thread":{"waitTime":300}}]}
    ]
  })";
  const auto tids = ExtractWctCandidateThreadIds(json, 2);
  assert(tids.size() == 2);
  assert(tids[0] == 3);   // waitTime=300
  assert(tids[1] == 2);   // waitTime=200
}

static void Test_ZeroTid_Skipped()
{
  const std::string json = R"({
    "threads": [
      {"tid": 0, "isCycle": true},
      {"tid": 42, "isCycle": false, "nodes": [{"thread":{"waitTime":10}}]}
    ]
  })";
  const auto tids = ExtractWctCandidateThreadIds(json, 8);
  assert(tids.size() == 1);
  assert(tids[0] == 42);
}

static void Test_StableContextSwitchesAcrossPasses()
{
  const std::string json = R"({
    "passes": [
      {"capture_usable":true,"threads":[
        {"tid":10,"nodes":[{"thread":{"threadId":10,"contextSwitches":100}}]},
        {"tid":20,"nodes":[{"thread":{"threadId":20,"contextSwitches":200}}]},
        {"tid":30,"nodes":[{"thread":{"threadId":30,"contextSwitches":300}}]}
      ]},
      {"capture_usable":true,"threads":[
        {"tid":10,"nodes":[{"thread":{"threadId":10,"contextSwitches":100}}]},
        {"tid":20,"nodes":[{"thread":{"threadId":20,"contextSwitches":201}}]},
        {"tid":30,"nodes":[{"thread":{"threadId":30,"contextSwitches":300}}]}
      ]}
    ]
  })";
  assert(CountWctThreadsWithStableContextSwitches(json, { 10u, 20u, 30u, 40u }) == 2u);
  assert(CountWctThreadsWithStableContextSwitches(R"({"passes":[]})", { 10u }) == 0u);
  assert(CountWctThreadsWithStableContextSwitches(
    R"({"passes":[{"capture_usable":false,"threads":[]},{"capture_usable":true,"threads":[]}]})",
    { 10u }) == 0u);
}

// ── TryParseWctCaptureDecision ─────────────────────────

static void Test_Capture_EmptyInput()
{
  const auto r = TryParseWctCaptureDecision("");
  assert(!r.has_value());
}

static void Test_Capture_NoCaptureKey()
{
  const auto r = TryParseWctCaptureDecision(R"({"threads":[]})");
  assert(!r.has_value());
}

static void Test_Capture_ValidCapture()
{
  const std::string json = R"({
    "capture": {
      "kind": "hang",
      "secondsSinceHeartbeat": 15.5,
      "thresholdSec": 10,
      "isLoading": true
    }
  })";
  const auto r = TryParseWctCaptureDecision(json);
  assert(r.has_value());
  assert(r->has == true);
  assert(r->kind == "hang");
  assert(r->secondsSinceHeartbeat > 15.0 && r->secondsSinceHeartbeat < 16.0);
  assert(r->thresholdSec == 10);
  assert(r->isLoading == true);
}

static void Test_Capture_DefaultValues()
{
  const std::string json = R"({"capture": {}})";
  const auto r = TryParseWctCaptureDecision(json);
  assert(r.has_value());
  assert(r->has == true);
  assert(r->kind.empty());
  assert(r->secondsSinceHeartbeat == 0.0);
  assert(r->thresholdSec == 0);
  assert(r->isLoading == false);
}

static void Test_FreezeSummary_ConsensusFields()
{
  const std::string json = R"({
    "capture_passes": 2,
    "cycle_consensus": true,
    "repeated_cycle_tids": [200, 300],
    "consistent_loading_signal": true,
    "longest_wait_tid_consensus": true,
    "threads": [
      {"tid": 200, "isCycle": true},
      {"tid": 300, "isCycle": true},
      {"tid": 400, "isCycle": false, "nodes": [{"thread":{"waitTime":900}}]}
    ],
    "capture": {
      "kind": "hang",
      "secondsSinceHeartbeat": 15.5,
      "thresholdSec": 10,
      "isLoading": true
    }
  })";

  const auto r = TryParseWctFreezeSummary(json);
  assert(r.has_value());
  assert(r->has == true);
  assert(r->capture_passes == 2);
  assert(r->cycle_consensus == true);
  assert(r->repeated_cycle_tids.size() == 2);
  assert(r->repeated_cycle_tids[0] == 200);
  assert(r->repeated_cycle_tids[1] == 300);
  assert(r->consistent_loading_signal == true);
  assert(r->longest_wait_tid_consensus == true);
}

// ── Longest waiter (SkyrimDiagWctWait.h) ───────────────

static void Test_LongestWaiter_EarliestBlockedWaitStartWins()
{
  skydiag::WctLongestWaiter waiter;
  assert(waiter.tid() == 0u);
  waiter.Observe(10u, skydiag::kWctObjectStatusBlocked, 5002000u);
  waiter.Observe(20u, skydiag::kWctObjectStatusBlocked, 5001200u);
  waiter.Observe(30u, 2u /*running*/, 5000000u);
  waiter.Observe(40u, skydiag::kWctObjectStatusBlocked, 0u);
  waiter.Observe(0u, skydiag::kWctObjectStatusBlocked, 1u);
  waiter.Observe(20u, skydiag::kWctObjectStatusBlocked, 5001200u);  // the same thread again is no tie
  assert(waiter.tid() == 20u);
  assert(waiter.wait_start_tick() == 5001200u);

  // A second blocked thread at the same tick (a process-wide suspend restarts
  // every wait at once): no single longest waiter.
  waiter.Observe(50u, skydiag::kWctObjectStatusBlocked, 5001200u);
  assert(waiter.tid() == 0u);
  assert(waiter.wait_start_tick() == 0u);

  // An earlier wait start still settles it.
  waiter.Observe(60u, skydiag::kWctObjectStatusBlocked, 5000300u);
  assert(waiter.tid() == 60u);
  assert(waiter.wait_start_tick() == 5000300u);
}

// Shaped like the 2026-10-18 hang: the game main thread waits on a critical
// section whose owner was suspended first, a worker has just started a new
// wait, and a running thread carries a tick older than all of them.
static void Test_FreezeSummary_LongestWaiterIsEarliestBlockedThread()
{
  const std::string json = R"({
    "threads": [
      {"tid": 100, "isCycle": false, "nodes": [
        {"objectType":8,"objectStatus":3,"thread":{"threadId":100,"waitTime":5001200}},
        {"objectType":1,"objectStatus":6,"objectName":""},
        {"objectType":8,"objectStatus":3,"thread":{"threadId":200,"waitTime":5001180}}]},
      {"tid": 200, "isCycle": false, "nodes": [{"objectType":8,"objectStatus":3,"thread":{"threadId":200,"waitTime":5001180}}]},
      {"tid": 300, "isCycle": false, "nodes": [{"objectType":8,"objectStatus":3,"thread":{"threadId":300,"waitTime":5002000}}]},
      {"tid": 400, "isCycle": false, "nodes": [{"objectType":8,"objectStatus":2,"thread":{"threadId":400,"waitTime":5000300}}]}
    ]
  })";
  const auto r = TryParseWctFreezeSummary(json);
  assert(r.has_value());
  assert(r->longest_wait_tid == 200u);  // not 300, which only just started waiting
  assert(r->longest_wait_start_tick == 5001180u);
}

static void Test_FreezeSummary_TiedEarliestWaitStartHasNoLongestWaiter()
{
  const std::string json = R"({
    "threads": [
      {"tid": 100, "isCycle": false, "nodes": [{"objectStatus":3,"thread":{"threadId":100,"waitTime":5000300}}]},
      {"tid": 200, "isCycle": false, "nodes": [{"objectStatus":3,"thread":{"threadId":200,"waitTime":5000300}}]},
      {"tid": 300, "isCycle": false, "nodes": [{"objectStatus":3,"thread":{"threadId":300,"waitTime":5002000}}]}
    ]
  })";
  const auto r = TryParseWctFreezeSummary(json);
  assert(r.has_value());
  assert(r->longest_wait_tid == 0u);
  assert(r->longest_wait_start_tick == 0u);
}

// Helpers before the earliest-wait rule wrote longest_wait_tid_consensus for
// the thread with the LATEST wait start; the parser recomputes it from the two
// recorded passes and only falls back to the flag when there are none.
static void Test_FreezeSummary_LongestWaitConsensusRecomputedFromPasses()
{
  const auto capture = [](const char* flag, const char* secondPassWorkerTick) {
    return std::string(R"({"longest_wait_tid_consensus":)") + flag + R"(,"threads":[],"passes":[
      {"capture_usable":true,"threads":[
        {"tid":100,"nodes":[{"objectStatus":3,"thread":{"threadId":100,"waitTime":5001200}}]},
        {"tid":300,"nodes":[{"objectStatus":3,"thread":{"threadId":300,"waitTime":5002000}}]}]},
      {"capture_usable":true,"threads":[
        {"tid":100,"nodes":[{"objectStatus":3,"thread":{"threadId":100,"waitTime":5001200}}]},
        {"tid":300,"nodes":[{"objectStatus":3,"thread":{"threadId":300,"waitTime":)" +
      secondPassWorkerTick + R"(}}]}]}]})";
  };

  // The old flag named the worker (latest start, 5002000 in both passes); the
  // earliest waiter 100 is the same in both passes too.
  auto r = TryParseWctFreezeSummary(capture("true", "5002100"));
  assert(r.has_value() && r->longest_wait_tid_consensus);
  r = TryParseWctFreezeSummary(capture("false", "5002100"));
  assert(r.has_value() && r->longest_wait_tid_consensus);

  // In the second pass the worker ties the main thread: no single longest
  // waiter there, whatever the old flag said.
  r = TryParseWctFreezeSummary(capture("true", "5001200"));
  assert(r.has_value() && !r->longest_wait_tid_consensus);

  // A pass the helper could not read gives no consensus.
  r = TryParseWctFreezeSummary(
    R"({"longest_wait_tid_consensus":true,"passes":[{"capture_usable":false,"threads":[]},{"capture_usable":true,"threads":[]}]})");
  assert(r.has_value() && !r->longest_wait_tid_consensus);
}

static void Test_HelperSource_PreservesPassesAndAvoidsLoaderHeuristic()
{
  const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto helperSource = repoRoot / "helper" / "src" / "WctCapture.cpp";
  std::ifstream in(helperSource);
  assert(in.is_open());

  std::ostringstream buffer;
  buffer << in.rdbuf();
  const std::string source = buffer.str();

  if (source.find("out[\"passes\"]") == std::string::npos) {
    std::abort();
  }
  if (source.find("\"threads\", secondPass.threads") == std::string::npos) {
    std::abort();
  }
  if (source.find("\"capture_usable\"") == std::string::npos) {
    std::abort();
  }
  if (source.find("ReadLoadingSignal(captureStateFlags)") == std::string::npos) {
    std::abort();
  }
  if (source.find("usablePassCount") == std::string::npos) {
    std::abort();
  }
  if (source.find("primaryPassIndex") == std::string::npos) {
    std::abort();
  }
  if (source.find("ContainsLoaderKeyword") != std::string::npos) {
    std::abort();
  }
  if (source.find("find(\"loader\")") != std::string::npos) {
    std::abort();
  }
  // The helper's longest waiter follows the same earliest-wait-start rule the
  // parser applies (the selection itself is tested above).
  if (source.find("skydiag::WctLongestWaiter") == std::string::npos ||
      source.find("longestWaiter.Observe(") == std::string::npos) {
    std::abort();
  }
  if (source.find("longest_wait_ms") != std::string::npos) {
    std::abort();
  }
}

int main()
{
  Test_EmptyInput_ReturnsEmpty();
  Test_InvalidJson_ReturnsEmpty();
  Test_NoThreadsKey_ReturnsEmpty();
  Test_CycleThreadsPrioritized();
  Test_NoCycle_SortedByWaitTime();
  Test_MaxN_Limit();
  Test_ZeroTid_Skipped();
  Test_StableContextSwitchesAcrossPasses();

  Test_Capture_EmptyInput();
  Test_Capture_NoCaptureKey();
  Test_Capture_ValidCapture();
  Test_Capture_DefaultValues();
  Test_FreezeSummary_ConsensusFields();
  Test_LongestWaiter_EarliestBlockedWaitStartWins();
  Test_FreezeSummary_LongestWaiterIsEarliestBlockedThread();
  Test_FreezeSummary_TiedEarliestWaitStartHasNoLongestWaiter();
  Test_FreezeSummary_LongestWaitConsensusRecomputedFromPasses();
  Test_HelperSource_PreservesPassesAndAvoidsLoaderHeuristic();

  return 0;
}
