#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>

#include "Analyzer.h"
#include "HangCaptureInternal.h"
#include "HelperLog.h"
#include "HelperRuntimeTestUtils.h"
#include "RetentionWorker.h"

using skydiag::helper::HangDecision;
using skydiag::helper::internal::ClearLog;
using skydiag::helper::internal::ExecuteConfirmedHangCapture;
using skydiag::helper::internal::HangCaptureState;
using skydiag::helper::internal::ShutdownRetentionWorker;
using skydiag::tests::runtime::CloseAttachedProcess;
using skydiag::tests::runtime::FindSingleFileWithExt;
using skydiag::tests::runtime::MakeSelfAttachedProcess;
using skydiag::tests::runtime::MakeSharedLayout;
using skydiag::tests::runtime::MakeTempDir;
using skydiag::tests::runtime::MakeTestConfig;
using skydiag::tests::runtime::Require;

namespace {

HANDLE g_release = nullptr;
HANDLE g_parked = nullptr;
std::atomic<int> g_sink{ 0 };

// noinline plus work after each call keeps every function as a real frame
// (no inlining, no tail call) in the RelWithDebInfo build.
__declspec(noinline) void DeepFrameC()
{
  SetEvent(g_parked);
  WaitForSingleObject(g_release, 30000);
  g_sink.fetch_add(3);
}

__declspec(noinline) void DeepFrameB()
{
  DeepFrameC();
  g_sink.fetch_add(2);
}

__declspec(noinline) void DeepFrameA()
{
  DeepFrameB();
  g_sink.fetch_add(1);
}

std::size_t FindFrame(const std::vector<std::wstring>& frames, const wchar_t* name)
{
  for (std::size_t i = 0; i < frames.size(); ++i) {
    if (frames[i].find(name) != std::wstring::npos) {
      return i;
    }
  }
  return frames.size();
}

// A dump of a thread parked three calls deep must unwind through every caller
// rather than stopping at the wait syscall or guessing stack slots.
void TestFormalStackwalkUnwindsRealCallerChain()
{
  const auto outBase = MakeTempDir(L"skydiag_stackwalk_e2e");
  ClearLog(outBase);

  g_release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_parked = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Require(g_release && g_parked, "CreateEventW failed");

  DWORD parkedTid = 0;
  std::thread parked([&]() {
    parkedTid = GetCurrentThreadId();
    DeepFrameA();
  });
  Require(WaitForSingleObject(g_parked, 5000) == WAIT_OBJECT_0, "Worker never reached the wait");
  Sleep(100);

  // The blackbox names the parked thread as the game main thread.
  auto shared = MakeSharedLayout();
  shared->header.last_heartbeat_qpc = 1;
  auto& heartbeat = shared->events[0];
  heartbeat.tid = parkedTid;
  heartbeat.qpc = 2;
  heartbeat.type = static_cast<std::uint16_t>(skydiag::EventType::kHeartbeat);
  heartbeat.seq = 2;
  shared->header.write_index = 1;

  auto proc = MakeSelfAttachedProcess(shared.get());
  HangDecision decision{};
  decision.isHang = true;
  decision.secondsSinceHeartbeat = 30.0;
  decision.thresholdSec = 10;
  std::wstring pendingViewer;
  HangCaptureState state{};
  ExecuteConfirmedHangCapture(MakeTestConfig(), proc, outBase, decision, 0u, &pendingViewer, &state);

  SetEvent(g_release);
  parked.join();
  ShutdownRetentionWorker();
  CloseAttachedProcess(&proc);
  CloseHandle(g_release);
  CloseHandle(g_parked);

  const auto dumpPath = FindSingleFileWithExt(outBase, L".dmp");
  Require(std::filesystem::exists(dumpPath), "Hang capture must write a dump");

  skydiag::dump_tool::AnalyzeOptions opt{};
  opt.language = skydiag::dump_tool::i18n::Language::kEnglish;
  opt.output_dir = outBase.wstring();
  skydiag::dump_tool::AnalysisResult result{};
  std::wstring err;
  Require(
    skydiag::dump_tool::AnalyzeDump(dumpPath.wstring(), outBase.wstring(), opt, result, &err),
    "AnalyzeDump failed on the stackwalk dump");

  Require(result.stackwalk_primary_tid == parkedTid, "The main thread must be the walked thread");
  Require(result.suspects_from_stackwalk, "Suspects must come from the formal stackwalk, not pointer scan");
  Require(result.stackwalk_total_frames >= 5u, "Formal stackwalk must unwind past the wait syscall");

  const auto& frames = result.stackwalk_primary_frames;
  const auto c = FindFrame(frames, L"DeepFrameC");
  const auto b = FindFrame(frames, L"DeepFrameB");
  const auto a = FindFrame(frames, L"DeepFrameA");
  Require(c < frames.size() && b < frames.size() && a < frames.size(), "Every caller in the chain must appear");
  Require(c < b && b < a, "Callers must appear innermost first");
  for (const auto& frame : frames) {
    Require(frame.rfind(L"0x", 0) != 0, "Walked frames must all belong to a loaded module");
  }

  std::filesystem::remove_all(outBase);
}

}  // namespace

int main()
{
  try {
    TestFormalStackwalkUnwindsRealCallerChain();
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
