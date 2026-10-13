// Freeze reports from the main thread's wait and the blackbox game state
// (ADR-0009), shaped like the user's real freezes: the main thread
// sleep-waiting inside the engine with the Console open, a wait inside the
// NVIDIA driver, and a freeze right after an 84-minute pause.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "Analyzer.h"
#include "AnalyzerInternals.h"
#include "AnalyzerPipeline.h"
#include "EvidenceBuilder.h"
#include "NextAction.h"
#include "OutputWriterPipeline.h"
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
  r.graphics_env.injection_modules = { L"d3d11.dll", L"d3dcompiler_46e.dll" };
  r.blackbox_freeze_summary.pause_gap_seconds = 5057.6;
  r.blackbox_freeze_summary.pause_gap_ended_seconds_before = 7.0;
  BuildEvidenceAndSummary(r, i18n::Language::kEnglish);

  Require(Contains(r.summary_sentence, L"waiting inside the graphics driver (nvwgf2umx.dll"), "driver wait in the summary");
  Require(Contains(r.summary_sentence, L"A pause of 84 minutes"), "the pause before the freeze is in the summary");
  Require(AnyStartsWith(r.recommendations, L"[Main thread]", L"(detected: d3d11.dll, d3dcompiler_46e.dll)"), "driver advice names the injector DLLs found");
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
    r.graphics_env.injection_modules = { L"dxgi.dll" };
    BuildEvidenceAndSummary(r, lang);

    Require(Contains(r.summary_sentence, en ? L"waiting for the GPU" : L"GPU를 기다리고"), "the summary says it waited for the GPU");
    Require(Contains(r.summary_sentence, L"ID3D11DeviceContext::GetData"), "the summary names the query poll");
    Require(!Contains(r.summary_sentence, en ? L"cannot tell what the engine was waiting for" : L"무엇을 기다렸는지는"),
      "a known poll does not say the wait is unknown");
    const auto next = SelectNextActionIndex(r.recommendations);
    Require(next != std::wstring::npos && r.recommendations[next].rfind(en ? L"[Main thread]" : L"[메인 스레드]", 0) == 0 &&
              Contains(r.recommendations[next], en ? L"GPU driver" : L"GPU 드라이버") &&
              Contains(r.recommendations[next], L"dxgi.dll"),
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

// The report keeps an overview and the most recent events; the full ring
// buffer is in the blackbox companion file.
void TestReportKeepsRecentEvents()
{
  AnalysisResult r = MakeHang();
  r.has_blackbox = true;
  for (std::uint32_t i = 0; i < 200; ++i) {
    auto row = Event(i % 2 == 0 ? skydiag::EventType::kMenuOpen : skydiag::EventType::kMenuClose, 5061700.0 + i);
    row.i = i;
    row.type_name = i % 2 == 0 ? L"MenuOpen" : L"MenuClose";
    r.events.push_back(row);
  }
  BuildEvidenceAndSummary(r, i18n::Language::kEnglish);
  const auto summary = skydiag::dump_tool::BuildSummaryJson(r, std::filesystem::temp_directory_path(), L"stem", false);
  const auto report = skydiag::dump_tool::BuildReportText(r, summary, false);
  Require(report.find("Events: 200 over 0.2 s (MenuOpen 100, MenuClose 100)") != std::string::npos, "event overview");
  Require(report.find("(last 60 of 200; all of them are in *_SkyrimDiagBlackbox.jsonl)") != std::string::npos,
    "the report says where the rest is");
  Require(report.find("[139] ") == std::string::npos && report.find("[140] ") != std::string::npos &&
            report.find("[199] ") != std::string::npos,
    "only the last 60 events are listed");
  Require(report.find("t_ms=5061899.0 ") != std::string::npos && report.find("e+06") == std::string::npos,
    "times are never in scientific notation");
}

// Captures of the same freeze land in one bucket even when the stack below
// the wait differs, and a different wait is a different bucket.
void TestFreezesGroupByTheMainThreadWait()
{
  const auto gpuPoll = [](std::uint64_t deepFrameOffset) {
    auto r = MakeHang();
    r.main_thread_wait.kind = "engine_wait";
    r.main_thread_wait.engine_wait_detail = "gpu_query_poll";
    r.main_thread_wait.wait_api = L"KERNELBASE.dll!SleepEx";
    r.main_thread_wait.waiting_module = L"SkyrimSE.exe";
    r.stackwalk_primary_frames = { L"frame" };
    r.stackwalk_primary_bucket_frames = { { L"SkyrimSE.exe", 0xE46E21 }, { L"CommunityShaders.dll", deepFrameOffset } };
    skydiag::dump_tool::internal::ComputeCrashBucket(r);
    return r.crash_bucket_key;
  };
  const auto first = gpuPoll(0x1000);
  Require(first.rfind(L"HANG1-", 0) == 0, "a freeze with a classified wait gets a wait bucket");
  Require(gpuPoll(0x2000) == first, "the stack below the wait does not split the bucket");

  auto driver = MakeHang();
  driver.main_thread_wait.kind = "graphics_driver_wait";
  driver.main_thread_wait.wait_api = L"KERNELBASE.dll!WaitForSingleObjectEx";
  driver.main_thread_wait.waiting_module = L"nvwgf2umx.dll";
  skydiag::dump_tool::internal::ComputeCrashBucket(driver);
  Require(driver.crash_bucket_key.rfind(L"HANG1-", 0) == 0 && driver.crash_bucket_key != first,
    "a different wait is a different bucket");

  auto running = MakeHang();
  running.main_thread_wait.kind = "running";
  running.stackwalk_primary_frames = { L"frame" };
  running.stackwalk_primary_bucket_frames = { { L"SomePlugin.dll", 0x10 } };
  skydiag::dump_tool::internal::ComputeCrashBucket(running);
  Require(running.crash_bucket_key.rfind(L"CTD3-", 0) == 0, "a running main thread keeps the stack bucket");

  AnalysisResult crash{};
  crash.exc_code = 0xC0000005u;
  crash.fault_module_filename = L"SmoothCam.dll";
  crash.fault_module_offset = 0x15FCC;
  skydiag::dump_tool::internal::ComputeCrashBucket(crash);
  Require(crash.crash_bucket_key.rfind(L"CTD3-", 0) == 0, "crashes keep the stack bucket");
}

// Plugins on a waiting main thread's stack are bystanders; recording them as
// the bucket's candidates would let every repeat of the freeze vote for them.
void TestBystanderStackIsNotRecordedInHistory()
{
  auto wait = MakeHang();
  wait.main_thread_wait.kind = "engine_wait";
  wait.main_thread_wait.waiting_module = L"SkyrimSE.exe";
  Require(skydiag::dump_tool::CollectHistoryCandidateKeys(wait).empty(), "bystander suspects are not history candidates");

  auto running = MakeHang();
  running.main_thread_wait.kind = "running";
  const auto keys = skydiag::dump_tool::CollectHistoryCandidateKeys(running);
  Require(!keys.empty(), "a running main thread still records its stack suspects");
}

// The summary keeps plugin counts, not the ~1 MB per-plugin list that the
// analysis reads from the dump itself.
void TestSummaryKeepsPluginCountsOnly()
{
  AnalysisResult r = MakeHang();
  r.has_plugin_scan = true;
  r.plugin_scan_json_utf8 = R"({"game_exe_version":"1.6.1170.0","plugins_source":"mo2_profile","plugins":[
    {"filename":"Skyrim.esm","is_active":true,"is_esl":false,"masters":[]},
    {"filename":"A.esl","is_active":true,"is_esl":true,"masters":["Skyrim.esm"]},
    {"filename":"B.esp","is_active":false,"is_esl":false,"masters":["Skyrim.esm"]}],
    "skse_log":{"status":"matched"}})";
  const auto summary = skydiag::dump_tool::BuildSummaryJson(r, std::filesystem::temp_directory_path(), L"stem", false);
  const auto& scan = summary["plugin_scan"];
  Require(!scan.contains("plugins"), "the per-plugin list is left out");
  Require(scan["plugin_count"] == 3 && scan["active_plugin_count"] == 2 && scan["light_plugin_count"] == 1, "counts");
  Require(scan["game_exe_version"] == "1.6.1170.0" && scan["skse_log"]["status"] == "matched", "other fields stay");

  r.plugin_scan_json_utf8 = std::string(10000, '{');
  const auto broken = skydiag::dump_tool::BuildSummaryJson(r, std::filesystem::temp_directory_path(), L"stem", false);
  Require(broken["plugin_scan"].is_null() && broken["plugin_scan_raw"].get<std::string>().size() == 4096u,
    "an unparsable scan keeps only a preview");
}

// Advice that does not fit the capture is left out: stutter advice in a crash
// report, "fault module not found" for a freeze, freeze-state advice for a
// manual capture of a running game, and load-order upkeep next to a dialog
// that already explains the freeze.
void TestAdviceFitsTheCapture()
{
  const auto hitch = [] {
    auto row = Event(skydiag::EventType::kPerfHitch, 1000.0, {}, 600);
    row.type_name = L"PerfHitch";
    return row;
  };

  AnalysisResult crash{};
  crash.dump_path = L"C:\\Logs\\SkyrimDiag_Crash_20261007_123742_440.dmp";
  crash.exc_code = 0xC0000005u;
  crash.has_blackbox = true;
  crash.events = { hitch() };
  BuildEvidenceAndSummary(crash, i18n::Language::kEnglish);
  Require(!AnyStartsWith(crash.recommendations, L"[Performance]"), "no stutter advice in a crash report");

  auto hang = MakeHang();
  hang.has_blackbox = true;
  hang.events = { hitch() };
  BuildEvidenceAndSummary(hang, i18n::Language::kEnglish);
  Require(!AnyStartsWith(hang.recommendations, L"[Check]", L"Fault module could not be determined"),
    "a freeze has no faulting module to look for");
  Require(AnyStartsWith(hang.recommendations, L"[Performance]"), "a freeze keeps the stutter advice");

  AnalysisResult manual{};
  manual.dump_path = L"C:\\Logs\\SkyrimDiag_Manual_20261012_102746_296.dmp";
  manual.freeze_analysis.has_analysis = true;
  manual.freeze_analysis.state_id = "freeze_ambiguous";
  BuildEvidenceAndSummary(manual, i18n::Language::kEnglish);
  Require(!AnyStartsWith(manual.recommendations, L"[Freeze]"), "no freeze-state advice for a running game");

  auto modal = MakeHang();
  modal.modal_dialog_wait.detected = true;
  modal.modal_dialog_wait.window_evidence = true;
  modal.modal_dialog_wait.dialog_title = L"SmoothCam.dll";
  skydiag::dump_tool::PluginRuleDiagnosis upkeep{};
  upkeep.rule_id = "ESL_SLOT_NEAR_LIMIT";
  upkeep.confidence_level = i18n::ConfidenceLevel::kLow;
  upkeep.recommendations = { L"[Optimization] Consider removing or merging unnecessary ESL plugins" };
  modal.plugin_diagnostics.push_back(upkeep);
  BuildEvidenceAndSummary(modal, i18n::Language::kEnglish);
  Require(!AnyStartsWith(modal.recommendations, L"[Optimization]"), "no load-order upkeep next to the dialog");
}

// The cell the player stood in (defined by a large location overhaul) is not
// a candidate; the character being processed is (2026-10-07 real crash).
void TestCellIsNotACandidate()
{
  AnalysisResult r{};
  r.dump_path = L"C:\\Logs\\SkyrimDiag_Crash_20261007_123742_440.dmp";
  r.exc_code = 0xC0000005u;
  r.fault_module_filename = L"SkyrimSE.exe";
  AnalysisResult::CrashLoggerModReference character{};
  character.esp_name = L"AE_StellarBlade_Doro.esp";
  character.best_object_type = L"Character*";
  character.best_location = L"RCX";
  character.relevance_score = 20;
  AnalysisResult::CrashLoggerModReference cell{};
  cell.esp_name = L"Lux.esp";
  cell.best_object_type = L"TESObjectCELL*";
  cell.best_location = L"RSP+328";
  cell.relevance_score = 7;
  r.crash_logger_object_refs = { character, cell };
  BuildEvidenceAndSummary(r, i18n::Language::kEnglish);
  bool hasCharacterPlugin = false;
  for (const auto& candidate : r.actionable_candidates) {
    Require(candidate.display_name != L"Lux.esp" && candidate.plugin_name != L"Lux.esp", "a cell is not a candidate");
    hasCharacterPlugin = hasCharacterPlugin || candidate.plugin_name == L"AE_StellarBlade_Doro.esp";
  }
  Require(hasCharacterPlugin, "the character's plugin still is");

  r.actionable_candidates.clear();
  for (const auto& key : skydiag::dump_tool::CollectHistoryCandidateKeys(r)) {
    Require(key.find("lux") == std::string::npos, "a cell is not a history candidate either");
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
    TestReportKeepsRecentEvents();
    TestFreezesGroupByTheMainThreadWait();
    TestBystanderStackIsNotRecordedInHistory();
    TestSummaryKeepsPluginCountsOnly();
    TestAdviceFitsTheCapture();
    TestCellIsNotACandidate();
    TestCrashesAreUntouched();
    std::puts("freeze context report tests passed");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
