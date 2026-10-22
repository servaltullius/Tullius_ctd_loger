#include "MainThreadWait.h"

#include <algorithm>
#include <cwchar>
#include <string>

namespace skydiag::dump_tool {
namespace {

std::wstring LowerAscii(std::wstring_view value)
{
  std::wstring out(value);
  for (auto& ch : out) {
    if (ch >= L'A' && ch <= L'Z') {
      ch = static_cast<wchar_t>(ch + (L'a' - L'A'));
    }
  }
  return out;
}

template <std::size_t N>
bool Contains(const std::wstring_view (&names)[N], std::wstring_view value)
{
  return std::find(std::begin(names), std::end(names), value) != std::end(names);
}

// Export names, so they resolve without PDBs, plus the internal names that
// appear when public symbols are available.
constexpr std::wstring_view kSleepApis[] = {
  L"NtDelayExecution", L"ZwDelayExecution", L"RtlDelayExecution", L"SleepEx", L"Sleep",
  L"NtYieldExecution", L"ZwYieldExecution", L"SwitchToThread",
};
constexpr std::wstring_view kSyncApis[] = {
  L"NtWaitForSingleObject", L"ZwWaitForSingleObject", L"NtWaitForMultipleObjects", L"ZwWaitForMultipleObjects",
  L"WaitForSingleObject", L"WaitForSingleObjectEx", L"WaitForMultipleObjects", L"WaitForMultipleObjectsEx",
  L"NtWaitForAlertByThreadId", L"ZwWaitForAlertByThreadId", L"NtWaitForKeyedEvent", L"ZwWaitForKeyedEvent",
  L"RtlSleepConditionVariableSRW", L"RtlSleepConditionVariableCS", L"SleepConditionVariableSRW",
  L"SleepConditionVariableCS", L"RtlWaitOnAddress", L"WaitOnAddress", L"RtlpWaitOnAddress",
  L"RtlpWaitOnCriticalSection", L"RtlpEnterCriticalSectionContended", L"RtlEnterCriticalSection",
  L"RtlAcquireSRWLockExclusive", L"RtlAcquireSRWLockShared", L"NtRemoveIoCompletion", L"ZwRemoveIoCompletion",
  L"GetQueuedCompletionStatus", L"GetQueuedCompletionStatusEx", L"NtUserMsgWaitForMultipleObjectsEx",
  L"MsgWaitForMultipleObjects", L"MsgWaitForMultipleObjectsEx", L"NtSignalAndWaitForSingleObject",
  L"SignalObjectAndWait",
};
// Export-only symbols name any address after an export by that export; keep
// the match close to the entry point.
constexpr std::uint64_t kMaxWaitApiDisplacement = 0x2000;

constexpr std::wstring_view kGraphicsDrivers[] = {
  L"nvwgf2umx.dll", L"nvwgf2um.dll", L"nvldumdx.dll", L"nvldumd.dll", L"nvd3dumx.dll", L"nvd3dum.dll",
  L"nvoglv64.dll", L"nvgpucomp64.dll", L"amdxx64.dll", L"atidxx64.dll", L"amdxc64.dll", L"atiumd64.dll",
  L"aticfx64.dll", L"amdihk64.dll", L"igd10iumd64.dll", L"igd12umd64.dll", L"igdumdim64.dll", L"igc64.dll",
  L"igd11dxva64.dll",
};
constexpr std::wstring_view kGraphicsRuntimes[] = {
  L"d3d11.dll", L"d3d12.dll", L"d3d12core.dll", L"dxgi.dll", L"d3d9.dll", L"dxcore.dll",
};

constexpr std::size_t kMaxPathModules = 6;

constexpr double kFileTimeTicksPerSecond = 10'000'000.0;

// A system call stub (ntdll.dll!NtReadFile, win32u.dll!NtUserGetMessage): the
// thread is in the kernel, which for a thread found there is usually a wait.
bool IsSystemCallStub(const ModalStackFrame& frame)
{
  const auto module = LowerAscii(frame.module_filename);
  const std::wstring_view symbol = frame.symbol;
  return (module == L"ntdll.dll" || module == L"win32u.dll") && frame.displacement <= kMaxWaitApiDisplacement &&
         (symbol.starts_with(L"Nt") || symbol.starts_with(L"Zw"));
}

}  // namespace

bool LooksLikeGpuQueryPoll(const std::vector<std::uint8_t>& codeBefore)
{
  const std::size_t n = codeBefore.size();
  // The instruction ending at the return address is the Sleep call:
  // call [rip+disp32] (FF 15 xx xx xx xx) or call rel32 (E8 xx xx xx xx).
  std::size_t callStart = 0;
  if (n >= 6 && codeBefore[n - 6] == 0xFF && codeBefore[n - 5] == 0x15) {
    callStart = n - 6;
  } else if (n >= 5 && codeBefore[n - 5] == 0xE8) {
    callStart = n - 5;
  } else {
    return false;
  }
  // call qword ptr [reg+0E8h]: FF /2 with mod=10 and a 32-bit displacement
  // (0xE8 does not fit a signed 8-bit one). rsp (rm=100) would need a SIB byte.
  // The call must be followed by a check of its HRESULT (GetData returns
  // S_FALSE until the GPU is done): test eax,eax / cmp eax,1. Any other
  // object's vtable slot 29 is unlikely to be checked the same way.
  const auto checksResult = [&](std::size_t at) {
    if (at + 2 <= callStart && codeBefore[at] == 0x85 && codeBefore[at + 1] == 0xC0) {
      return true;  // test eax, eax
    }
    if (at + 3 <= callStart && codeBefore[at] == 0x83 && codeBefore[at + 1] == 0xF8 && codeBefore[at + 2] == 0x01) {
      return true;  // cmp eax, 1
    }
    return false;
  };
  for (std::size_t i = 0; i + 6 <= callStart; ++i) {
    const std::uint8_t modrm = codeBefore[i + 1];
    if (codeBefore[i] == 0xFF && (modrm & 0xF8) == 0x90 && modrm != 0x94 && codeBefore[i + 2] == 0xE8 &&
        codeBefore[i + 3] == 0x00 && codeBefore[i + 4] == 0x00 && codeBefore[i + 5] == 0x00 && checksResult(i + 6)) {
      return true;
    }
  }
  return false;
}

bool IsGraphicsDriverModule(std::wstring_view moduleFilename)
{
  return Contains(kGraphicsDrivers, LowerAscii(moduleFilename));
}

bool IsGraphicsStackModule(std::wstring_view moduleFilename)
{
  const auto lower = LowerAscii(moduleFilename);
  return Contains(kGraphicsDrivers, lower) || Contains(kGraphicsRuntimes, lower);
}

MainThreadWaitInfo ClassifyMainThreadWait(const std::vector<ModalStackFrame>& frames)
{
  MainThreadWaitInfo info{};
  if (frames.empty()) {
    return info;
  }

  // The first frame that is neither Windows nor the graphics stack is the code
  // that made the call the thread is sitting in.
  std::size_t caller = frames.size();
  std::size_t topEnd = frames.size();
  for (std::size_t i = 0; i < frames.size(); ++i) {
    const auto& frame = frames[i];
    if (!frame.has_module) {
      topEnd = i;
      break;
    }
    if (!frame.is_system && !IsGraphicsStackModule(frame.module_filename)) {
      caller = i;
      topEnd = i;
      break;
    }
  }

  const ModalStackFrame* waitFrame = nullptr;
  const ModalStackFrame* graphicsFrame = nullptr;
  bool unnamedSystemFrame = false;
  for (std::size_t i = 0; i < topEnd; ++i) {
    const auto& frame = frames[i];
    if (IsGraphicsStackModule(frame.module_filename)) {
      if (!graphicsFrame ||
          (IsGraphicsDriverModule(frame.module_filename) && !IsGraphicsDriverModule(graphicsFrame->module_filename))) {
        graphicsFrame = &frame;
      }
      continue;
    }
    if (frame.symbol.empty() || frame.displacement > kMaxWaitApiDisplacement) {
      // Without a close name the function is unknown, so it can be neither
      // ruled a wait nor ruled out as one.
      unnamedSystemFrame = true;
      continue;
    }
    // Keep the outermost wait API: "KERNELBASE.dll!SleepEx" reads better
    // than the ntdll system call under it.
    if (Contains(kSleepApis, frame.symbol)) {
      waitFrame = &frame;
      info.wait_class = "sleep";
    } else if (Contains(kSyncApis, frame.symbol)) {
      waitFrame = &frame;
      info.wait_class = "sync";
    }
  }
  if (waitFrame) {
    info.wait_api = waitFrame->module_filename + L"!" + waitFrame->symbol;
  }

  const bool callerKnown = caller < frames.size();
  if (callerKnown && caller == 0u) {
    info.kind = "running";
    info.waiting_module = frames[0].module_filename;
    info.waiting_mod_name = frames[0].inferred_mod_name;
  } else if (graphicsFrame && waitFrame) {
    info.kind = "graphics_driver_wait";
    info.waiting_module = graphicsFrame->module_filename;
  } else if (graphicsFrame && !unnamedSystemFrame) {
    info.kind = "running";
    info.waiting_module = graphicsFrame->module_filename;
  } else if (waitFrame && callerKnown) {
    const auto& callerFrame = frames[caller];
    info.kind = callerFrame.is_game_exe ? "engine_wait" : "plugin_wait";
    info.waiting_module = callerFrame.module_filename;
    info.waiting_mod_name = callerFrame.inferred_mod_name;
    if (callerFrame.is_game_exe && info.wait_class == "sleep" && LooksLikeGpuQueryPoll(callerFrame.code_before)) {
      info.engine_wait_detail = "gpu_query_poll";
    }
  } else if (!waitFrame && !unnamedSystemFrame && callerKnown) {
    info.kind = "running";
    info.waiting_module = frames[caller].module_filename;
    info.waiting_mod_name = frames[caller].inferred_mod_name;
  } else {
    // A wait whose caller the walk did not reach, or system frames without
    // names: not enough to say more.
    info.kind = "unknown";
  }

  const auto waitingLower = LowerAscii(info.waiting_module);
  std::vector<std::wstring> seen;
  for (std::size_t i = callerKnown ? caller : topEnd; i < frames.size(); ++i) {
    const auto& frame = frames[i];
    if (!frame.has_module || frame.is_system || frame.is_game_exe || frame.is_skse_runtime ||
        IsGraphicsStackModule(frame.module_filename)) {
      continue;
    }
    const auto lower = LowerAscii(frame.module_filename);
    if (lower == waitingLower || std::find(seen.begin(), seen.end(), lower) != seen.end()) {
      continue;
    }
    seen.push_back(lower);
    info.path_modules.push_back(frame.module_filename);
    if (info.path_modules.size() >= kMaxPathModules) {
      break;
    }
  }
  return info;
}

bool IsBystanderWait(const MainThreadWaitInfo& wait)
{
  return wait.kind == "engine_wait" || wait.kind == "graphics_driver_wait";
}

bool IsGpuWait(const MainThreadWaitInfo& wait)
{
  return wait.kind == "graphics_driver_wait" ||
         (wait.kind == "engine_wait" && wait.engine_wait_detail == "gpu_query_poll");
}

namespace {

bool IsPluginFrame(const ModalStackFrame& frame)
{
  if (!frame.has_module || frame.is_system || frame.is_game_exe || frame.is_skse_runtime || frame.is_hook_framework ||
      IsGraphicsStackModule(frame.module_filename)) {
    return false;
  }
  return LowerAscii(frame.module_filename) != L"skyrimdiag.dll";
}

std::wstring FrameName(const ModalStackFrame& frame)
{
  if (!frame.has_module) {
    return {};
  }
  return frame.symbol.empty() ? frame.module_filename : (frame.module_filename + L"!" + frame.symbol);
}

// A plugin that runs a game thread's own loop and waits there for work: the
// thread starts, enters the game's thread function (at most a few frames),
// the plugin's hook, and a wait on a sync object. That is the thread idling
// (Hitch Hunter's hooked game thread looked like this in every freeze of the
// 2026-10-12 crash test), not work the main thread is waiting for.
// A walk that stops below the plugin without leaving the game's thread
// function (no unwind data for that plugin build, typically an old dump
// re-analyzed after a mod update) cannot be told apart from that idle loop,
// so it counts as one: the freeze is not pinned on a thread that may be idle.
bool IsPluginOwnedIdleLoop(const std::vector<ModalStackFrame>& frames, std::size_t pluginIndex)
{
  bool syncWaitOnTop = false;
  for (std::size_t i = 0; i < pluginIndex; ++i) {
    if (!frames[i].is_system) {
      return false;
    }
    syncWaitOnTop = syncWaitOnTop || Contains(kSyncApis, frames[i].symbol);
  }
  if (!syncWaitOnTop) {
    return false;
  }
  constexpr std::size_t kMaxGameFramesToThreadStart = 3;
  std::size_t gameFrames = 0;
  for (std::size_t i = pluginIndex + 1; i < frames.size(); ++i) {
    const auto& frame = frames[i];
    if (frame.is_system && (frame.symbol == L"BaseThreadInitThunk" || frame.symbol == L"RtlUserThreadStart")) {
      return true;
    }
    if (!frame.is_game_exe || ++gameFrames > kMaxGameFramesToThreadStart) {
      return false;
    }
  }
  // The walk stopped before the thread start with only the idle-loop shape
  // seen so far (see above).
  return true;
}

struct BlockedCandidate
{
  const ThreadTopFrames* thread = nullptr;
  std::size_t pluginIndex = 0;
  int rank = 0;  // 0 waiting in the plugin, 1 engine called by it, 2 plugin running
};

}  // namespace

void FindBlockedGameThread(
  MainThreadWaitInfo& wait,
  const std::vector<ThreadTopFrames>& gameThreads,
  std::uint32_t lockOwnerTid)
{
  if (wait.kind != "engine_wait" || wait.wait_class != "sync") {
    return;
  }
  std::vector<BlockedCandidate> candidates;
  for (const auto& thread : gameThreads) {
    if (lockOwnerTid != 0u && thread.tid != lockOwnerTid) {
      continue;
    }
    const std::size_t limit = std::min(thread.frames.size(), kBlockedThreadTopFrames);
    for (std::size_t i = 0; i < limit; ++i) {
      if (!IsPluginFrame(thread.frames[i])) {
        continue;
      }
      if (IsPluginOwnedIdleLoop(thread.frames, i)) {
        break;
      }
      bool engineAbove = false;
      bool mayWaitAbove = false;
      for (std::size_t j = 0; j < i; ++j) {
        const auto& above = thread.frames[j];
        engineAbove = engineAbove || above.is_game_exe;
        // A named non-wait call (the clock a spin loop reads, crash test
        // 2026-10-13) is the plugin running; an unnamed one may be a wait,
        // and so may any system call (blocking I/O such as NtReadFile or
        // NtDeviceIoControlFile, a message wait), which the lists leave out.
        mayWaitAbove = mayWaitAbove || above.symbol.empty() || above.displacement > kMaxWaitApiDisplacement ||
                       Contains(kSleepApis, above.symbol) || Contains(kSyncApis, above.symbol) ||
                       IsSystemCallStub(above);
      }
      BlockedCandidate candidate{ &thread, i, i == 0 ? 2 : (engineAbove ? 1 : (mayWaitAbove ? 0 : 2)) };
      candidates.push_back(candidate);
      break;
    }
  }
  if (candidates.empty()) {
    return;
  }
  std::stable_sort(candidates.begin(), candidates.end(), [](const BlockedCandidate& a, const BlockedCandidate& b) {
    return a.rank != b.rank ? a.rank < b.rank : a.pluginIndex < b.pluginIndex;
  });
  const auto& best = candidates.front();
  const auto& plugin = best.thread->frames[best.pluginIndex];
  wait.blocked_thread_tid = best.thread->tid;
  wait.blocked_module = plugin.module_filename;
  wait.blocked_module_path = plugin.module_path;
  wait.blocked_mod_name = plugin.inferred_mod_name;
  wait.blocked_state = best.rank == 0 ? "waiting_in_plugin" : (best.rank == 1 ? "engine_call_from_plugin" : "running_in_plugin");
  // The call the plugin made (KERNELBASE.dll!SleepEx) reads better than the
  // syscall stub on top.
  const auto& topFrame = (best.rank == 0 && best.pluginIndex > 0) ? best.thread->frames[best.pluginIndex - 1]
                                                                   : best.thread->frames.front();
  wait.blocked_top = FrameName(topFrame);
  wait.blocked_thread_count = static_cast<std::uint32_t>(candidates.size());
}

bool HasBlockedGameThread(const MainThreadWaitInfo& wait)
{
  return wait.blocked_thread_tid != 0u && !wait.blocked_module.empty();
}

bool EndsWithSyscall(const std::vector<std::uint8_t>& codeBefore)
{
  const std::size_t n = codeBefore.size();
  return n >= 2u && codeBefore[n - 2u] == 0x0F && codeBefore[n - 1u] == 0x05;
}

bool EndsWithCallInstruction(const std::vector<std::uint8_t>& codeBefore)
{
  const std::size_t n = codeBefore.size();
  if (n < 7u) {
    return false;
  }
  if (codeBefore[n - 5u] == 0xE8) {
    return true;  // call rel32
  }
  // call r/m64 (FF /2): the opcode 2 (call reg), 3 (disp8), 4 (SIB + disp8),
  // 6 (disp32 or rip-relative) or 7 (SIB + disp32) bytes back.
  for (const std::size_t back : { 2u, 3u, 4u, 6u, 7u }) {
    if (codeBefore[n - back] == 0xFF && (codeBefore[n - back + 1u] & 0x38) == 0x10) {
      return true;
    }
  }
  return false;
}

void DescribeLockOwnerFrames(MainThreadLockOwnerInfo& owner, const std::vector<ModalStackFrame>& frames)
{
  constexpr std::size_t kShownFrames = 6;
  owner.top_frames.clear();
  owner.top_module.clear();
  owner.top_mod_name.clear();
  for (const auto& frame : frames) {
    if (owner.top_frames.size() < kShownFrames) {
      if (!frame.has_module) {
        wchar_t buf[32]{};
        std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"0x%llx", static_cast<unsigned long long>(frame.pc));
        owner.top_frames.emplace_back(buf);
      } else if (frame.symbol.empty() || frame.displacement > kMaxWaitApiDisplacement) {
        // An export-only symbol far from pc names some other function.
        owner.top_frames.push_back(frame.module_filename);
      } else {
        wchar_t disp[32]{};
        std::swprintf(disp, sizeof(disp) / sizeof(disp[0]), L"+0x%llx", static_cast<unsigned long long>(frame.displacement));
        owner.top_frames.push_back(frame.module_filename + L"!" + frame.symbol + disp);
      }
    }
    if (owner.top_module.empty() && frame.has_module &&
        (!frame.is_system || IsGraphicsStackModule(frame.module_filename))) {
      owner.top_module = frame.module_filename;
      owner.top_mod_name = frame.inferred_mod_name;
    }
  }
}

std::wstring DescribeLockOwnerThread(const MainThreadLockOwnerInfo& owner, bool en)
{
  std::vector<std::wstring> parts;
  if (!owner.thread_name.empty()) {
    parts.push_back(owner.thread_name);
  }
  if (!owner.top_module.empty()) {
    parts.push_back((en ? L"top module: " : L"맨 위 모듈: ") + owner.top_module);
  }
  std::wstring details;
  for (const auto& part : parts) {
    details += (details.empty() ? L"" : L", ") + part;
  }
  const std::wstring tid = std::to_wstring(owner.tid);
  if (details.empty()) {
    return (en ? L"thread " : L"스레드 ") + tid;
  }
  return en ? (L"thread " + tid + L" (" + details + L")") : (L"스레드 " + tid + L"(" + details + L")");
}

std::wstring DescribeLockType(const MainThreadLockOwnerInfo& owner, bool en)
{
  if (owner.lock_type == "mutex") {
    return en ? L"mutex" : L"뮤텍스";
  }
  return en ? L"critical section" : L"크리티컬 섹션";
}

bool IsCrashLoggerThreadDumpTruncated(const CrashLoggerThreadDumpSignals& signals)
{
  if (!signals.is_thread_dump_log) {
    return false;
  }
  return !signals.thread_dump_has_current_thread ||
         (signals.thread_dump_total_threads > 0u && signals.thread_dump_records < signals.thread_dump_total_threads);
}

bool FreezeStartedAfterThreadDump(std::uint64_t captureTime, double heartbeatAgeSec, std::uint64_t threadDumpStartTime)
{
  // NaN fails the comparison too.
  if (captureTime == 0u || threadDumpStartTime == 0u || !(heartbeatAgeSec >= 0.0)) {
    return false;
  }
  const double freezeStart = static_cast<double>(captureTime) - heartbeatAgeSec * kFileTimeTicksPerSecond;
  return freezeStart >= static_cast<double>(threadDumpStartTime) - kThreadDumpStartToleranceSec * kFileTimeTicksPerSecond;
}

bool ThreadDumpLogStoppedBeforeCapture(std::uint64_t logLastWriteTime, std::uint64_t captureTime)
{
  return logLastWriteTime != 0u && captureTime != 0u && logLastWriteTime < captureTime;
}

CrashLoggerThreadDumpDeadlockInfo EvaluateCrashLoggerThreadDumpDeadlock(
  const MainThreadLockOwnerInfo& owner,
  const CrashLoggerThreadDumpSignals& signals)
{
  CrashLoggerThreadDumpDeadlockInfo info{};
  info.crash_logger_tid = signals.crash_logger_tid;
  info.crash_logger_module = signals.crash_logger_module;
  info.thread_dump_truncated = IsCrashLoggerThreadDumpTruncated(signals);
  info.thread_dump_records = signals.thread_dump_records;
  info.thread_dump_total_threads = signals.thread_dump_total_threads;
  info.thread_dump_before_capture = signals.is_thread_dump_log && signals.log_before_dump;
  info.freeze_started_after_thread_dump = signals.is_thread_dump_log && signals.freeze_started_after_thread_dump;
  info.thread_dump_stopped_before_capture = signals.is_thread_dump_log && signals.log_stopped_before_capture;
  const bool crashLoggerThread = signals.crash_logger_tid != 0u && signals.crash_logger_tid != owner.tid;
  // Outside a system call it can only have been stopped from outside; inside
  // one it may have been, which the other signals then have to carry (medium).
  const bool ownerSuspended = owner.tid != 0u && owner.blocked_without_wait_object &&
    (owner.ip_state == "not_after_syscall" || owner.ip_state == "after_syscall");
  info.detected = crashLoggerThread && ownerSuspended && info.thread_dump_truncated &&
    info.freeze_started_after_thread_dump && info.thread_dump_stopped_before_capture;
  return info;
}

void ApplyCrashLoggerThreadDumpDeadlockVerdict(AnalysisResult& out)
{
  out.suspects.clear();
  auto& wait = out.main_thread_wait;
  wait.blocked_thread_tid = 0;
  wait.blocked_module.clear();
  wait.blocked_module_path.clear();
  wait.blocked_mod_name.clear();
  wait.blocked_state.clear();
  wait.blocked_top.clear();
  wait.blocked_thread_count = 0;
}

i18n::ConfidenceLevel CrashLoggerThreadDumpDeadlockConfidence(
  const MainThreadLockOwnerInfo& owner,
  const CrashLoggerThreadDumpDeadlockInfo& deadlock)
{
  return (owner.ip_state == "not_after_syscall" && owner.confirmed && owner.context_switches_stable &&
          deadlock.thread_dump_before_capture)
    ? i18n::ConfidenceLevel::kHigh
    : i18n::ConfidenceLevel::kMedium;
}

}  // namespace skydiag::dump_tool
