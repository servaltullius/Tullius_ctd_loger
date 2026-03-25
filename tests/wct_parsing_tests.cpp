#include "WctTypes.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using skydiag::dump_tool::internal::ExtractWctCandidateThreadIds;
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

  Test_Capture_EmptyInput();
  Test_Capture_NoCaptureKey();
  Test_Capture_ValidCapture();
  Test_Capture_DefaultValues();
  Test_FreezeSummary_ConsensusFields();
  Test_HelperSource_PreservesPassesAndAvoidsLoaderHeuristic();

  return 0;
}
