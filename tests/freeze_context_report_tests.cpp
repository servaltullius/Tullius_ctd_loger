// Freeze reports from the main thread's wait and the blackbox game state
// (ADR-0009), shaped like the user's real freezes: the main thread
// sleep-waiting inside the engine with the Console open, a wait inside the
// NVIDIA driver, and a freeze right after an 84-minute pause.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "Analyzer.h"
#include "AnalyzerInternals.h"
#include "EvidenceBuilder.h"
#include "NextAction.h"
#include "SkyrimDiagShared.h"

namespace {

using skydiag::dump_tool::AnalysisResult;
using skydiag::dump_tool::BuildEvidenceAndSummary;
using skydiag::dump_tool::EventRow;
using skydiag::dump_tool::SelectNextActionIndex;
namespace i18n = skydiag::dump_tool::i18n;

void Require(bool condition, const char* message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

bool Contains(const std::wstring& text, std::wstring_view part)
{
  return text.find(part) != std::wstring::npos;
}

bool AnyStartsWith(const std::vector<std::wstring>& lines, std::wstring_view prefix, std::wstring_view part = L"")
{
  for (const auto& line : lines) {
    if (line.rfind(prefix, 0) == 0 && Contains(line, part)) {
      return true;
    }
  }
  return false;
}

// Menu and module names are packed into b/c/d like the plugin writes them.
EventRow Event(skydiag::EventType type, double tMs, std::string_view name = {}, std::uint64_t a = 0)
{
  EventRow row{};
  row.t_ms = tMs;
  row.type = static_cast<std::uint16_t>(type);
  row.a = a;
  char buf[24]{};
  std::memcpy(buf, name.data(), std::min<std::size_t>(name.size(), 23));
  std::memcpy(&row.b, buf, 8);
  std::memcpy(&row.c, buf + 8, 8);
  std::memcpy(&row.d, buf + 16, 8);
  return row;
}

void TestBlackboxGameState()
{
  using skydiag::EventType;
  const std::vector<EventRow> events = {
    Event(EventType::kLoadEnd, 1000.0),
    Event(EventType::kMenuOpen, 2000.0, "TweenMenu"),
    Event(EventType::kMenuClose, 3000.0, "TweenMenu"),
    Event(EventType::kMenuOpen, 4000.0, "Console"),
    Event(EventType::kMenuOpen, 4000.0, "Cursor Menu"),  // overlay, not a game menu
    Event(EventType::kPerfHitch, 5061700.0, {}, 5057699),  // 84 minutes
    Event(EventType::kPerfHitch, 5068500.0, {}, 6844),
  };
  const auto state = skydiag::dump_tool::internal::BuildBlackboxFreezeSummary(events, false);
  Require(state.open_menus == std::vector<std::wstring>{ L"Console" }, "only game menus still open are listed");
  Require(state.seconds_since_load_end > 5067.0 && state.seconds_since_load_end < 5068.0, "time since the last load end");
  Require(state.pause_gap_seconds > 5057.0 && state.pause_gap_seconds < 5058.0, "a gap of minutes is a pause");
  Require(state.pause_gap_ended_seconds_before > 6.0 && state.pause_gap_ended_seconds_before < 7.0, "when the pause ended");

  const auto quiet = skydiag::dump_tool::internal::BuildBlackboxFreezeSummary(
    { Event(EventType::kPerfHitch, 10000.0, {}, 6488) }, false);
  Require(quiet.pause_gap_seconds == 0.0 && quiet.open_menus.empty() && quiet.seconds_since_load_end < 0.0,
    "a stutter is not a pause and no load means no load time");

  // Quiet play writes no events: 15 minutes after the load the newest event is
  // still the LoadEnd, so times are measured to the last heartbeat.
  const auto quietPlay = skydiag::dump_tool::internal::BuildBlackboxFreezeSummary(
    { Event(EventType::kLoadEnd, 1000.0) }, false, 901000.0);
  Require(quietPlay.seconds_since_load_end > 899.0 && quietPlay.seconds_since_load_end < 901.0,
    "time since the load is measured to the last heartbeat, not the newest event");
  const auto noHeartbeat = skydiag::dump_tool::internal::BuildBlackboxFreezeSummary(
    { Event(EventType::kLoadEnd, 1000.0), Event(EventType::kMenuOpen, 4000.0, "Console") }, false, 2000.0);
  Require(noHeartbeat.seconds_since_load_end > 2.9 && noHeartbeat.seconds_since_load_end < 3.1,
    "an older heartbeat does not pull the reference back");
}

AnalysisResult MakeHang()
{
  AnalysisResult r{};
  r.dump_path = L"C:\\Logs\\SkyrimDiag_Hang_20260926_192448_760.dmp";
  r.freeze_analysis.has_analysis = true;
  r.freeze_analysis.state_id = "freeze_ambiguous";
  skydiag::dump_tool::SuspectItem suspect{};
  suspect.module_filename = L"CommunityShaders.dll";
  suspect.score = 1;
  suspect.confidence_level = i18n::ConfidenceLevel::kLow;
  r.suspects.push_back(suspect);
  r.suspects_from_stackwalk = true;
  r.suspects_from_main_thread = true;
  return r;
}

void TestEngineWaitWithConsoleOpen()
{
  for (const auto lang : { i18n::Language::kEnglish, i18n::Language::kKorean }) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = MakeHang();
    r.main_thread_wait.kind = "engine_wait";
    r.main_thread_wait.wait_class = "sleep";
    r.main_thread_wait.wait_api = L"KERNELBASE.dll!SleepEx";
    r.main_thread_wait.waiting_module = L"SkyrimSE.exe";
    r.main_thread_wait.path_modules = { L"EngineFixes.dll", L"CommunityShaders.dll", L"hdtsmp64.dll" };
    r.blackbox_freeze_summary.open_menus = { L"Console" };
    BuildEvidenceAndSummary(r, lang);

    Require(Contains(r.summary_sentence, en ? L"waiting inside the game engine (SkyrimSE.exe" : L"게임 엔진(SkyrimSE.exe"),
      "the summary says the main thread waited in the engine");
    Require(Contains(r.summary_sentence, en ? L"Open at the time: Console." : L"당시 열려 있던 메뉴: Console."),
      "the summary names the open Console");
    Require(!Contains(r.summary_sentence, en ? L"actionable candidate" : L"실행 우선 후보"),
      "the summary does not name a stack module as the candidate");
    for (const auto& candidate : r.actionable_candidates) {
      Require(candidate.module_filename != L"CommunityShaders.dll", "a waiting main thread's stack makes no candidate");
    }
    const auto next = SelectNextActionIndex(r.recommendations);
    Require(next != std::wstring::npos && r.recommendations[next].rfind(en ? L"[Main thread]" : L"[메인 스레드]", 0) == 0,
      "NextAction explains what the main thread was doing");
    Require(AnyStartsWith(r.recommendations, en ? L"[Context]" : L"[상황]", en ? L"Console" : L"콘솔"),
      "the open Console is pointed out");
    bool hasWaitEvidence = false;
    for (const auto& e : r.evidence) {
      hasWaitEvidence = hasWaitEvidence || Contains(e.title, en ? L"inside the game engine" : L"게임 엔진 안에서");
    }
    Require(hasWaitEvidence, "the wait is listed as evidence");
  }
}

void TestGraphicsDriverWaitAfterPause()
{
  auto r = MakeHang();
  r.main_thread_wait.kind = "graphics_driver_wait";
  r.main_thread_wait.wait_api = L"KERNELBASE.dll!WaitForSingleObjectEx";
  r.main_thread_wait.waiting_module = L"nvwgf2umx.dll";
  r.graphics_env.enb_detected = true;
  r.blackbox_freeze_summary.pause_gap_seconds = 5057.6;
  r.blackbox_freeze_summary.pause_gap_ended_seconds_before = 7.0;
  BuildEvidenceAndSummary(r, i18n::Language::kEnglish);

  Require(Contains(r.summary_sentence, L"waiting inside the graphics driver (nvwgf2umx.dll"), "driver wait in the summary");
  Require(Contains(r.summary_sentence, L"A pause of 84 minutes"), "the pause before the freeze is in the summary");
  Require(AnyStartsWith(r.recommendations, L"[Main thread]", L"(detected: ENB)"), "driver advice names the detected injector");
  Require(AnyStartsWith(r.recommendations, L"[Context]", L"pause of 84 minutes"), "the pause gets its own advice");
}

// The four field freezes on 1.6.1170: the engine polled a Direct3D query in
// a Sleep loop, so the report points at the GPU, not at the stack plugins.
void TestEngineGpuQueryPoll()
{
  for (const auto lang : { i18n::Language::kEnglish, i18n::Language::kKorean }) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = MakeHang();
    r.main_thread_wait.kind = "engine_wait";
    r.main_thread_wait.engine_wait_detail = "gpu_query_poll";
    r.main_thread_wait.wait_class = "sleep";
    r.main_thread_wait.wait_api = L"KERNELBASE.dll!SleepEx";
    r.main_thread_wait.waiting_module = L"SkyrimSE.exe";
    r.main_thread_wait.path_modules = { L"EngineFixes.dll", L"CommunityShaders.dll" };
    r.graphics_env.reshade_detected = true;
    BuildEvidenceAndSummary(r, lang);

    Require(Contains(r.summary_sentence, en ? L"waiting for the GPU" : L"GPU를 기다리고"), "the summary says it waited for the GPU");
    Require(Contains(r.summary_sentence, L"ID3D11DeviceContext::GetData"), "the summary names the query poll");
    Require(!Contains(r.summary_sentence, en ? L"cannot tell what the engine was waiting for" : L"무엇을 기다렸는지는"),
      "a known poll does not say the wait is unknown");
    const auto next = SelectNextActionIndex(r.recommendations);
    Require(next != std::wstring::npos && r.recommendations[next].rfind(en ? L"[Main thread]" : L"[메인 스레드]", 0) == 0 &&
              Contains(r.recommendations[next], en ? L"GPU driver" : L"GPU 드라이버") &&
              Contains(r.recommendations[next], L"ReShade"),
      "NextAction gives the GPU-side advice");
    for (const auto& candidate : r.actionable_candidates) {
      Require(candidate.module_filename != L"CommunityShaders.dll", "the stack plugins make no candidate");
    }
  }
}

// The stack walk picked a WCT cycle thread, not the main thread: the main
// thread's wait says nothing about that thread's modules.
void TestOtherThreadSuspectsKeepTheirSignal()
{
  auto withMain = MakeHang();
  withMain.suspects[0].score = 12;
  withMain.suspects[0].confidence_level = i18n::ConfidenceLevel::kHigh;
  withMain.main_thread_wait.kind = "engine_wait";
  withMain.main_thread_wait.waiting_module = L"SkyrimSE.exe";
  auto withCycleThread = withMain;
  withCycleThread.suspects_from_main_thread = false;
  BuildEvidenceAndSummary(withMain, i18n::Language::kEnglish);
  BuildEvidenceAndSummary(withCycleThread, i18n::Language::kEnglish);

  const auto hasStackSignal = [](const AnalysisResult& r) {
    for (const auto& candidate : r.actionable_candidates) {
      if (candidate.module_filename == L"CommunityShaders.dll") {
        return true;
      }
    }
    return false;
  };
  Require(!hasStackSignal(withMain), "the main thread's own stack makes no candidate");
  Require(hasStackSignal(withCycleThread), "a cycle thread's stack still makes a candidate");
}

// A module-level stall (the same module on the main thread and other stuck
// threads) or a deadlock explains the freeze; the wait advice must not
// contradict it as the first action.
void TestStallOutranksTheWaitAdvice()
{
  for (const auto& stateId : { std::string("synchronization_stall_likely"), std::string("deadlock_likely") }) {
    auto r = MakeHang();
    r.freeze_analysis.state_id = stateId;
    r.main_thread_wait.kind = "engine_wait";
    r.main_thread_wait.waiting_module = L"SkyrimSE.exe";
    r.main_thread_wait.path_modules = { L"CommunityShaders.dll" };
    if (stateId == "synchronization_stall_likely") {
      r.hang_thread_module_consensus.has_consensus = true;
      r.hang_thread_module_consensus.module_filename = L"CommunityShaders.dll";
      r.hang_thread_module_consensus.matching_thread_count = 4u;
    }
    BuildEvidenceAndSummary(r, i18n::Language::kEnglish);
    Require(!AnyStartsWith(r.recommendations, L"[Main thread]"), "no wait advice when a stall explains the freeze");
    const auto next = SelectNextActionIndex(r.recommendations);
    Require(next == std::wstring::npos || r.recommendations[next].rfind(L"[Main thread]", 0) != 0,
      "NextAction is not the wait advice");
  }
}

void TestCrashesAreUntouched()
{
  AnalysisResult r{};
  r.dump_path = L"C:\\Logs\\SkyrimDiag_Crash_20261005_143810_399.dmp";
  r.exc_code = 0xC0000005u;
  r.fault_module_filename = L"SmoothCam.dll";
  r.blackbox_freeze_summary.open_menus = { L"Console" };
  BuildEvidenceAndSummary(r, i18n::Language::kEnglish);
  Require(!Contains(r.summary_sentence, L"Open at the time"), "crash summaries do not gain the freeze context");
  Require(!AnyStartsWith(r.recommendations, L"[Context]"), "crash checklists do not gain the freeze context");
}

}  // namespace

int main()
{
  try {
    TestBlackboxGameState();
    TestEngineWaitWithConsoleOpen();
    TestGraphicsDriverWaitAfterPause();
    TestEngineGpuQueryPoll();
    TestOtherThreadSuspectsKeepTheirSignal();
    TestStallOutranksTheWaitAdvice();
    TestCrashesAreUntouched();
    std::puts("freeze context report tests passed");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
