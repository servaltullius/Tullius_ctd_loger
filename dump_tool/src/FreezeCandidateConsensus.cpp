#include "FreezeCandidateConsensus.h"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

#include "GameLockWait.h"
#include "MainThreadWait.h"

namespace skydiag::dump_tool {
namespace {

std::wstring PickCandidateName(const ActionableCandidate& candidate)
{
  if (!candidate.display_name.empty()) {
    return candidate.display_name;
  }
  if (!candidate.plugin_name.empty()) {
    return candidate.plugin_name;
  }
  if (!candidate.mod_name.empty()) {
    return candidate.mod_name;
  }
  return candidate.module_filename;
}

std::wstring JoinModules(const std::vector<std::wstring>& items, std::size_t maxCount)
{
  std::wstring joined;
  const std::size_t limit = std::min(items.size(), maxCount);
  for (std::size_t i = 0; i < limit; ++i) {
    if (i != 0u) {
      joined += L", ";
    }
    joined += items[i];
  }
  return joined;
}

bool HasSnapshotBackedCapture(const FreezeSignalInput& input)
{
  return input.wct && input.wct->pss_snapshot_used;
}

bool HasSnapshotFallbackCapture(const FreezeSignalInput& input)
{
  return input.wct && input.wct->pss_snapshot_requested && !input.wct->pss_snapshot_used;
}

bool HasLiveProcessCapture(const FreezeSignalInput& input)
{
  return input.wct && input.wct->has_capture;
}

bool HasConsensusBackedDeadlock(const FreezeSignalInput& input)
{
  return input.wct &&
         input.wct->cycle_consensus &&
         !input.wct->repeated_cycle_tids.empty() &&
         input.wct->pss_snapshot_used;
}

FreezeRelatedCandidate ToRelatedCandidate(const ActionableCandidate& candidate, i18n::Language language)
{
  FreezeRelatedCandidate row{};
  row.confidence_level = candidate.confidence_level;
  row.confidence = candidate.confidence.empty()
    ? i18n::ConfidenceText(language, candidate.confidence_level)
    : candidate.confidence;
  row.display_name = PickCandidateName(candidate);
  return row;
}

FreezeRelatedCandidate ToRelatedModuleCandidate(std::wstring name, i18n::Language language)
{
  FreezeRelatedCandidate row{};
  row.confidence_level = i18n::ConfidenceLevel::kLow;
  row.confidence = i18n::ConfidenceText(language, row.confidence_level);
  row.display_name = std::move(name);
  return row;
}

bool HasStrongFirstChanceLoaderSignal(const FreezeSignalInput& input, bool loadingSignal)
{
  return loadingSignal &&
         input.first_chance.has_value() &&
         input.first_chance->has_context &&
         (input.first_chance->loading_window_count >= 2u || input.first_chance->repeated_signature_count > 0u);
}

bool HasConsensusBackedLoaderSignal(const FreezeSignalInput& input, bool strongFirstChanceLoaderSignal)
{
  if (!input.wct || !input.wct->pss_snapshot_used || !input.wct->consistent_loading_signal) {
    return false;
  }

  const bool repeatedLoadingContext =
    (input.blackbox.has_value() && input.blackbox->loading_window) ||
    strongFirstChanceLoaderSignal;
  return repeatedLoadingContext;
}

std::string DetermineSupportQuality(
  const FreezeSignalInput& input,
  bool consensusBackedDeadlock,
  bool consensusBackedLoaderSignal)
{
  if (consensusBackedDeadlock || consensusBackedLoaderSignal) {
    return "snapshot_consensus_backed";
  }
  if (HasSnapshotBackedCapture(input)) {
    return "snapshot_backed";
  }
  if (HasSnapshotFallbackCapture(input)) {
    return "snapshot_fallback";
  }
  if (HasLiveProcessCapture(input)) {
    return "live_process";
  }
  return "unknown";
}

std::wstring JoinNames(const std::vector<std::wstring>& names)
{
  std::wstring out;
  for (const auto& name : names) {
    if (!out.empty()) {
      out += L", ";
    }
    out += name;
  }
  return out;
}

// What the main thread was doing, as the first reason: it decides how much the
// modules on its stack mean (ADR-0009).
void AddMainThreadWaitReason(
  const MainThreadWaitInfo& wait,
  const MainThreadLockOwnerInfo* lockOwner,
  i18n::Language language,
  FreezeAnalysisResult* result)
{
  const bool en = language == i18n::Language::kEnglish;
  const std::wstring api = wait.wait_api.empty() ? std::wstring{} : (L", " + wait.wait_api);
  const std::wstring where = wait.waiting_module.empty() ? wait.wait_api : (wait.waiting_module + api);
  std::wstring line;
  if (lockOwner) {
    // WCT named the thread holding the lock: the wait's own module (dxgi.dll
    // for a DXGI critical section) only says where the main thread called in.
    line = en
      ? (L"The main thread was waiting" + (where.empty() ? std::wstring{} : (L" (in " + where + L")")) + L" for a " +
          DescribeLockType(*lockOwner, en) + L" held by " + DescribeLockOwnerThread(*lockOwner, en) +
          (lockOwner->confirmed ? L"; both WCT passes named the same owner" : L"; not confirmed by a second WCT pass"))
      : (L"메인 스레드가 " + DescribeLockOwnerThread(*lockOwner, en) + L"이(가) 쥔 " + DescribeLockType(*lockOwner, en) +
          L" 잠금을 기다리고 있었음" + (where.empty() ? std::wstring{} : (L"(대기 위치: " + where + L")")) +
          (lockOwner->confirmed ? L"; WCT 두 번 모두 같은 소유 스레드를 가리킴" : L"; 두 번째 WCT로는 확인되지 않음"));
  } else if (wait.kind == "engine_wait" && wait.engine_wait_detail == "gpu_query_poll") {
    line = en
      ? (L"The main thread was waiting for the GPU: the engine was polling a Direct3D query "
          L"(ID3D11DeviceContext::GetData) in a Sleep loop (" + wait.waiting_module + api + L")")
      : (L"메인 스레드가 GPU를 기다리고 있었음: 엔진이 Sleep 루프에서 Direct3D 쿼리"
          L"(ID3D11DeviceContext::GetData) 결과를 기다림(" + wait.waiting_module + api + L")");
  } else if (wait.kind == "engine_wait") {
    line = en
      ? (L"The main thread was waiting inside the game engine (" + wait.waiting_module + api +
          L"), not running plugin code")
      : (L"메인 스레드는 플러그인 코드를 실행하던 것이 아니라 게임 엔진(" + wait.waiting_module + api +
          L") 안에서 대기 중이었음");
  } else if (wait.kind == "graphics_driver_wait") {
    line = en
      ? (L"The main thread was waiting inside the graphics driver (" + wait.waiting_module + api + L")")
      : (L"메인 스레드가 그래픽 드라이버(" + wait.waiting_module + api + L") 안에서 대기 중이었음");
  } else if (wait.kind == "plugin_wait") {
    line = en
      ? (L"The main thread was waiting in " + wait.wait_api + L", called by " + wait.waiting_module)
      : (L"메인 스레드가 " + wait.waiting_module + L"이(가) 호출한 " + wait.wait_api + L"에서 대기 중이었음");
  } else if (wait.kind == "running") {
    line = en
      ? (L"The main thread was running code in " + wait.waiting_module + L" (no wait on top of its stack)")
      : (L"메인 스레드가 " + wait.waiting_module + L"에서 코드를 실행 중이었음(스택 맨 위에 대기 API 없음)");
  } else {
    return;
  }
  if ((IsBystanderWait(wait) || lockOwner) && !wait.path_modules.empty()) {
    line += en
      ? (L"; plugins further down its stack (call path only): " + JoinNames(wait.path_modules))
      : (L"; 스택 아래쪽 플러그인(호출 경로일 뿐): " + JoinNames(wait.path_modules));
  }
  // A deadlock, module-level stall or loader stall keeps its own reasons first.
  const bool stallExplained = result->state_id == "deadlock_likely" ||
    result->state_id == "synchronization_stall_likely" || result->state_id == "loader_stall_likely" ||
    result->state_id == "crash_logger_thread_dump_deadlock" || result->state_id == "engine_spin_nan" ||
    result->state_id == "engine_lock_deadlock";
  if (stallExplained) {
    result->primary_reasons.push_back(std::move(line));
  } else {
    result->primary_reasons.insert(result->primary_reasons.begin(), std::move(line));
  }
  if (HasCallPathGameThread(wait) && result->state_id != "engine_spin_nan") {
    const std::wstring tid = std::to_wstring(wait.blocked_thread_tid);
    const std::wstring path = JoinNames(wait.blocked_path_modules);
    result->primary_reasons.push_back(en
      ? (L"The game thread it waited for (tid " + tid + L") was running game engine code (top: " + wait.blocked_top +
          L")" + (path.empty() ? std::wstring{} : (L"; plugins further down its stack (call path only): " + path)))
      : (L"메인 스레드가 기다린 게임 스레드(tid " + tid + L")는 게임 엔진 코드를 실행 중이었음(맨 위: " +
          wait.blocked_top + L")" + (path.empty() ? std::wstring{} : (L"; 스택 아래쪽 플러그인(호출 경로일 뿐): " + path))));
  }
}

// engine_spin_nan: an engine float compare loop on a NaN value, which never
// ends; the plugins on that thread's stack only called into the engine.
void AddEngineSpinNanReasons(const EngineSpinInfo& spin, i18n::Language language, FreezeAnalysisResult* result)
{
  const bool en = language == i18n::Language::kEnglish;
  const std::wstring who = spin.is_main_thread
    ? (en ? std::wstring(L"The main thread") : std::wstring(L"메인 스레드"))
    : (en ? (L"Thread " + std::to_wstring(spin.tid)) : (L"스레드 " + std::to_wstring(spin.tid)));
  result->primary_reasons.push_back(en
    ? (who + L" was running in both WCT passes, not waiting on anything")
    : (who + L"은(는) WCT 두 번 모두 실행 중(Running)이었고 무언가를 기다리고 있지 않았음"));
  const std::wstring compare(spin.compare.begin(), spin.compare.end());  // ASCII
  const std::wstring reg(spin.nan_register.begin(), spin.nan_register.end());
  result->primary_reasons.push_back(en
    ? (L"Its instruction was a floating-point compare loop in the game engine (" + EngineSpinLocation(spin) + L", " +
        compare + L" with a backward jump), and " + reg +
        L" held NaN; NaN compares unordered, so the loop's exit test never passes")
    : (L"그 위치는 게임 엔진의 부동소수점 비교 루프(" + EngineSpinLocation(spin) + L", " + compare +
        L" 뒤 되돌아가는 분기)였고 " + reg + L" 값이 NaN이었음. NaN은 어떤 값과도 크기 비교가 되지 않아 루프의 종료 조건이 끝내 "
        L"맞지 않음"));
  if (!spin.call_path_modules.empty()) {
    result->primary_reasons.push_back(en
      ? (L"Plugins on that thread's stack were only on the call path: " + JoinNames(spin.call_path_modules))
      : (L"그 스레드 스택의 플러그인은 호출 경로에 있었을 뿐임: " + JoinNames(spin.call_path_modules)));
  }
}

// engine_lock_deadlock (ADR-0021): game threads waiting on each other's engine
// locks, read from the lock memory the helper kept, or a known pattern.
void AddEngineLockDeadlockReasons(
  const EngineLockDeadlockInfo& deadlock,
  i18n::Language language,
  FreezeAnalysisResult* result)
{
  const bool en = language == i18n::Language::kEnglish;
  const std::wstring tids = JoinThreadIds(EngineLockDeadlockThreads(deadlock));
  if (!deadlock.cycle.empty()) {
    result->primary_reasons.push_back(en
      ? (L"Game threads " + tids + L" were each waiting in the game engine for a lock another of them held: " +
          DescribeEngineLockCycle(deadlock, en))
      : (L"게임 스레드 " + tids + L"이(가) 게임 엔진 안에서 서로가 쥔 잠금을 기다리고 있었음: " +
          DescribeEngineLockCycle(deadlock, en)));
    result->primary_reasons.push_back(en
      ? L"They were sleeping between tries in the engine's lock wait; the lock owners were read from game memory kept "
        L"in this dump, and such a cycle never resolves by itself"
      : L"모두 엔진의 잠금 대기에서 다시 시도하기 전에 잠들어 있었음. 잠금 소유 스레드는 이 덤프에 담긴 게임 메모리에서 읽었고, "
        L"이런 순환 대기는 스스로 풀리지 않음");
  }
  if (!deadlock.known_pattern.empty()) {
    const std::wstring id(deadlock.known_pattern.begin(), deadlock.known_pattern.end());
    result->primary_reasons.push_back(en
      ? (L"The waiting game threads (" + JoinThreadIds(deadlock.pattern_tids) + L") match the known engine deadlock " + id +
          L": their call stacks and the engine lock memory in this dump are that deadlock's")
      : (L"기다리던 게임 스레드(" + JoinThreadIds(deadlock.pattern_tids) + L")가 알려진 엔진 데드락 " + id +
          L"과(와) 일치함: 이 덤프의 호출 스택과 엔진 잠금 메모리가 그 데드락의 모습임"));
  }
  result->primary_reasons.push_back(en
    ? L"The freeze is inside the game engine; plugins on these threads' stacks were only on the call path"
    : L"프리징은 게임 엔진 안에서 일어났음. 이 스레드들 스택의 플러그인은 호출 경로에 있었을 뿐임");
}

void AddCrashLoggerThreadDumpDeadlockReasons(
  const MainThreadLockOwnerInfo& owner,
  const CrashLoggerThreadDumpDeadlockInfo& deadlock,
  i18n::Language language,
  FreezeAnalysisResult* result)
{
  const bool en = language == i18n::Language::kEnglish;
  const std::wstring clTid = std::to_wstring(deadlock.crash_logger_tid);
  result->primary_reasons.push_back(en
    ? (L"Crash Logger's thread dump was running on thread " + clTid + L" (" + deadlock.crash_logger_module +
        L" return addresses on its stack)")
    : (L"Crash Logger의 스레드 덤프가 스레드 " + clTid + L"에서 실행 중이었음(스택에 " + deadlock.crash_logger_module +
        L" 반환 주소)"));
  if (owner.ip_state == "after_syscall") {
    result->primary_reasons.push_back(en
      ? (L"The lock owner, " + DescribeLockOwnerThread(owner, en) +
          L", was blocked with nothing WCT could see to wait on; it was inside a system call, so it may have been "
          L"waiting by itself rather than stopped from outside")
      : (L"잠금을 쥔 " + DescribeLockOwnerThread(owner, en) +
          L"은(는) WCT에서 기다리는 대상 없이 blocked였음. 시스템 호출 중이었으므로 밖에서 정지된 것이 아니라 "
          L"스스로 기다리고 있었을 수도 있음"));
  } else {
    result->primary_reasons.push_back(en
      ? (L"The lock owner, " + DescribeLockOwnerThread(owner, en) +
          L", was stopped from outside: WCT shows it blocked with nothing to wait on, and it was not in a system call")
      : (L"잠금을 쥔 " + DescribeLockOwnerThread(owner, en) +
          L"은(는) 밖에서 정지된 상태였음: WCT에서 기다리는 대상 없이 blocked인데 시스템 호출 중도 아니었음"));
  }
  const std::wstring records = std::to_wstring(deadlock.thread_dump_records);
  result->primary_reasons.push_back(en
    ? (L"The paired Crash Logger thread dump stopped early: " + records +
        (deadlock.thread_dump_total_threads > 0u
          ? (L" of " + std::to_wstring(deadlock.thread_dump_total_threads) + L" threads")
          : std::wstring(L" threads")) +
        L" written, no [CURRENT THREAD] record")
    : (L"짝지은 Crash Logger 스레드 덤프가 중간에 멈춤: " +
        (deadlock.thread_dump_total_threads > 0u
          ? (std::to_wstring(deadlock.thread_dump_total_threads) + L"개 스레드 중 ")
          : std::wstring{}) +
        records + L"개만 기록, [CURRENT THREAD] 기록 없음"));
  result->primary_reasons.push_back(en
    ? L"The game froze after the thread dump started, and the dump's log was last written before this capture"
    : L"게임은 스레드 덤프가 시작된 뒤에 멈췄고, 덤프 로그는 이번 캡처 전에 마지막으로 기록됨");
}

std::wstring DescribeModalDialogCaller(const ModalDialogWaitInfo& modal)
{
  if (!modal.caller_inferred_mod_name.empty()) {
    return modal.caller_inferred_mod_name + L" (" + modal.caller_module_filename + L")";
  }
  return modal.caller_module_filename;
}

void AddModalDialogReasons(const ModalDialogWaitInfo& modal, i18n::Language language, FreezeAnalysisResult* result)
{
  const bool en = language == i18n::Language::kEnglish;
  if (modal.window_evidence) {
    std::wstring line = en
      ? L"The game main thread owned a visible dialog window at capture time"
      : L"캡처 시점에 게임 메인 스레드가 보이는 대화상자 창을 소유하고 있었음";
    if (!modal.dialog_title.empty()) {
      line += L": \"" + modal.dialog_title + L"\"";
    }
    result->primary_reasons.push_back(std::move(line));
  }
  if (modal.stack_evidence) {
    result->primary_reasons.push_back(en
      ? (L"The main-thread stack is waiting inside " + modal.wait_api + L" with no plugin code above it")
      : (L"메인 스레드 스택이 " + modal.wait_api + L" 안에서 대기 중이며 그 위에 플러그인 코드가 없음"));
  }
  const auto caller = DescribeModalDialogCaller(modal);
  if (modal.caller_kind == "plugin" && !caller.empty()) {
    result->primary_reasons.push_back(en
      ? (L"The dialog was opened by " + caller)
      : (L"대화상자를 연 모듈: " + caller));
  } else if (modal.caller_kind == "skse_runtime") {
    result->primary_reasons.push_back(en
      ? L"The dialog was opened by the SKSE runtime itself (typically a plugin load or version error)"
      : L"대화상자를 연 주체는 SKSE 런타임 자체임 (보통 플러그인 로드/버전 오류)");
  } else if (modal.caller_kind == "hook_framework" && !caller.empty()) {
    result->primary_reasons.push_back(en
      ? (L"The dialog was opened by hook framework " + caller)
      : (L"대화상자를 연 모듈은 훅 프레임워크 " + caller + L"임"));
  } else if (modal.caller_kind == "game_exe") {
    result->primary_reasons.push_back(en
      ? L"The dialog was opened by the game executable"
      : L"대화상자를 연 주체는 게임 실행 파일임");
  }
  if (!modal.named_module_filename.empty()) {
    const bool inTitle = modal.named_module_source == "title";
    result->primary_reasons.push_back(en
      ? (std::wstring(inTitle ? L"The dialog's title names loaded mod DLL " : L"The dialog's text names loaded mod DLL ") +
          modal.named_module_filename)
      : (std::wstring(inTitle ? L"대화상자 제목에 로드된 모드 DLL " : L"대화상자 내용에 로드된 모드 DLL ") +
          modal.named_module_filename + L"이(가) 적혀 있음"));
  }
  result->primary_reasons.push_back(en
    ? L"Modules on the waiting stack are bystanders of the dialog, not the hang cause"
    : L"대기 중인 스택의 모듈은 대화상자 대기의 구경꾼일 뿐 프리징 원인이 아님");
}

}  // namespace

FreezeAnalysisResult BuildFreezeCandidateConsensus(const FreezeSignalInput& input, i18n::Language language)
{
  FreezeAnalysisResult result{};
  const bool freezeLike = input.is_hang_like || input.is_snapshot_like || input.is_manual_capture;
  if (!freezeLike) {
    return result;
  }

  result.has_analysis = true;
  if (input.blackbox.has_value()) {
    result.blackbox_context = *input.blackbox;
  } else {
    result.blackbox_context.loading_window = input.loading_context;
  }
  if (input.first_chance.has_value()) {
    result.first_chance_context = *input.first_chance;
  }
  if (input.thread_module_consensus.has_value()) {
    result.thread_module_consensus = *input.thread_module_consensus;
  }

  const bool loadingSignal = input.loading_context || (input.wct && input.wct->isLoading);
  const bool hasBlackboxChurn = input.blackbox.has_value() &&
    (input.blackbox->module_churn_score > 0u || input.blackbox->thread_churn_score > 0u);
  const bool strongFirstChanceLoaderSignal = HasStrongFirstChanceLoaderSignal(input, loadingSignal);
  const bool consensusBackedDeadlock = HasConsensusBackedDeadlock(input);
  const bool consensusBackedLoaderSignal = HasConsensusBackedLoaderSignal(input, strongFirstChanceLoaderSignal);
  const bool strongLoaderContext =
    (loadingSignal && input.blackbox.has_value() && input.blackbox->module_churn_score >= 3u) ||
    strongFirstChanceLoaderSignal;
  // Capture quality only; a thread-group consensus is reported by state_id and
  // thread_module_consensus, and must not hide a snapshot fallback.
  result.support_quality = DetermineSupportQuality(input, consensusBackedDeadlock, consensusBackedLoaderSignal);
  // A capture taken while the main thread still answered (a manual snapshot
  // during a load that was still progressing) is no stuck load. Without the
  // helper's heartbeat age (it writes thresholdSec beside it) the load counts
  // as stuck, as before.
  const bool heartbeatFresh = input.wct && input.wct->has_capture && input.wct->thresholdSec > 0u &&
    input.wct->secondsSinceHeartbeat < internal::kNotHangHeartbeatAgeSec;
  const bool loaderStallPossible = loadingSignal && (input.is_hang_like || !heartbeatFresh);

  const bool modalWait = input.modal_dialog_wait.has_value() && input.modal_dialog_wait->detected;
  const MainThreadLockOwnerInfo* lockOwner =
    (!modalWait && input.lock_owner.has_value() && input.lock_owner->tid != 0u) ? &*input.lock_owner : nullptr;
  const CrashLoggerThreadDumpDeadlockInfo* threadDumpDeadlock =
    (lockOwner && input.crash_logger_thread_dump_deadlock.has_value() && input.crash_logger_thread_dump_deadlock->detected)
      ? &*input.crash_logger_thread_dump_deadlock
      : nullptr;
  const EngineSpinInfo* engineSpinNan =
    (input.engine_spin.has_value() && input.engine_spin->nan_detected) ? &*input.engine_spin : nullptr;
  const EngineLockDeadlockInfo* engineLockDeadlock =
    (input.engine_lock_deadlock.has_value() && input.engine_lock_deadlock->detected) ? &*input.engine_lock_deadlock
                                                                                      : nullptr;
  if (modalWait) {
    // Checked first: the main thread is the heartbeat source, so a dialog it
    // is pumping fully explains the stall regardless of other wait chains.
    const auto& modal = *input.modal_dialog_wait;
    result.state_id = "modal_dialog_wait";
    result.confidence_level = (modal.window_evidence && modal.stack_evidence)
      ? i18n::ConfidenceLevel::kHigh
      : i18n::ConfidenceLevel::kMedium;
    AddModalDialogReasons(modal, language, &result);
  } else if (threadDumpDeadlock) {
    // Crash Logger's thread dump suspended the main thread's lock owner and
    // then waited on a lock that owner held: no mod on the stacks is the cause.
    result.state_id = "crash_logger_thread_dump_deadlock";
    result.confidence_level = CrashLoggerThreadDumpDeadlockConfidence(*lockOwner, *threadDumpDeadlock);
    AddCrashLoggerThreadDumpDeadlockReasons(*lockOwner, *threadDumpDeadlock, language, &result);
  } else if (engineLockDeadlock) {
    // Game threads deadlocked on the engine's own locks: the freeze is inside
    // the engine, and no mod on the stacks is shown to have caused it.
    result.state_id = "engine_lock_deadlock";
    result.confidence_level = i18n::ConfidenceLevel::kMedium;
    AddEngineLockDeadlockReasons(*engineLockDeadlock, language, &result);
  } else if (engineSpinNan) {
    // The thread the main thread waits on (or the main thread) is stuck in an
    // engine loop that a NaN value never lets end. Which mod produced the
    // value is not in the dump, so no single mod is named.
    result.state_id = "engine_spin_nan";
    result.confidence_level = i18n::ConfidenceLevel::kMedium;
    AddEngineSpinNanReasons(*engineSpinNan, language, &result);
  } else if (input.wct && input.wct->cycles > 0) {
    result.state_id = "deadlock_likely";
    const bool repeatedCycleSupport =
      consensusBackedDeadlock || (input.wct->cycle_consensus && input.wct->longest_wait_tid_consensus);
    result.confidence_level = repeatedCycleSupport
      ? i18n::ConfidenceLevel::kHigh
      : i18n::ConfidenceLevel::kMedium;
    result.primary_reasons.push_back(
      language == i18n::Language::kEnglish
        ? L"WCT reported cycle threads"
        : L"WCT에서 cycle thread가 감지됨");
    if (input.wct->cycle_consensus && !input.wct->repeated_cycle_tids.empty()) {
      result.primary_reasons.push_back(
        language == i18n::Language::kEnglish
          ? (L"Repeated WCT captures preserved the same cycle thread set (count=" +
              std::to_wstring(input.wct->repeated_cycle_tids.size()) + L")")
          : (L"반복 WCT 캡처에서 같은 cycle thread 집합이 유지됨 (count=" +
              std::to_wstring(input.wct->repeated_cycle_tids.size()) + L")"));
    }
    if (input.wct->longest_wait_tid != 0u) {
      result.primary_reasons.push_back(
        language == i18n::Language::kEnglish
          ? L"A blocked thread with the longest observed wait was identified"
          : L"가장 오래 기다린 blocked thread가 식별됨");
      if (input.wct->longest_wait_tid_consensus) {
        result.primary_reasons.push_back(
          language == i18n::Language::kEnglish
            ? L"The longest-wait thread stayed stable across repeated WCT captures"
            : L"반복 WCT 캡처에서도 longest-wait thread가 유지됨");
      }
    }
  } else if (result.thread_module_consensus.has_consensus) {
    result.state_id = "synchronization_stall_likely";
    result.confidence_level = i18n::ConfidenceLevel::kMedium;
    result.primary_reasons.push_back(
      language == i18n::Language::kEnglish
        ? (L"The game main thread and " +
            std::to_wstring(result.thread_module_consensus.matching_thread_count - 1u) +
            L" other threads retained " + result.thread_module_consensus.module_filename +
            L" near their active stacks")
        : (L"게임 메인 스레드와 다른 " +
            std::to_wstring(result.thread_module_consensus.matching_thread_count - 1u) +
            L"개 스레드의 현재 스택 상단에 " + result.thread_module_consensus.module_filename +
            L"이(가) 반복됨"));
    result.primary_reasons.push_back(
      language == i18n::Language::kEnglish
        ? (L"All " + std::to_wstring(result.thread_module_consensus.stable_thread_count) +
            L" matching threads had zero context-switch delta across repeated WCT captures")
        : (L"일치한 " + std::to_wstring(result.thread_module_consensus.stable_thread_count) +
            L"개 스레드 모두 반복 WCT 캡처 사이 context-switch 변화가 없음"));
    result.primary_reasons.push_back(
      language == i18n::Language::kEnglish
        ? L"This supports a logical synchronization stall, but WCT did not prove an OS lock cycle"
        : L"논리적 동기화 정지를 뒷받침하지만 WCT가 OS 잠금 사이클을 입증한 것은 아님");
  } else if (loaderStallPossible) {
    result.state_id = "loader_stall_likely";
    result.confidence_level = (strongLoaderContext || consensusBackedLoaderSignal)
      ? i18n::ConfidenceLevel::kHigh
      : i18n::ConfidenceLevel::kMedium;
    result.primary_reasons.push_back(
      language == i18n::Language::kEnglish
        ? L"Freeze was captured in a loading context"
        : L"로딩 문맥에서 프리징이 캡처됨");
    if (consensusBackedLoaderSignal) {
      result.primary_reasons.push_back(
        language == i18n::Language::kEnglish
          ? L"Repeated WCT captures stayed aligned with the loading window"
          : L"반복 WCT 캡처가 동일한 로딩 윈도우와 계속 일치함");
    } else if (input.wct && input.wct->has_capture) {
      result.primary_reasons.push_back(
        language == i18n::Language::kEnglish
          ? L"WCT capture metadata stayed consistent with a loading stall"
          : L"WCT 캡처 메타데이터가 로딩 stall 정황과 일치함");
    }
    if (input.blackbox.has_value()) {
      if (input.blackbox->module_churn_score > 0u) {
        result.primary_reasons.push_back(
          language == i18n::Language::kEnglish
            ? (L"blackbox module churn rose during loading (score=" + std::to_wstring(input.blackbox->module_churn_score) + L")")
            : (L"로딩 중 blackbox module churn이 증가함 (score=" + std::to_wstring(input.blackbox->module_churn_score) + L")"));
      }
      if (input.blackbox->thread_churn_score > 0u) {
        result.primary_reasons.push_back(
          language == i18n::Language::kEnglish
            ? (L"thread churn accompanied the loading stall (score=" + std::to_wstring(input.blackbox->thread_churn_score) + L")")
            : (L"thread churn이 로딩 stall과 함께 관찰됨 (score=" + std::to_wstring(input.blackbox->thread_churn_score) + L")"));
      }
    }
    if (strongFirstChanceLoaderSignal) {
      // HasStrongFirstChanceLoaderSignal() already requires first_chance to hold
      // a value, but clang-tidy's optional dataflow does not cross that call, so
      // it reports the dereferences below as unchecked.
      // NOLINTBEGIN(bugprone-unchecked-optional-access)
      result.primary_reasons.push_back(
        language == i18n::Language::kEnglish
          ? (L"repeated suspicious first-chance exceptions appeared during loading (count=" +
              std::to_wstring(input.first_chance->loading_window_count) + L")")
          : (L"로딩 중 반복적인 suspicious first-chance 예외가 관찰됨 (count=" +
              std::to_wstring(input.first_chance->loading_window_count) + L")"));
      if (!input.first_chance->recent_non_system_modules.empty()) {
        result.primary_reasons.push_back(
          language == i18n::Language::kEnglish
            ? (L"first-chance context highlighted non-system modules: " +
                JoinModules(input.first_chance->recent_non_system_modules, 3))
            : (L"first-chance 문맥에서 비시스템 모듈이 강조됨: " +
                JoinModules(input.first_chance->recent_non_system_modules, 3)));
      }
      // NOLINTEND(bugprone-unchecked-optional-access)
    }
  } else if (lockOwner && lockOwner->confirmed) {
    // Both WCT passes showed the main thread blocked on a lock that the same
    // other thread held: the stall is on that thread, and the modules on the
    // main thread's stack only called into the wait.
    result.state_id = "lock_owner_wait";
    result.confidence_level = i18n::ConfidenceLevel::kMedium;
    result.primary_reasons.push_back(
      language == i18n::Language::kEnglish
        ? L"Both WCT passes showed the main thread blocked on a lock held by the same other thread"
        : L"WCT 두 번 모두 메인 스레드가 같은 스레드의 잠금에 막혀 있었음");
  } else if (!input.actionable_candidates.empty()) {
    result.state_id = "freeze_candidate";
    result.confidence_level = i18n::ConfidenceLevel::kLow;
    result.primary_reasons.push_back(
      language == i18n::Language::kEnglish
        ? L"Freeze signals exist but do not isolate a deadlock or loader stall"
        : L"프리징 신호는 있으나 데드락/로더 stall로 단정되지는 않음");
    if (hasBlackboxChurn) {
      result.primary_reasons.push_back(
        language == i18n::Language::kEnglish
          ? L"blackbox captured module/thread churn, but not inside a decisive loading window"
          : L"blackbox에서 module/thread churn이 보였지만 결정적인 로딩 윈도우와는 맞지 않음");
    }
  } else {
    result.state_id = "freeze_ambiguous";
    result.confidence_level = i18n::ConfidenceLevel::kLow;
    result.primary_reasons.push_back(
      language == i18n::Language::kEnglish
        ? L"Freeze evidence was too weak or conflicting"
        : L"프리징 근거가 약하거나 서로 충돌함");
    if (hasBlackboxChurn) {
      result.primary_reasons.push_back(
        language == i18n::Language::kEnglish
          ? L"blackbox saw churn, but not enough to distinguish deadlock from loader stall"
          : L"blackbox churn은 있으나 deadlock과 loader stall을 구분할 정도는 아님");
    }
  }

  if (loadingSignal && !loaderStallPossible &&
      (result.state_id == "freeze_candidate" || result.state_id == "freeze_ambiguous")) {
    result.primary_reasons.push_back(
      language == i18n::Language::kEnglish
        ? L"Captured during loading while the main thread was still responding, so the load was not stuck at that moment"
        : L"로딩 중에 캡처됐지만 메인 스레드가 아직 응답하고 있었으므로, 그 시점에 로딩이 멈춘 것은 아님");
  }
  if (!modalWait && input.main_thread_wait.has_value()) {
    AddMainThreadWaitReason(*input.main_thread_wait, lockOwner, language, &result);
  }
  result.confidence = i18n::ConfidenceText(language, result.confidence_level);
  std::unordered_set<std::wstring> seenNames;
  // Under a lock-owner wait, the thread-dump deadlock or an engine NaN loop
  // the stack candidates are bystanders, not related to the stall.
  const bool stackCandidatesAreBystanders =
    IsLockOwnerFreezeState(result.state_id) || engineSpinNan != nullptr || engineLockDeadlock != nullptr;
  const std::size_t maxCandidates =
    stackCandidatesAreBystanders ? 0u : std::min<std::size_t>(input.actionable_candidates.size(), 2u);
  for (std::size_t i = 0; i < maxCandidates; ++i) {
    auto related = ToRelatedCandidate(input.actionable_candidates[i], language);
    seenNames.insert(related.display_name);
    result.related_candidates.push_back(std::move(related));
  }
  if (modalWait || threadDumpDeadlock || engineSpinNan || engineLockDeadlock) {
    // Recent module loads are timeline noise next to a dialog the user can
    // see, a deadlock Crash Logger's thread dump caused, or an engine loop
    // that no module on the stacks is shown to have caused.
    return result;
  }
  if (input.blackbox.has_value()) {
    for (const auto& moduleName : input.blackbox->recent_non_system_modules) {
      if (moduleName.empty() || seenNames.contains(moduleName)) {
        continue;
      }
      result.related_candidates.push_back(ToRelatedModuleCandidate(moduleName, language));
      seenNames.insert(moduleName);
      if (result.related_candidates.size() >= 4u) {
        break;
      }
    }
  }
  if (input.first_chance.has_value()) {
    for (const auto& moduleName : input.first_chance->recent_non_system_modules) {
      if (moduleName.empty() || seenNames.contains(moduleName)) {
        continue;
      }
      result.related_candidates.push_back(ToRelatedModuleCandidate(moduleName, language));
      seenNames.insert(moduleName);
      if (result.related_candidates.size() >= 4u) {
        break;
      }
    }
  }
  return result;
}

}  // namespace skydiag::dump_tool
