#include "FreezeCandidateConsensus.h"
#include "SourceGuardTestUtils.h"
#include "WctTypes.h"

#include <filesystem>

using skydiag::dump_tool::ActionableCandidate;
using skydiag::dump_tool::BuildFreezeCandidateConsensus;
using skydiag::dump_tool::FreezeSignalInput;
using skydiag::dump_tool::i18n::ConfidenceLevel;
using skydiag::dump_tool::i18n::ConfidenceText;
using skydiag::dump_tool::i18n::Language;
using skydiag::tests::source_guard::AssertContains;
using skydiag::tests::source_guard::ReadAllText;

namespace {

ActionableCandidate MakeCandidate(std::wstring name, ConfidenceLevel level = ConfidenceLevel::kMedium)
{
  ActionableCandidate candidate{};
  candidate.display_name = std::move(name);
  candidate.confidence_level = level;
  candidate.confidence = ConfidenceText(Language::kEnglish, level);
  return candidate;
}

void TestSourceContracts()
{
  const std::filesystem::path repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  const auto analyzerHeader = repoRoot / "dump_tool" / "src" / "Analyzer.h";
  const auto analyzerCpp = repoRoot / "dump_tool" / "src" / "Analyzer.cpp";
  const auto freezeConsensusHeader = repoRoot / "dump_tool" / "src" / "FreezeCandidateConsensus.h";
  const auto freezeConsensusCpp = repoRoot / "dump_tool" / "src" / "FreezeCandidateConsensus.cpp";

  assert(std::filesystem::exists(analyzerHeader) && "dump_tool/src/Analyzer.h not found");
  assert(std::filesystem::exists(analyzerCpp) && "dump_tool/src/Analyzer.cpp not found");
  assert(std::filesystem::exists(freezeConsensusHeader) && "dump_tool/src/FreezeCandidateConsensus.h not found");
  assert(std::filesystem::exists(freezeConsensusCpp) && "dump_tool/src/FreezeCandidateConsensus.cpp not found");

  const auto analyzerHeaderText = ReadAllText(analyzerHeader);
  const auto analyzerCppText = ReadAllText(analyzerCpp);
  const auto freezeConsensusHeaderText = ReadAllText(freezeConsensusHeader);
  const auto freezeConsensusCppText = ReadAllText(freezeConsensusCpp);

  AssertContains(analyzerHeaderText, "struct FreezeAnalysisResult", "AnalysisResult must expose a structured freeze analysis model.");
  AssertContains(analyzerHeaderText, "freeze_analysis", "AnalysisResult must store freeze analysis output.");
  AssertContains(analyzerHeaderText, "modal_dialog_wait", "Freeze analysis state ids must include modal_dialog_wait.");
  AssertContains(analyzerHeaderText, "deadlock_likely", "Freeze analysis state ids must include deadlock_likely.");
  AssertContains(analyzerHeaderText, "synchronization_stall_likely", "Freeze analysis state ids must include synchronization_stall_likely.");
  AssertContains(analyzerHeaderText, "loader_stall_likely", "Freeze analysis state ids must include loader_stall_likely.");
  AssertContains(analyzerHeaderText, "freeze_candidate", "Freeze analysis state ids must include freeze_candidate.");
  AssertContains(analyzerHeaderText, "freeze_ambiguous", "Freeze analysis state ids must include freeze_ambiguous.");

  AssertContains(freezeConsensusHeaderText, "BuildFreezeCandidateConsensus", "Freeze candidate consensus entry point must exist.");
  AssertContains(freezeConsensusHeaderText, "first_chance", "Freeze candidate consensus input must accept first-chance context.");
  AssertContains(freezeConsensusCppText, "deadlock_likely", "Freeze candidate consensus must classify deadlock_likely.");
  AssertContains(freezeConsensusCppText, "synchronization_stall_likely", "Freeze candidate consensus must classify synchronization_stall_likely.");
  AssertContains(freezeConsensusCppText, "loader_stall_likely", "Freeze candidate consensus must classify loader_stall_likely.");
  AssertContains(freezeConsensusCppText, "freeze_candidate", "Freeze candidate consensus must classify freeze_candidate.");
  AssertContains(freezeConsensusCppText, "freeze_ambiguous", "Freeze candidate consensus must classify freeze_ambiguous.");
  AssertContains(freezeConsensusCppText, "snapshot_consensus_backed", "Freeze candidate consensus must expose richer snapshot_consensus_backed quality.");
  AssertContains(freezeConsensusCppText, "cycle_consensus", "Freeze candidate consensus must consume WCT cycle consensus.");
  AssertContains(freezeConsensusCppText, "consistent_loading_signal", "Freeze candidate consensus must consume consistent loading signal conservatively.");
  AssertContains(freezeConsensusCppText, "repeated suspicious first-chance", "Freeze candidate consensus must explain repeated suspicious first-chance context.");
  AssertContains(analyzerCppText, "BuildFreezeCandidateConsensus", "Analyzer must invoke freeze candidate consensus for freeze-like dumps.");
}

void TestWctFreezeSummaryParsing()
{
  const std::string json =
    R"({"capture":{"kind":"hang","secondsSinceHeartbeat":15.0,"thresholdSec":10,"isLoading":true,"pss_snapshot_requested":true,"pss_snapshot_used":true,"pss_snapshot_capture_ms":42,"pss_snapshot_status":"ok","dump_transport":"pss_snapshot"},"threads":[{"tid":1234,"isCycle":true},{"tid":5678,"isCycle":false,"nodes":[{"objectStatus":3,"thread":{"waitTime":500}}]}]})";

  const auto parsed = skydiag::dump_tool::internal::TryParseWctFreezeSummary(json);
  assert(parsed.has_value());
  assert(parsed->has);
  assert(parsed->threads == 2);
  assert(parsed->cycles == 1);
  assert(parsed->cycle_thread_ids.size() == 1);
  assert(parsed->cycle_thread_ids[0] == 1234u);
  assert(parsed->longest_wait_tid == 5678u);
  assert(parsed->longest_wait_start_tick == 500u);
  assert(parsed->has_capture);
  assert(parsed->capture_kind == "hang");
  assert(parsed->isLoading);
  assert(parsed->suggestsHang);
  assert(parsed->pss_snapshot_requested);
  assert(parsed->pss_snapshot_used);
  assert(parsed->pss_snapshot_capture_ms == 42u);
  assert(parsed->pss_snapshot_status == "ok");
  assert(parsed->dump_transport == "pss_snapshot");
}

void TestConsensusDeadlockLikely()
{
  FreezeSignalInput input{};
  input.is_hang_like = true;
  input.wct = skydiag::dump_tool::internal::WctFreezeSummary{};
  input.wct->has = true;
  input.wct->cycles = 2;
  input.wct->pss_snapshot_used = true;

  const auto result = BuildFreezeCandidateConsensus(input, Language::kEnglish);
  assert(result.has_analysis);
  assert(result.state_id == "deadlock_likely");
  assert(result.support_quality == "snapshot_backed");
  assert(result.confidence_level == ConfidenceLevel::kMedium);
}

void TestConsensusDeadlockSinglePassLiveStaysConservative()
{
  FreezeSignalInput input{};
  input.is_hang_like = true;
  input.wct = skydiag::dump_tool::internal::WctFreezeSummary{};
  input.wct->has = true;
  input.wct->has_capture = true;
  input.wct->cycles = 1;
  input.wct->cycle_thread_ids = { 1111u };

  const auto result = BuildFreezeCandidateConsensus(input, Language::kEnglish);
  assert(result.has_analysis);
  assert(result.state_id == "deadlock_likely");
  assert(result.support_quality == "live_process");
  assert(result.confidence_level == ConfidenceLevel::kMedium);
}

void TestConsensusDeadlockSnapshotConsensusBacked()
{
  FreezeSignalInput input{};
  input.is_hang_like = true;
  input.wct = skydiag::dump_tool::internal::WctFreezeSummary{};
  input.wct->has = true;
  input.wct->has_capture = true;
  input.wct->cycles = 2;
  input.wct->cycle_consensus = true;
  input.wct->repeated_cycle_tids = { 1234u, 5678u };
  input.wct->longest_wait_tid_consensus = true;
  input.wct->pss_snapshot_used = true;

  const auto result = BuildFreezeCandidateConsensus(input, Language::kEnglish);
  assert(result.has_analysis);
  assert(result.state_id == "deadlock_likely");
  assert(result.support_quality == "snapshot_consensus_backed");
  assert(result.confidence_level == ConfidenceLevel::kHigh);
  assert(result.primary_reasons.size() >= 2u);
}

void TestConsensusModuleThreadGroupSupportsLogicalSynchronizationStall()
{
  FreezeSignalInput input{};
  input.is_hang_like = true;
  input.thread_module_consensus = skydiag::dump_tool::HangThreadModuleConsensus{};
  input.thread_module_consensus->has_consensus = true;
  input.thread_module_consensus->main_thread_id = 45112u;
  input.thread_module_consensus->module_filename = L"hdtsmp64.dll";
  input.thread_module_consensus->matching_thread_count = 16u;
  input.thread_module_consensus->stable_thread_count = 16u;
  input.thread_module_consensus->os_lock_cycle_proven = false;

  const auto result = BuildFreezeCandidateConsensus(input, Language::kEnglish);
  assert(result.has_analysis);
  assert(result.state_id == "synchronization_stall_likely");
  // support_quality describes the capture; the thread group is in state_id.
  assert(result.support_quality == "unknown");
  assert(result.confidence_level == ConfidenceLevel::kMedium);
  assert(result.thread_module_consensus.module_filename == L"hdtsmp64.dll");
  assert(result.primary_reasons.size() == 3u);

  // A PSS fallback stays visible next to a thread-group consensus (review L11).
  input.wct = skydiag::dump_tool::internal::WctFreezeSummary{};
  input.wct->has = true;
  input.wct->has_capture = true;
  input.wct->pss_snapshot_requested = true;
  input.wct->pss_snapshot_used = false;
  const auto fallback = BuildFreezeCandidateConsensus(input, Language::kEnglish);
  assert(fallback.state_id == "synchronization_stall_likely");
  assert(fallback.support_quality == "snapshot_fallback");
}

// A manual snapshot taken while the main thread still answered is no stuck
// load; one taken while it did not, or an automatic hang capture, still is
// (review L12).
void TestManualLoadingSnapshotWithFreshHeartbeatIsNoLoaderStall()
{
  FreezeSignalInput input{};
  input.is_snapshot_like = true;
  input.is_manual_capture = true;
  input.loading_context = true;
  input.wct = skydiag::dump_tool::internal::WctFreezeSummary{};
  input.wct->has = true;
  input.wct->has_capture = true;
  input.wct->capture_kind = "manual";
  input.wct->isLoading = true;
  input.wct->thresholdSec = 600u;
  input.wct->secondsSinceHeartbeat = 0.4;

  const auto fresh = BuildFreezeCandidateConsensus(input, Language::kEnglish);
  assert(fresh.state_id == "freeze_ambiguous");
  assert(fresh.confidence_level == ConfidenceLevel::kLow);
  bool explained = false;
  for (const auto& reason : fresh.primary_reasons) {
    explained = explained || reason.find(L"main thread was still responding") != std::wstring::npos;
  }
  assert(explained);

  FreezeSignalInput stale = input;
  stale.wct->secondsSinceHeartbeat = 45.0;
  assert(BuildFreezeCandidateConsensus(stale, Language::kEnglish).state_id == "loader_stall_likely");

  FreezeSignalInput hang = input;
  hang.is_snapshot_like = false;
  hang.is_manual_capture = false;
  hang.is_hang_like = true;
  assert(BuildFreezeCandidateConsensus(hang, Language::kEnglish).state_id == "loader_stall_likely");

  // Without the helper's heartbeat age the load still counts as stuck.
  FreezeSignalInput unknownAge = input;
  unknownAge.wct->thresholdSec = 0u;
  assert(BuildFreezeCandidateConsensus(unknownAge, Language::kEnglish).state_id == "loader_stall_likely");
}

void TestConsensusLoaderStallLikely()
{
  FreezeSignalInput input{};
  input.is_hang_like = true;
  input.loading_context = true;
  input.wct = skydiag::dump_tool::internal::WctFreezeSummary{};
  input.wct->has = true;
  input.wct->has_capture = true;
  input.wct->capture_kind = "hang";
  input.wct->isLoading = true;

  const auto result = BuildFreezeCandidateConsensus(input, Language::kEnglish);
  assert(result.has_analysis);
  assert(result.state_id == "loader_stall_likely");
  assert(result.confidence_level == ConfidenceLevel::kMedium);
}

void TestConsensusLoaderStallWithBlackboxChurn()
{
  FreezeSignalInput input{};
  input.is_hang_like = true;
  input.loading_context = true;
  input.wct = skydiag::dump_tool::internal::WctFreezeSummary{};
  input.wct->has = true;
  input.wct->has_capture = true;
  input.wct->isLoading = true;

  skydiag::dump_tool::BlackboxFreezeSummary blackbox{};
  blackbox.has_context = true;
  blackbox.loading_window = true;
  blackbox.recent_module_loads = 3;
  blackbox.recent_module_unloads = 2;
  blackbox.module_churn_score = 5;
  blackbox.recent_non_system_modules.push_back(L"po3_PapyrusExtender.dll");
  input.blackbox = blackbox;

  const auto result = BuildFreezeCandidateConsensus(input, Language::kEnglish);
  assert(result.has_analysis);
  assert(result.state_id == "loader_stall_likely");
  assert(result.confidence_level == ConfidenceLevel::kHigh);
  assert(!result.primary_reasons.empty());
}

void TestConsensusLoaderStallWithFirstChanceContext()
{
  FreezeSignalInput input{};
  input.is_hang_like = true;
  input.loading_context = true;
  input.wct = skydiag::dump_tool::internal::WctFreezeSummary{};
  input.wct->has = true;
  input.wct->has_capture = true;
  input.wct->isLoading = true;

  skydiag::dump_tool::FirstChanceSummary firstChance{};
  firstChance.has_context = true;
  firstChance.recent_count = 4;
  firstChance.loading_window_count = 4;
  firstChance.repeated_signature_count = 2;
  firstChance.recent_non_system_modules.push_back(L"po3_PapyrusExtender.dll");
  input.first_chance = firstChance;

  const auto result = BuildFreezeCandidateConsensus(input, Language::kEnglish);
  assert(result.has_analysis);
  assert(result.state_id == "loader_stall_likely");
  assert(result.confidence_level == ConfidenceLevel::kHigh);
  assert(!result.primary_reasons.empty());
}

void TestConsensusLoaderStallNeedsContextForConsistentLoadingSignal()
{
  FreezeSignalInput weakInput{};
  weakInput.is_hang_like = true;
  weakInput.wct = skydiag::dump_tool::internal::WctFreezeSummary{};
  weakInput.wct->has = true;
  weakInput.wct->has_capture = true;
  weakInput.wct->isLoading = true;
  weakInput.wct->consistent_loading_signal = true;
  weakInput.wct->pss_snapshot_used = true;

  const auto weakResult = BuildFreezeCandidateConsensus(weakInput, Language::kEnglish);
  assert(weakResult.has_analysis);
  assert(weakResult.state_id == "loader_stall_likely");
  assert(weakResult.support_quality == "snapshot_backed");
  assert(weakResult.confidence_level == ConfidenceLevel::kMedium);

  FreezeSignalInput loadingOnlyInput = weakInput;
  loadingOnlyInput.loading_context = true;

  const auto loadingOnlyResult = BuildFreezeCandidateConsensus(loadingOnlyInput, Language::kEnglish);
  assert(loadingOnlyResult.has_analysis);
  assert(loadingOnlyResult.state_id == "loader_stall_likely");
  assert(loadingOnlyResult.support_quality == "snapshot_backed");
  assert(loadingOnlyResult.confidence_level == ConfidenceLevel::kMedium);

  FreezeSignalInput strongInput = loadingOnlyInput;
  skydiag::dump_tool::FirstChanceSummary firstChance{};
  firstChance.has_context = true;
  firstChance.loading_window_count = 3;
  firstChance.repeated_signature_count = 1;
  strongInput.first_chance = firstChance;

  const auto strongResult = BuildFreezeCandidateConsensus(strongInput, Language::kEnglish);
  assert(strongResult.has_analysis);
  assert(strongResult.state_id == "loader_stall_likely");
  assert(strongResult.support_quality == "snapshot_consensus_backed");
  assert(strongResult.confidence_level == ConfidenceLevel::kHigh);
}

void TestConsensusSnapshotFallbackAndSnapshotBackedStayStateConservative()
{
  FreezeSignalInput fallbackInput{};
  fallbackInput.is_manual_capture = true;
  fallbackInput.actionable_candidates.push_back(MakeCandidate(L"FallbackFreezeMod"));
  fallbackInput.wct = skydiag::dump_tool::internal::WctFreezeSummary{};
  fallbackInput.wct->has = true;
  fallbackInput.wct->has_capture = true;
  fallbackInput.wct->pss_snapshot_requested = true;
  fallbackInput.wct->pss_snapshot_used = false;

  const auto fallbackResult = BuildFreezeCandidateConsensus(fallbackInput, Language::kEnglish);
  assert(fallbackResult.has_analysis);
  assert(fallbackResult.state_id == "freeze_candidate");
  assert(fallbackResult.support_quality == "snapshot_fallback");
  assert(fallbackResult.confidence_level == ConfidenceLevel::kLow);

  FreezeSignalInput backedInput{};
  backedInput.is_manual_capture = true;
  backedInput.wct = skydiag::dump_tool::internal::WctFreezeSummary{};
  backedInput.wct->has = true;
  backedInput.wct->has_capture = true;
  backedInput.wct->pss_snapshot_used = true;

  const auto backedResult = BuildFreezeCandidateConsensus(backedInput, Language::kEnglish);
  assert(backedResult.has_analysis);
  assert(backedResult.state_id == "freeze_ambiguous");
  assert(backedResult.support_quality == "snapshot_backed");
  assert(backedResult.confidence_level == ConfidenceLevel::kLow);
}

void TestConsensusModalDialogWaitOutranksOtherStates()
{
  skydiag::dump_tool::ModalDialogWaitInfo modal{};
  modal.detected = true;
  modal.window_evidence = true;
  modal.stack_evidence = true;
  modal.main_thread_id = 100;
  modal.wait_api = L"user32.dll!MessageBoxW";
  modal.dialog_title = L"Fatal Error";
  modal.caller_module_filename = L"BrokenPlugin.dll";
  modal.caller_kind = "plugin";

  FreezeSignalInput input{};
  input.is_hang_like = true;
  input.loading_context = true;
  input.wct = skydiag::dump_tool::internal::WctFreezeSummary{};
  input.wct->has = true;
  input.wct->has_capture = true;
  input.wct->cycles = 2;
  skydiag::dump_tool::BlackboxFreezeSummary blackbox{};
  blackbox.has_context = true;
  blackbox.loading_window = true;
  blackbox.module_churn_score = 5;
  blackbox.recent_non_system_modules.push_back(L"ColdBreathNG.dll");
  input.blackbox = blackbox;
  input.modal_dialog_wait = modal;
  input.actionable_candidates.push_back(MakeCandidate(L"BrokenPlugin.dll"));

  const auto both = BuildFreezeCandidateConsensus(input, Language::kEnglish);
  assert(both.has_analysis);
  assert(both.state_id == "modal_dialog_wait");
  assert(both.confidence_level == ConfidenceLevel::kHigh);
  assert(both.related_candidates.size() == 1u);
  assert(both.related_candidates[0].display_name == L"BrokenPlugin.dll");
  bool mentionsBystanders = false;
  for (const auto& reason : both.primary_reasons) {
    if (reason.find(L"bystanders") != std::wstring::npos) {
      mentionsBystanders = true;
    }
  }
  assert(mentionsBystanders);

  modal.stack_evidence = false;
  input.modal_dialog_wait = modal;
  const auto windowOnly = BuildFreezeCandidateConsensus(input, Language::kEnglish);
  assert(windowOnly.state_id == "modal_dialog_wait");
  assert(windowOnly.confidence_level == ConfidenceLevel::kMedium);

  modal.detected = false;
  input.modal_dialog_wait = modal;
  const auto notModal = BuildFreezeCandidateConsensus(input, Language::kEnglish);
  assert(notModal.state_id == "deadlock_likely");
}

void TestConsensusFreezeCandidateAndAmbiguous()
{
  FreezeSignalInput candidateInput{};
  candidateInput.is_manual_capture = true;
  candidateInput.actionable_candidates.push_back(MakeCandidate(L"ExampleFreezeMod"));

  const auto candidateResult = BuildFreezeCandidateConsensus(candidateInput, Language::kEnglish);
  assert(candidateResult.has_analysis);
  assert(candidateResult.state_id == "freeze_candidate");
  assert(candidateResult.related_candidates.size() == 1);
  assert(candidateResult.related_candidates[0].display_name == L"ExampleFreezeMod");

  FreezeSignalInput ambiguousInput{};
  ambiguousInput.is_manual_capture = true;

  const auto ambiguousResult = BuildFreezeCandidateConsensus(ambiguousInput, Language::kEnglish);
  assert(ambiguousResult.has_analysis);
  assert(ambiguousResult.state_id == "freeze_ambiguous");
  assert(ambiguousResult.related_candidates.empty());
}

}  // namespace

// ADR-0009: the main thread's wait is the first reason, without changing the
// state, and a modal dialog keeps its own reasons.
void TestConsensusMainThreadWaitLeadsTheReasons()
{
  skydiag::dump_tool::MainThreadWaitInfo engineWait{};
  engineWait.kind = "engine_wait";
  engineWait.wait_api = L"KERNELBASE.dll!SleepEx";
  engineWait.waiting_module = L"SkyrimSE.exe";
  engineWait.path_modules = { L"EngineFixes.dll", L"CommunityShaders.dll" };

  FreezeSignalInput input{};
  input.is_hang_like = true;
  input.main_thread_wait = engineWait;
  const auto result = BuildFreezeCandidateConsensus(input, Language::kEnglish);
  assert(result.state_id == "freeze_ambiguous");
  assert(!result.primary_reasons.empty());
  assert(result.primary_reasons.front().find(L"waiting inside the game engine (SkyrimSE.exe, KERNELBASE.dll!SleepEx)") !=
         std::wstring::npos);
  assert(result.primary_reasons.front().find(L"call path only): EngineFixes.dll, CommunityShaders.dll") !=
         std::wstring::npos);

  skydiag::dump_tool::MainThreadWaitInfo driverWait{};
  driverWait.kind = "graphics_driver_wait";
  driverWait.waiting_module = L"nvwgf2umx.dll";
  FreezeSignalInput driver{};
  driver.is_hang_like = true;
  driver.main_thread_wait = driverWait;
  const auto driverResult = BuildFreezeCandidateConsensus(driver, Language::kKorean);
  assert(driverResult.primary_reasons.front().find(L"그래픽 드라이버(nvwgf2umx.dll)") != std::wstring::npos);

  FreezeSignalInput modal{};
  modal.is_hang_like = true;
  modal.main_thread_wait = engineWait;
  modal.modal_dialog_wait = skydiag::dump_tool::ModalDialogWaitInfo{};
  modal.modal_dialog_wait->detected = true;
  modal.modal_dialog_wait->window_evidence = true;
  const auto modalResult = BuildFreezeCandidateConsensus(modal, Language::kEnglish);
  assert(modalResult.state_id == "modal_dialog_wait");
  for (const auto& reason : modalResult.primary_reasons) {
    assert(reason.find(L"game engine") == std::wstring::npos);
  }

  // A WCT-proven deadlock keeps its own reasons first; the wait follows.
  FreezeSignalInput deadlock{};
  deadlock.is_hang_like = true;
  deadlock.main_thread_wait = engineWait;
  deadlock.wct = skydiag::dump_tool::internal::WctFreezeSummary{};
  deadlock.wct->has = true;
  deadlock.wct->cycles = 2;
  const auto deadlockResult = BuildFreezeCandidateConsensus(deadlock, Language::kEnglish);
  assert(deadlockResult.state_id == "deadlock_likely");
  assert(deadlockResult.primary_reasons.size() >= 2u);
  assert(deadlockResult.primary_reasons.front().find(L"game engine") == std::wstring::npos);
  assert(deadlockResult.primary_reasons.back().find(L"waiting inside the game engine") != std::wstring::npos);
}

// The 2026-10-18 freeze, synthetic: Crash Logger's thread dump suspended the
// owner of the DXGI critical section the main thread waited on.
static void TestConsensusCrashLoggerThreadDumpDeadlock()
{
  skydiag::dump_tool::MainThreadWaitInfo driverWait{};
  driverWait.kind = "graphics_driver_wait";
  driverWait.wait_api = L"ntdll.dll!RtlEnterCriticalSection";
  driverWait.waiting_module = L"dxgi.dll";
  driverWait.path_modules = { L"HitchHunter.dll", L"hdtsmp64.dll" };

  skydiag::dump_tool::MainThreadLockOwnerInfo owner{};
  owner.tid = 51944;
  owner.lock_type = "critical_section";
  owner.confirmed = true;
  owner.thread_name = L"nv.sl.dlss_g.thread.dlssg";
  owner.top_module = L"nvwgf2umx.dll";
  owner.blocked_without_wait_object = true;
  owner.context_switches_stable = true;
  owner.ip_state = "not_after_syscall";

  skydiag::dump_tool::CrashLoggerThreadDumpDeadlockInfo deadlock{};
  deadlock.detected = true;
  deadlock.crash_logger_tid = 54776;
  deadlock.crash_logger_module = L"CrashLogger.dll";
  deadlock.thread_dump_truncated = true;
  deadlock.thread_dump_records = 167;
  deadlock.thread_dump_total_threads = 292;
  deadlock.thread_dump_before_capture = true;

  FreezeSignalInput input{};
  input.is_hang_like = true;
  input.main_thread_wait = driverWait;
  input.lock_owner = owner;
  input.crash_logger_thread_dump_deadlock = deadlock;
  input.actionable_candidates.push_back(MakeCandidate(L"HitchHunter.dll", ConfidenceLevel::kLow));
  input.blackbox = skydiag::dump_tool::BlackboxFreezeSummary{};
  input.blackbox->has_context = true;
  input.blackbox->recent_non_system_modules = { L"SomeRecent.dll" };
  const auto result = BuildFreezeCandidateConsensus(input, Language::kEnglish);
  assert(result.state_id == "crash_logger_thread_dump_deadlock");
  assert(result.confidence_level == ConfidenceLevel::kHigh);
  assert(result.primary_reasons.size() == 4u);
  assert(result.primary_reasons[0].find(L"thread 54776") != std::wstring::npos);
  assert(result.primary_reasons[1].find(L"thread 51944 (nv.sl.dlss_g.thread.dlssg") != std::wstring::npos);
  assert(result.primary_reasons[2].find(L"167 of 292 threads") != std::wstring::npos);
  // The main thread's wait names the lock owner, not the graphics driver.
  assert(result.primary_reasons[3].find(L"for a critical section held by thread 51944") != std::wstring::npos);
  assert(result.primary_reasons[3].find(L"graphics driver") == std::wstring::npos);
  for (const auto& related : result.related_candidates) {
    assert(related.display_name != L"SomeRecent.dll");
  }

  // Without the deadlock the lock owner still leads the main-thread reason,
  // and the state stays what the other signals say.
  FreezeSignalInput ownerOnly = input;
  ownerOnly.crash_logger_thread_dump_deadlock.reset();
  ownerOnly.lock_owner->confirmed = false;
  const auto ownerResult = BuildFreezeCandidateConsensus(ownerOnly, Language::kKorean);
  assert(ownerResult.state_id == "freeze_candidate");
  assert(ownerResult.primary_reasons.front().find(L"스레드 51944(nv.sl.dlss_g.thread.dlssg") != std::wstring::npos);
  assert(ownerResult.primary_reasons.front().find(L"두 번째 WCT로는 확인되지 않음") != std::wstring::npos);
  assert(ownerResult.primary_reasons.front().find(L"HitchHunter.dll, hdtsmp64.dll") != std::wstring::npos);

  // Confirmed by both passes: its own state at medium confidence, the lock
  // owner first, and no stack candidate among the related ones.
  FreezeSignalInput confirmedOwner = input;
  confirmedOwner.crash_logger_thread_dump_deadlock.reset();
  const auto confirmedResult = BuildFreezeCandidateConsensus(confirmedOwner, Language::kEnglish);
  assert(confirmedResult.state_id == "lock_owner_wait");
  assert(confirmedResult.confidence_level == ConfidenceLevel::kMedium);
  assert(confirmedResult.primary_reasons.size() == 2u);
  assert(confirmedResult.primary_reasons.front().find(L"for a critical section held by thread 51944") != std::wstring::npos);
  assert(confirmedResult.primary_reasons.front().find(L"both WCT passes named the same owner") != std::wstring::npos);
  for (const auto& related : confirmedResult.related_candidates) {
    assert(related.display_name != L"HitchHunter.dll");
  }
  bool keptRecent = false;
  for (const auto& related : confirmedResult.related_candidates) {
    keptRecent = keptRecent || related.display_name == L"SomeRecent.dll";
  }
  assert(keptRecent);
  // A WCT cycle or a stuck load still outranks the lock owner.
  FreezeSignalInput cycle = confirmedOwner;
  cycle.wct = skydiag::dump_tool::internal::WctFreezeSummary{};
  cycle.wct->has = true;
  cycle.wct->cycles = 2;
  assert(BuildFreezeCandidateConsensus(cycle, Language::kEnglish).state_id == "deadlock_likely");
  FreezeSignalInput loading = confirmedOwner;
  loading.loading_context = true;
  assert(BuildFreezeCandidateConsensus(loading, Language::kEnglish).state_id == "loader_stall_likely");

  // A modal dialog outranks both.
  FreezeSignalInput modal = input;
  modal.modal_dialog_wait = skydiag::dump_tool::ModalDialogWaitInfo{};
  modal.modal_dialog_wait->detected = true;
  assert(BuildFreezeCandidateConsensus(modal, Language::kEnglish).state_id == "modal_dialog_wait");
}

int main()
{
  TestSourceContracts();
  TestWctFreezeSummaryParsing();
  TestConsensusDeadlockLikely();
  TestConsensusDeadlockSinglePassLiveStaysConservative();
  TestConsensusDeadlockSnapshotConsensusBacked();
  TestConsensusModuleThreadGroupSupportsLogicalSynchronizationStall();
  TestConsensusLoaderStallLikely();
  TestConsensusLoaderStallWithBlackboxChurn();
  TestConsensusLoaderStallWithFirstChanceContext();
  TestConsensusLoaderStallNeedsContextForConsistentLoadingSignal();
  TestManualLoadingSnapshotWithFreshHeartbeatIsNoLoaderStall();
  TestConsensusSnapshotFallbackAndSnapshotBackedStayStateConservative();
  TestConsensusFreezeCandidateAndAmbiguous();
  TestConsensusModalDialogWaitOutranksOtherStates();
  TestConsensusMainThreadWaitLeadsTheReasons();
  TestConsensusCrashLoggerThreadDumpDeadlock();
  return 0;
}
