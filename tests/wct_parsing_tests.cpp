#include "WctTypes.h"
#include "SkyrimDiagWctWait.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

// engine_spin_nan: the 2026-10-22 freeze's game thread 19588 was Running
// (objectStatus 2) in both passes with contextSwitches 35263 both times.
static void Test_RunningWithoutContextSwitch()
{
  const auto node = [](int tid, int status, int switches) {
    return R"({"tid":)" + std::to_string(tid) + R"(,"isCycle":false,"nodes":[{"objectStatus":)" +
      std::to_string(status) + R"(,"objectType":8,"thread":{"contextSwitches":)" + std::to_string(switches) +
      R"(,"processId":45008,"threadId":)" + std::to_string(tid) + R"(,"waitTime":8707855}}]})";
  };
  const auto pass = [](bool usable, const std::string& threads) {
    return std::string(R"({"capture_usable":)") + (usable ? "true" : "false") + R"(,"threads":[)" + threads + "]}";
  };
  const std::string first = node(19588, 2, 35263) + "," + node(12860, 2, 100) + "," + node(48060, 3, 173618) + "," +
    node(7, 2, 5);
  const std::string last = node(19588, 2, 35263) + "," + node(12860, 2, 101) + "," + node(48060, 3, 173618) + "," +
    node(7, 3, 5);
  const std::string json = R"({"passes":[)" + pass(true, first) + "," + pass(true, last) + "]}";
  using skydiag::dump_tool::internal::FindWctThreadsRunningWithoutContextSwitch;
  // 12860 switched, 48060 was blocked (a waiting thread does not switch
  // either), 7 blocked in the last pass, 99 is not in WCT.
  const auto running = FindWctThreadsRunningWithoutContextSwitch(json, { 48060u, 12860u, 19588u, 7u, 99u, 19588u });
  assert((running == std::vector<std::uint32_t>{ 19588u }));
  // One pass, an unusable pass, no input.
  assert(FindWctThreadsRunningWithoutContextSwitch(R"({"passes":[)" + pass(true, first) + "]}", { 19588u }).empty());
  assert(FindWctThreadsRunningWithoutContextSwitch(
    R"({"passes":[)" + pass(false, first) + "," + pass(true, last) + "]}", { 19588u }).empty());
  assert(FindWctThreadsRunningWithoutContextSwitch("", { 19588u }).empty());
  assert(FindWctThreadsRunningWithoutContextSwitch("{bad json", { 19588u }).empty());
  assert(FindWctThreadsRunningWithoutContextSwitch(json, {}).empty());
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

// ── FindWctLockOwner ────────────────────────────────────

// One pass's threads: the main thread (100) waits on a lock of the given type
// and status owned by ownerTid (0: no owner node), and the owner's own entry
// is blocked with no wait object.
static std::string LockChainThreads(std::uint32_t ownerTid, std::uint32_t objectType = 1, std::uint32_t status = 6,
  std::uint64_t ownerSwitches = 6634)
{
  std::string chain =
    R"({"tid":100,"isCycle":false,"nodes":[)"
    R"({"objectStatus":3,"objectType":8,"thread":{"contextSwitches":181004,"threadId":100,"waitTime":7753219}},)"
    R"({"objectName":"","objectStatus":)" + std::to_string(status) + R"(,"objectType":)" + std::to_string(objectType) + "}";
  if (ownerTid != 0u) {
    chain += R"(,{"objectStatus":3,"objectType":8,"thread":{"contextSwitches":)" + std::to_string(ownerSwitches) +
             R"(,"threadId":)" + std::to_string(ownerTid) + R"(,"waitTime":7753219}})";
  }
  chain += "]}";
  std::string threads = "[" + chain;
  if (ownerTid != 0u) {
    threads += R"(,{"tid":)" + std::to_string(ownerTid) +
               R"(,"isCycle":false,"nodes":[{"objectStatus":3,"objectType":8,"thread":{"contextSwitches":)" +
               std::to_string(ownerSwitches) + R"(,"threadId":)" + std::to_string(ownerTid) + R"(,"waitTime":7753219}}]})";
  }
  threads += "]";
  return threads;
}

static std::string TwoPassCapture(const std::string& pass0, const std::string& pass1)
{
  return R"({"capture_passes":2,"threads":)" + pass0 + R"(,"passes":[{"pass_index":0,"capture_usable":true,"threads":)" +
         pass0 + R"(},{"pass_index":1,"capture_usable":true,"threads":)" + pass1 + "}]}";
}

static void Test_LockOwner_CriticalSectionChainConfirmedByBothPasses()
{
  const auto json = TwoPassCapture(LockChainThreads(51944), LockChainThreads(51944));
  const auto owner = skydiag::dump_tool::internal::FindWctLockOwner(json, 100);
  assert(owner.has_value());
  assert(owner->owner_tid == 51944u);
  assert(owner->object_type == skydiag::dump_tool::internal::kWctObjectTypeCriticalSection);
  assert(owner->confirmed);
  assert(owner->owner_blocked_without_wait_object);
  assert(owner->owner_context_switches_stable);

  // A mutex owner counts the same way.
  const auto mutexJson = TwoPassCapture(LockChainThreads(77, 3), LockChainThreads(77, 3));
  const auto mutexOwner = skydiag::dump_tool::internal::FindWctLockOwner(mutexJson, 100);
  assert(mutexOwner.has_value() && mutexOwner->owner_tid == 77u && mutexOwner->confirmed);
}

static void Test_LockOwner_PassesDisagreeStayUnconfirmed()
{
  // The second pass names another owner; the owner also kept switching.
  const auto json = TwoPassCapture(LockChainThreads(51944), LockChainThreads(4242));
  const auto owner = skydiag::dump_tool::internal::FindWctLockOwner(json, 100);
  assert(owner.has_value());
  assert(owner->owner_tid == 51944u);
  assert(!owner->confirmed);

  const auto moving = TwoPassCapture(LockChainThreads(51944, 1, 6, 10), LockChainThreads(51944, 1, 6, 11));
  const auto movingOwner = skydiag::dump_tool::internal::FindWctLockOwner(moving, 100);
  assert(movingOwner.has_value() && movingOwner->confirmed && !movingOwner->owner_context_switches_stable);

  // A capture without passes (older helpers) has nothing to confirm with.
  const auto single = R"({"threads":)" + LockChainThreads(51944) + "}";
  const auto singleOwner = skydiag::dump_tool::internal::FindWctLockOwner(single, 100);
  assert(singleOwner.has_value() && singleOwner->owner_tid == 51944u && !singleOwner->confirmed);
}

static void Test_LockOwner_UnknownOwnerOrOtherChainsNameNoOne()
{
  // The lock is reported owned, but WCT could not name the owner.
  const auto unknown = TwoPassCapture(LockChainThreads(0), LockChainThreads(0));
  const auto owner = skydiag::dump_tool::internal::FindWctLockOwner(unknown, 100);
  assert(owner.has_value());
  assert(owner->owner_tid == 0u);
  assert(!owner->confirmed);

  // A SendMessage wait (type 2) or a lock not owned (status 7) is no lock owner.
  assert(!skydiag::dump_tool::internal::FindWctLockOwner(TwoPassCapture(LockChainThreads(5, 2), LockChainThreads(5, 2)), 100));
  assert(!skydiag::dump_tool::internal::FindWctLockOwner(TwoPassCapture(LockChainThreads(5, 1, 7), LockChainThreads(5, 1, 7)), 100));
  // Another waiter, no JSON, a cycle chain.
  assert(!skydiag::dump_tool::internal::FindWctLockOwner(TwoPassCapture(LockChainThreads(5), LockChainThreads(5)), 200));
  assert(!skydiag::dump_tool::internal::FindWctLockOwner("{bad json", 100));
  std::string cycle = LockChainThreads(5);
  cycle.replace(cycle.find(R"("isCycle":false)"), std::string(R"("isCycle":false)").size(), R"("isCycle":true)");
  assert(!skydiag::dump_tool::internal::FindWctLockOwner(TwoPassCapture(cycle, cycle), 100));

  // An owner that waits on something itself is not "blocked with no wait object".
  std::string waiting = LockChainThreads(5);
  const std::string ownerEntryEnd = R"("waitTime":7753219}}]})";
  const auto last = waiting.rfind(ownerEntryEnd);
  waiting.replace(last, ownerEntryEnd.size(),
    R"("waitTime":7753219}},{"objectName":"","objectStatus":3,"objectType":1}]})");
  const auto waitingOwner = skydiag::dump_tool::internal::FindWctLockOwner(TwoPassCapture(waiting, waiting), 100);
  assert(waitingOwner.has_value() && waitingOwner->owner_tid == 5u && !waitingOwner->owner_blocked_without_wait_object);
}

static std::string ThreadNode(std::uint32_t tid, std::uint32_t pid)
{
  return R"({"objectStatus":3,"objectType":8,"thread":{"contextSwitches":6634,"processId":)" + std::to_string(pid) +
         R"(,"threadId":)" + std::to_string(tid) + R"(,"waitTime":7753219}})";
}

static std::string OwnEntry(std::uint32_t tid, std::uint32_t pid)
{
  return R"({"tid":)" + std::to_string(tid) + R"(,"isCycle":false,"nodes":[)" + ThreadNode(tid, pid) + "]}";
}

static void Test_LockOwner_FollowsTheChainToItsLastThread()
{
  // main (100) -> critical section -> A (200) -> critical section -> B (300):
  // A only waits too; B, blocked with nothing to wait on, is the owner.
  const std::string lock = R"({"objectName":"","objectStatus":6,"objectType":1})";
  const std::string threads = "[" + std::string(R"({"tid":100,"isCycle":false,"nodes":[)") + ThreadNode(100, 49416) + "," +
    lock + "," + ThreadNode(200, 49416) + "," + lock + "," + ThreadNode(300, 49416) + "]}," +
    R"({"tid":200,"isCycle":false,"nodes":[)" + ThreadNode(200, 49416) + "," + lock + "," + ThreadNode(300, 49416) + "]}," +
    OwnEntry(300, 49416) + "]";
  const auto owner = skydiag::dump_tool::internal::FindWctLockOwner(TwoPassCapture(threads, threads), 100);
  assert(owner.has_value() && owner->owner_tid == 300u && owner->confirmed);
  assert(owner->owner_blocked_without_wait_object);
}

static void Test_LockOwner_InAnotherProcessIsNoOwner()
{
  // A named mutex held by a thread of another process (an overlay).
  const std::string mutex = R"({"objectName":"Overlay","objectStatus":6,"objectType":3})";
  const std::string direct = "[" + std::string(R"({"tid":100,"isCycle":false,"nodes":[)") + ThreadNode(100, 49416) + "," +
    mutex + "," + ThreadNode(900, 1234) + "]}]";
  const auto outside = skydiag::dump_tool::internal::FindWctLockOwner(TwoPassCapture(direct, direct), 100);
  assert(outside.has_value() && outside->owner_tid == 0u && !outside->confirmed);

  // The owner in the game waits on another process: it stays the owner.
  const std::string lock = R"({"objectName":"","objectStatus":6,"objectType":1})";
  const std::string chained = "[" + std::string(R"({"tid":100,"isCycle":false,"nodes":[)") + ThreadNode(100, 49416) +
    "," + lock + "," + ThreadNode(200, 49416) + "," + mutex + "," + ThreadNode(900, 1234) + "]}," +
    OwnEntry(200, 49416) + "]";
  const auto inGame = skydiag::dump_tool::internal::FindWctLockOwner(TwoPassCapture(chained, chained), 100);
  assert(inGame.has_value() && inGame->owner_tid == 200u && inGame->confirmed);
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
  Test_RunningWithoutContextSwitch();

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
  Test_LockOwner_CriticalSectionChainConfirmedByBothPasses();
  Test_LockOwner_PassesDisagreeStayUnconfirmed();
  Test_LockOwner_UnknownOwnerOrOtherChainsNameNoOne();
  Test_LockOwner_FollowsTheChainToItsLastThread();
  Test_LockOwner_InAnotherProcessIsNoOwner();

  return 0;
}
