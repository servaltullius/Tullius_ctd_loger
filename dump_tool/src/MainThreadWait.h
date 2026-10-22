#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "Analyzer.h"
#include "ModalDialogWait.h"

namespace skydiag::dump_tool {

// Graphics drivers' user-mode DLLs and the D3D/DXGI runtimes. Drivers can load
// from DriverStore paths that look system-like, so they are matched by name.
bool IsGraphicsStackModule(std::wstring_view moduleFilename);
bool IsGraphicsDriverModule(std::wstring_view moduleFilename);

// Reads the top of the game main thread's stack (innermost first, the frames
// the modal-dialog check uses) and says what the thread was doing: waiting in
// the engine, in a plugin, in the graphics driver, or running code. Called for
// freeze captures that are not modal-dialog waits.
MainThreadWaitInfo ClassifyMainThreadWait(const std::vector<ModalStackFrame>& frames);

// The code before a Sleep call site (the caller frame's code_before, ending at
// the return address) polls a Direct3D 11 query: a call through vtable slot
// 0xE8 (ID3D11DeviceContext::GetData) shortly before the Sleep call. Field
// freezes on 1.6.1170 sat in exactly this loop (SkyrimSE.exe+0xe46e21).
bool LooksLikeGpuQueryPoll(const std::vector<std::uint8_t>& codeBefore);

// A wait that the modules further down the stack did not perform themselves:
// those modules are on the call path, not evidence of the cause.
bool IsBystanderWait(const MainThreadWaitInfo& wait);

// The main thread was waiting for the GPU: inside the graphics driver, or in
// the engine's Direct3D query poll.
bool IsGpuWait(const MainThreadWaitInfo& wait);

// The top of a game-started thread's stack (innermost first).
struct ThreadTopFrames
{
  std::uint32_t tid = 0;
  std::vector<ModalStackFrame> frames;
};

// Frames searched for plugin code on a game thread.
inline constexpr std::size_t kBlockedThreadTopFrames = 12;

// Only for an engine_wait on a sync object (the main thread waiting for other
// game threads): fills wait.blocked_* with the game thread that was inside
// plugin code. Threads waiting inside the plugin come first, then engine code
// the plugin called, then plugin code running; the shallower plugin frame
// wins a tie. The Windows system, the game, the SKSE runtime, the graphics
// stack, known hook frameworks and SkyrimDiag itself are not plugin code.
// When WCT named the thread holding the lock the main thread waits on
// (lockOwnerTid != 0), the main thread waits for that thread only, so no
// other game thread is considered.
void FindBlockedGameThread(
  MainThreadWaitInfo& wait,
  const std::vector<ThreadTopFrames>& gameThreads,
  std::uint32_t lockOwnerTid = 0);

// The main thread waited for another game thread that was inside plugin code.
bool HasBlockedGameThread(const MainThreadWaitInfo& wait);

// The code bytes before an instruction pointer end with a syscall (0F 05): a
// thread stopped there is in a system call. A blocked thread stopped anywhere
// else did not wait by itself; it was suspended.
bool EndsWithSyscall(const std::vector<std::uint8_t>& codeBefore);

// The code bytes before a stack value end with a call instruction (call rel32,
// or call r/m64 in its 2-, 3-, 4-, 6- and 7-byte forms), so the value can be
// a return address. Needs the 7 bytes before it.
bool EndsWithCallInstruction(const std::vector<std::uint8_t>& codeBefore);

// The lock owner's top frames: the first module that is not a Windows DLL
// (graphics drivers and runtimes count), and its frames as text.
void DescribeLockOwnerFrames(MainThreadLockOwnerInfo& owner, const std::vector<ModalStackFrame>& frames);

// "thread 51944 (nv.sl.dlss_g.thread.dlssg, top module: nvwgf2umx.dll)" /
// "스레드 51944(nv.sl.dlss_g.thread.dlssg, 맨 위 모듈: nvwgf2umx.dll)".
std::wstring DescribeLockOwnerThread(const MainThreadLockOwnerInfo& owner, bool en);
// "critical section" / "크리티컬 섹션", "mutex" / "뮤텍스".
std::wstring DescribeLockType(const MainThreadLockOwnerInfo& owner, bool en);

// Both file names carry whole seconds and the heartbeat runs every 100 ms by
// default, so a freeze that began just after the dump started can measure up
// to about 1.1 s before it.
inline constexpr double kThreadDumpStartToleranceSec = 2.0;

// The freeze began (capture time minus the heartbeat age) no earlier than the
// thread dump started, less kThreadDumpStartToleranceSec. Times are FILETIME
// (UTC, 100 ns); 0 or a negative age is unknown and returns false. A player
// who pressed the dump hotkey because the game had already frozen fails this.
bool FreezeStartedAfterThreadDump(std::uint64_t captureTime, double heartbeatAgeSec, std::uint64_t threadDumpStartTime);

// The thread dump log was last written before the capture: it had stopped,
// not still being written. 0 is unknown and returns false.
bool ThreadDumpLogStoppedBeforeCapture(std::uint64_t logLastWriteTime, std::uint64_t captureTime);

// What the dump and the paired Crash Logger log say, for
// EvaluateCrashLoggerThreadDumpDeadlock.
struct CrashLoggerThreadDumpSignals
{
  // A non-main thread with Crash Logger return addresses on its stack.
  std::uint32_t crash_logger_tid = 0;
  std::wstring crash_logger_module;
  bool is_thread_dump_log = false;
  std::uint32_t thread_dump_records = 0;
  std::uint32_t thread_dump_total_threads = 0;
  bool thread_dump_has_current_thread = false;
  bool log_before_dump = false;
  bool freeze_started_after_thread_dump = false;  // FreezeStartedAfterThreadDump
  bool log_stopped_before_capture = false;        // ThreadDumpLogStoppedBeforeCapture
};

// A Crash Logger thread dump that stopped early: fewer thread records than
// its "Total Threads" line, or without the [CURRENT THREAD] record it writes
// last. Not "the last record is the stuck thread": the log is written through
// a 4 KiB buffer, so the stuck thread is usually past the last record.
bool IsCrashLoggerThreadDumpTruncated(const CrashLoggerThreadDumpSignals& signals);

// crash_logger_thread_dump_deadlock: a Crash Logger thread, the main thread's
// lock owner blocked without a wait object of its own (suspended from outside;
// its code readable), a paired thread dump that stopped early and before the
// capture, and a freeze that began after the dump started. Crash Logger's idle
// hotkey thread also carries Crash Logger return addresses, so the thread
// alone proves little; the log and the timing carry the verdict.
// MINIDUMP_THREAD.SuspendCount is no evidence (DbgHelp records 0 for threads
// another thread suspended).
CrashLoggerThreadDumpDeadlockInfo EvaluateCrashLoggerThreadDumpDeadlock(
  const MainThreadLockOwnerInfo& owner,
  const CrashLoggerThreadDumpSignals& signals);

// After a crash_logger_thread_dump_deadlock verdict: Crash Logger stopped the
// game, so neither the main thread's stack (suspects) nor a game thread it
// may have waited for (wait.blocked_*) names a mod.
void ApplyCrashLoggerThreadDumpDeadlockVerdict(AnalysisResult& out);

// High when the owner was not in a system call, both WCT passes named it, it
// made no context switch between them and the thread dump started before the
// capture; else medium (an owner suspended inside a system call is medium).
// Unlike the suspend itself, the PDB lock is inferred, not read from the dump.
i18n::ConfidenceLevel CrashLoggerThreadDumpDeadlockConfidence(
  const MainThreadLockOwnerInfo& owner,
  const CrashLoggerThreadDumpDeadlockInfo& deadlock);

}  // namespace skydiag::dump_tool
