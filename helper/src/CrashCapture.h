#pragma once

#ifdef _WIN32
#include <Windows.h>
#else
#include <cstdint>
using DWORD = std::uint32_t;
#endif

#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "SkyrimDiagCloseRequest.h"
#include "SkyrimDiagCrashCodes.h"

namespace skydiag {
struct SharedHeader;
struct SharedLayout;
}

namespace skydiag::helper {
struct AttachedProcess;
struct HelperConfig;
}

namespace skydiag::helper::internal {

struct PendingCrashAnalysis;
struct PendingCrashEtwCapture;

struct CrashEventInfo {
  std::uint32_t exceptionCode = 0;
  std::uint64_t exceptionAddr = 0;
  std::uint32_t faultingTid = 0;
  std::uint32_t stateFlags = 0;
  std::uint32_t crashSeq = 0;
  bool isStrong = false;
  bool inMenu = false;
};

enum class CleanExitDumpState {
  kPendingDelete,
  kDiscarded,
  kPreserved,
  kDeleteFailed,
  kNotCaptured,
};

struct CleanExitDumpIdentity {
  bool valid = false;
  std::wstring filename;
  std::uint64_t sizeBytes = 0;
  std::uint64_t lastWriteTimeUtc100ns = 0;
};

// Helper-owned state for one captured crash generation. Keeping the immutable
// CrashEventInfo snapshot beside the latch lets the later process-exit path
// describe the same incident even after shared memory changes or disappears.
// Late recovery: after a crash is kept, the game may keep running because
// the exception was handled after all (the filters saw no heartbeat while
// the main thread was busy, e.g. in a long load). A real CTD ends the
// process well within a minute even with Crash Logger writing its log.
inline constexpr std::uint64_t kLateRecoveryMinAliveMs = 60'000;
inline constexpr std::uint64_t kLateRecoveryWindowMs = 5'000;
inline constexpr std::uint32_t kLateRecoveryRequiredWindows = 6;  // 30 s of advancing heartbeat

struct LateRecoveryState {
  std::uint64_t latchedTick64 = 0;
  std::uint64_t lastSampleTick64 = 0;
  std::uint64_t lastHeartbeatQpc = 0;
  std::uint32_t advancingWindows = 0;
};

inline void StartLateRecovery(LateRecoveryState* s, std::uint64_t nowTick64, std::uint64_t heartbeatQpc) noexcept
{
  if (!s) {
    return;
  }
  *s = LateRecoveryState{ nowTick64, nowTick64, heartbeatQpc, 0 };
}

// Samples the heartbeat every kLateRecoveryWindowMs. True once the process
// has stayed alive kLateRecoveryMinAliveMs since the crash was kept and the
// heartbeat advanced in each of the last kLateRecoveryRequiredWindows windows;
// a window without progress (a pause, a stall) starts the count again.
inline bool AdvanceLateRecovery(LateRecoveryState* s, std::uint64_t nowTick64, std::uint64_t heartbeatQpc) noexcept
{
  if (!s || s->latchedTick64 == 0 || nowTick64 < s->lastSampleTick64 ||
      nowTick64 - s->lastSampleTick64 < kLateRecoveryWindowMs) {
    return false;
  }
  s->advancingWindows = heartbeatQpc > s->lastHeartbeatQpc ? s->advancingWindows + 1u : 0u;
  s->lastSampleTick64 = nowTick64;
  s->lastHeartbeatQpc = heartbeatQpc;
  return s->advancingWindows >= kLateRecoveryRequiredWindows &&
         nowTick64 - s->latchedTick64 >= kLateRecoveryMinAliveMs;
}

// Crash Logger 1.25 writes a thread dump on its own hotkey (Ctrl+Shift+F12 by
// default, also the manual capture hotkey up to v0.2.68-rc3), and probes on
// each thread it dumps. A thread that threw and caught a C++ exception (or hit
// a breakpoint) in the last minute then gets that exception recorded as a
// crash (v0.2.68-rc1/rc2 captures, 2026-10-18: one or two per press, each a
// 600 MB dump that the heartbeat filter deleted again). Only those codes reach
// the crash record through Crash Logger's probes in the default
// CrashHookMode=1. The window runs from when Crash Logger's hotkey was last
// seen held (ManualCaptureKeyWatcher), whatever the manual capture hotkey is.
inline constexpr std::uint64_t kCrashLoggerThreadDumpWindowMs = 60000;

constexpr bool IsInCrashLoggerThreadDumpWindow(
  std::uint32_t exceptionCode,
  std::uint64_t nowTick64,
  std::uint64_t lastCrashLoggerHotkeyTick64) noexcept
{
  return lastCrashLoggerHotkeyTick64 != 0u && nowTick64 >= lastCrashLoggerHotkeyTick64 &&
         nowTick64 - lastCrashLoggerHotkeyTick64 <= kCrashLoggerThreadDumpWindowMs &&
         (exceptionCode == 0xE06D7363u || exceptionCode == 0x80000003u);
}

// Crash Logger's thread dump probes the other threads from its own threads,
// and a kept exception is recorded only when a probe faults on the thread
// that threw it. The game's main thread is only a suspended target there, so
// a fault recorded on it is not a probe but a crash Crash Logger is reporting
// from its handler on that thread, which can end the game within seconds. A
// main thread id of 0 (not known from the blackbox) keeps the window.
constexpr bool IsLikelyCrashLoggerThreadDumpFault(
  std::uint32_t exceptionCode,
  std::uint64_t nowTick64,
  std::uint64_t lastCrashLoggerHotkeyTick64,
  std::uint32_t faultingTid,
  std::uint32_t mainThreadId) noexcept
{
  return IsInCrashLoggerThreadDumpWindow(exceptionCode, nowTick64, lastCrashLoggerHotkeyTick64) &&
         (mainThreadId == 0u || faultingTid != mainThreadId);
}

// When Crash Logger's thread dump hotkey was last seen held (GetTickCount64),
// for the check above.
void NoteCrashLoggerThreadDumpHotkeyPressed(std::uint64_t tick64) noexcept;

// By the time a crash is latched the heartbeat has usually stopped for about
// a hang threshold already (the filters' waits, the dump), and a game that is
// still in its crash keeps it stopped. That is the captured crash, not a
// second incident: hang detection waits until the heartbeat moves past where
// it was when the crash was latched. A real freeze after an exception that
// was handled after all is still detected, since the heartbeat moved first.
constexpr bool ShouldHoldHangDetectionForCapturedCrash(
  bool latched,
  std::uint64_t latchedHeartbeatQpc,
  std::uint64_t heartbeatQpc) noexcept
{
  return latched && heartbeatQpc <= latchedHeartbeatQpc;
}

// A fault the heartbeat filter releases as handled is often one that the game
// or a mod raises and catches again and again (e.g. an SEH-guarded probe), and
// MiniDumpWriteDump suspends every game thread for ~1 s while a dump is
// written. Once the same code at the same address was released
// kRepeatedRecoveredFaultMinCount times in this session (main thread included),
// later occurrences check recovery first instead of writing a dump upfront: if
// the game recovers nothing is written, if it stalls while still alive the dump
// is written then, and if it already exited only post-exit exception metadata
// is recorded (ADR-0031). First-time faults still write the dump first.
inline constexpr std::size_t kRecoveredFaultHistoryCapacity = 16;
inline constexpr std::uint32_t kRepeatedRecoveredFaultMinCount = 2;

struct RecoveredFaultHistory
{
  struct Entry
  {
    std::uint32_t exceptionCode = 0;
    std::uint64_t exceptionAddr = 0;
    std::uint32_t count = 0;
  };
  Entry entries[kRecoveredFaultHistoryCapacity]{};
  std::size_t nextReplace = 0;
};

constexpr void NoteRecoveredFault(
  RecoveredFaultHistory* history,
  std::uint32_t exceptionCode,
  std::uint64_t exceptionAddr) noexcept
{
  if (!history) {
    return;
  }
  for (auto& entry : history->entries) {
    if (entry.count != 0u && entry.exceptionCode == exceptionCode && entry.exceptionAddr == exceptionAddr) {
      ++entry.count;
      return;
    }
  }
  for (auto& entry : history->entries) {
    if (entry.count == 0u) {
      entry = { exceptionCode, exceptionAddr, 1u };
      return;
    }
  }
  // Full: replace in turn, so a long session keeps tracking new faults.
  history->entries[history->nextReplace] = { exceptionCode, exceptionAddr, 1u };
  history->nextReplace = (history->nextReplace + 1u) % kRecoveredFaultHistoryCapacity;
}

// Not for a C++ throw or a breakpoint: their address is always the same
// RaiseException / DebugBreak in KernelBase, so code and address match every
// unrelated throw, not the same fault.
constexpr bool IsRepeatedRecoveredFault(
  const RecoveredFaultHistory& history,
  std::uint32_t exceptionCode,
  std::uint64_t exceptionAddr) noexcept
{
  if (exceptionCode == skydiag::kStatusCppException || exceptionCode == skydiag::kStatusBreakpoint) {
    return false;
  }
  for (const auto& entry : history.entries) {
    if (entry.count != 0u && entry.exceptionCode == exceptionCode && entry.exceptionAddr == exceptionAddr) {
      return entry.count >= kRepeatedRecoveredFaultMinCount;
    }
  }
  return false;
}

struct ReleasedDumpContext;  // CaptureCommon.h

struct CrashCaptureState {
  bool latched = false;
  // The heartbeat when the crash was latched
  // (ShouldHoldHangDetectionForCapturedCrash).
  std::uint64_t latchedHeartbeatQpc = 0;
  CrashEventInfo capturedInfo{};
  bool cleanExitEvidenceWritten = false;
  bool cleanExitEvidenceFinalized = false;
  std::filesystem::path cleanExitEvidencePath;
  CleanExitDumpIdentity cleanExitDumpIdentity{};
  std::wstring cleanExitFilterContext;
  LateRecoveryState lateRecovery{};
  // ADR-0022: the close request this crash followed (FindForcedCloseInSnapshot),
  // and the one the released crash below followed. A forced-close error opens
  // no viewer.
  skydiag::ForcedCloseMatch forcedClose{};
  skydiag::ForcedCloseMatch releasedForcedClose{};
  // The crash generation last released as a handled exception (the game kept
  // running or exited with code 0). Its record stays committed in shared
  // memory, so the exit path must not report it again, unless
  // ShouldReportReleasedCrashAtExit says otherwise. A crash superseded by a
  // newer one (kRetryNewerCrash) is not released.
  std::uint32_t releasedCrashSeq = 0;
  // When that release happened (GetTickCount64), and whether it rested only on
  // the heartbeat advancing for a few seconds, before a dump (Crash Logger's
  // thread dump window or a repeated handled fault) or after it.
  std::uint64_t releasedCrashTick64 = 0;
  bool releasedOnBriefHeartbeat = false;
  // Whether that release was the early one in Crash Logger's thread dump
  // window, and when its crash event came (FILETIME, UTC). There a thread dump
  // probe and a worker's crash that Crash Logger reports look alike, so the
  // exit path records it only with Crash Logger's crash log as evidence
  // (DrainCrashEventBeforeExit).
  bool releasedInThreadDumpWindow = false;
  std::uint64_t releasedCrashEventFileTimeUtc = 0;
  // The dump of a crash released on a few seconds of advancing heartbeat
  // (FilterVerdict::kDeleteRecovered). It is kept until that release is
  // settled, since the exit path may still report the fault
  // (ShouldReportReleasedCrashAtExit): deleted at a zero exit, once the game
  // outlived it (releasedDumpRecovery, as late recovery), or when a newer
  // crash supersedes it; made the crash's dump when the fault is reported.
  std::wstring releasedDumpPath;
  std::uint32_t releasedDumpExceptionCode = 0;
  std::uint64_t releasedDumpExceptionAddr = 0;
  bool releasedDumpPreserve = false;
  LateRecoveryState releasedDumpRecovery{};
  // What the held dump needs when it is made the crash's dump at the exit,
  // read at the release because the game is gone by then: the plugin scan's
  // inputs (the game's folder and modules) and the facts of its incident
  // manifest. The scan itself, which reads every plugin header, runs only
  // then.
  std::shared_ptr<const ReleasedDumpContext> releasedDumpContext;
  // Faults released as handled this session (IsRepeatedRecoveredFault).
  RecoveredFaultHistory recoveredFaults{};
  // The crash generation whose dump could not be written, whether the game
  // had already exited then, and when (GetTickCount64; 0: not noted). Its
  // dump-failure WER hint stays at an exit that follows it. A game that ran
  // on for kLateRecoveryMinAliveMs handled that exception, and a later exit
  // is not its crash (DrainCrashEventBeforeExit).
  std::uint32_t dumpWriteFailedCrashSeq = 0;
  bool dumpWriteFailedProcessExited = false;
  std::uint64_t dumpWriteFailedTick64 = 0;
  // ADR-0032: the crash generation the plugin said was not handled (it
  // reached the top-level exception filter). 0: none.
  std::uint32_t unhandledSignalCrashSeq = 0;
  // The crash generation whose late recovery was held back because that
  // signal is still set (logged once). 0: none.
  std::uint32_t lateRecoveryWithheldCrashSeq = 0;
  // The exit drain made a released crash's dump the crash dump because the
  // plugin reported it as not handled after the release
  // (ShouldReportReleasedCrashOnUnhandledSignal). The exit tick then keeps
  // it whatever the exit code is.
  bool releasedCrashPromotedOnUnhandledSignal = false;
};

// ADR-0032: whether the captured crash is one the plugin reported as not
// handled and that no filter has made run on since. The plugin sets the signal
// only for the exception in the crash record and takes it back when a filter
// under its own resumes the thread; the helper resets it when it lets go of
// the incident. While it is set, the faulting thread is still in the filters
// that end the process (a crash handler writing its log, or showing a dialog),
// however long the main thread's heartbeat goes on: not a handled exception,
// and not a normal exit when the process then ends with code 0.
constexpr bool IsCapturedCrashStillUnhandled(bool crashLatched, bool unhandledSignalSet) noexcept
{
  return crashLatched && unhandledSignalSet;
}

// Whether the late recovery of the captured crash was held back
// (HandleLateRecoveryTick): the game ran on for two minutes with the heartbeat
// advancing while the plugin's signal stayed set.
constexpr bool IsLateRecoveryWithheld(std::uint32_t withheldCrashSeq, std::uint32_t capturedCrashSeq) noexcept
{
  return withheldCrashSeq != 0u && withheldCrashSeq == capturedCrashSeq;
}

// Whether a captured crash stands although the process ended with code 0: it
// is still reported as not handled (IsCapturedCrashStillUnhandled), and the
// game did not run on long enough for its late recovery to come up. Then
// something ended the process with code 0 while the faulting thread was in
// the filters: a filter under the plugin's, or the game closing on another
// thread.
//
// Not once the late recovery was held back. The plugin takes its signal back
// only when a filter answers EXCEPTION_CONTINUE_EXECUTION; code that calls
// UnhandledExceptionFilter from its own __except and then runs on leaves it
// set, and a normal exit long after that is not that exception's crash. Such
// a capture keeps its dump only, as the late recovery would have
// (CleanupCrashArtifactsAfterZeroExit).
constexpr bool DoesCapturedCrashOutliveZeroExit(
  bool crashLatched,
  bool unhandledSignalSet,
  bool lateRecoveryWithheld) noexcept
{
  return IsCapturedCrashStillUnhandled(crashLatched, unhandledSignalSet) && !lateRecoveryWithheld;
}

// Whether an exit with `exitCode` says the captured exception was handled:
// code 0 does, unless the crash outlives it (DoesCapturedCrashOutliveZeroExit).
// The places that act on a captured crash after its verdict judge the exit
// with this: the viewer wait after the dump, the exit tick, the exit's viewer.
// (The verdict itself comes from ClassifyExitCodeVerdictWithContext.)
constexpr bool IsNormalExitForCapturedCrash(std::uint32_t exitCode, bool crashOutlivesZeroExit) noexcept
{
  return exitCode == 0u && !crashOutlivesZeroExit;
}

struct StableSharedSnapshot {
  struct AlignedByteDeleter {
    std::size_t alignment = alignof(std::max_align_t);
    void operator()(std::byte* ptr) const noexcept;
  };

  using Storage = std::unique_ptr<std::byte, AlignedByteDeleter>;

  Storage storage{nullptr, AlignedByteDeleter{}};
  std::size_t byteSize = 0;

  const skydiag::SharedLayout* layout() const noexcept;
  std::size_t size() const noexcept;
};

enum class FilterVerdict {
  kKeepDump,
  kDeleteBenign,
  kDeleteRecovered,
  kRetryNewerCrash,
};

// Bounded retry budget for crash dump writes.
inline constexpr int kDumpWriteAttempts = 3;

// Decides whether dump attempt `attemptIndex` (0-based) should run.
//
// The first attempt always runs. Retries only make sense while the game process
// is alive: MiniDumpWriteDump reads the target address space, so once the
// process exits there is nothing left to capture and further attempts would
// only delay the exit-path handling.
inline bool ShouldAttemptDumpWrite(int attemptIndex, bool processStillActive) noexcept
{
  if (attemptIndex <= 0) {
    return true;
  }
  if (attemptIndex >= kDumpWriteAttempts) {
    return false;
  }
  return processStillActive;
}

// Re-exported from the shared definition so helper quarantine uses the same
// known-benign constants as the plugin's exception filtering. The plugin does
// not currently preserve or rank an earlier fault over a later one.
inline constexpr std::uint32_t kStatusInvalidHandle = skydiag::kStatusInvalidHandle;
inline constexpr std::uint32_t kStatusCppException = skydiag::kStatusCppException;
inline constexpr std::uint32_t kStatusClrException = skydiag::kStatusClrException;
inline constexpr std::uint32_t kStatusBreakpoint = skydiag::kStatusBreakpoint;
inline constexpr std::uint32_t kStatusControlCExit = skydiag::kStatusControlCExit;
inline constexpr std::uint32_t kStateInMenu = 1u << 2;

inline bool IsStrongCrashExceptionCode(std::uint32_t code) noexcept
{
  return skydiag::IsStrongCrashExceptionCode(code);
}

inline CrashEventInfo BuildCrashEventInfo(
  std::uint32_t exceptionCode,
  std::uint64_t exceptionAddr,
  std::uint32_t faultingTid,
  std::uint32_t stateFlags) noexcept
{
  CrashEventInfo info{};
  info.exceptionCode = exceptionCode;
  info.exceptionAddr = exceptionAddr;
  info.faultingTid = faultingTid;
  info.stateFlags = stateFlags;
  info.isStrong = IsStrongCrashExceptionCode(exceptionCode);
  info.inMenu = (stateFlags & kStateInMenu) != 0u;
  return info;
}

inline FilterVerdict ClassifyExitCodeVerdict(
  std::uint32_t exitCode,
  const CrashEventInfo&,
  const std::filesystem::path&) noexcept
{
  if (exitCode == 0) {
    return FilterVerdict::kDeleteBenign;
  }
  return FilterVerdict::kKeepDump;
}

// "crash followed a close request (Alt+F4) 0.8 s earlier", for the helper log.
inline std::wstring DescribeForcedCloseForLog(const skydiag::ForcedCloseMatch& match)
{
  const double ms = match.msBeforeCrash > 0.0 ? match.msBeforeCrash : 0.0;
  const auto tenths = static_cast<std::uint64_t>(std::llround(ms / 100.0));
  return L"crash followed a close request (" + std::wstring(skydiag::CloseRequestKindLabel(match.kind)) + L") " +
    std::to_wstring(tenths / 10u) + L"." + std::to_wstring(tenths % 10u) + L" s earlier";
}

// The close request (ADR-0022) the newest crash in a blackbox snapshot came at
// most kForcedCloseCrashWindowMs after; no match without one.
skydiag::ForcedCloseMatch FindForcedCloseInSnapshot(
  const skydiag::SharedLayout* snapshot,
  std::size_t snapshotBytes) noexcept;

// Not noexcept: queueing copies the path into a std::wstring, which can throw.
inline bool QueueDeferredCrashViewer(
  const std::wstring& dumpPath,
  std::wstring* pendingCrashViewerDumpPath)
{
  if (!pendingCrashViewerDumpPath) {
    return false;
  }
  if (pendingCrashViewerDumpPath->empty()) {
    *pendingCrashViewerDumpPath = dumpPath;
    return true;
  }
  if (*pendingCrashViewerDumpPath == dumpPath) {
    return true;
  }
  return false;
}

inline bool ShouldLatchCrashCapture(FilterVerdict verdict) noexcept
{
  return verdict == FilterVerdict::kKeepDump;
}

inline bool IsCommittedCrashSequence(std::uint32_t crashSeq) noexcept
{
  return crashSeq != 0u && (crashSeq & 1u) == 0u;
}

// Exit code 0 is treated as authoritative proof that an exception was handled,
// which is what keeps handled first-chance exceptions from producing dumps.
// That rule is deliberately absolute, so it also discards the rare genuine CTD
// that exits 0 — a foreign handler calling ExitProcess(0), a launcher
// normalizing the child exit code, or teardown that reaches a normal exit path
// after a fatal fault. The one exception is a crash the plugin reported as not
// handled (ADR-0032; ClassifyExitCodeVerdictWithContext for the verdict,
// IsNormalExitForCapturedCrash after it): there the plugin saw what the exit
// code can only suggest.
//
// This predicate does not change the filter decision. It only marks the subset
// where the filtered incident carried real fault evidence, so a compact metadata
// record can be preserved for investigation. `kDeleteRecovered` covers the
// heartbeat-recovery case separately, so reaching a benign zero-exit verdict
// with a committed strong fault means recovery was never actually observed.
inline bool ShouldQuarantineCleanExitEvidence(
  std::uint32_t exitCode,
  const CrashEventInfo& info) noexcept
{
  return exitCode == 0u && info.isStrong && IsCommittedCrashSequence(info.crashSeq);
}

// A crash released as handled is normally not reported again at exit. A
// release on a few seconds of advancing heartbeat is weak evidence, though,
// before a dump (Crash Logger's thread dump window) as after one: a worker
// thread's crash handler (e.g. Crash Logger writing its log) can run that long
// while the main thread still ticks, and no dump is left either way. So a
// non-zero exit within kLateRecoveryMinAliveMs of such a release still records
// the released fault, unless the game froze after the release (a hang
// captured, the heartbeat stalled at exit, or Windows closed it as not
// responding): a crash handler on a worker thread leaves the heartbeat
// running, while a frozen game that was closed does not. Any non-zero exit
// with the released exception's own code records it too. A release in the
// thread dump window also needs Crash Logger's crash log, since a soft-locked
// game whose thread dump was taken before it was ended looks the same
// (DrainCrashEventBeforeExit).
constexpr bool ShouldReportReleasedCrashAtExit(
  std::uint32_t exitCode,
  std::uint32_t exceptionCode,
  bool releasedOnBriefHeartbeat,
  bool gameFrozeAfterRelease,
  std::uint64_t releasedTick64,
  std::uint64_t exitTick64) noexcept
{
  if (exitCode == 0u) {
    return false;
  }
  if (exitCode == exceptionCode) {
    return true;
  }
  return releasedOnBriefHeartbeat && !gameFrozeAfterRelease && exitTick64 >= releasedTick64 &&
         exitTick64 - releasedTick64 < kLateRecoveryMinAliveMs;
}

// ADR-0032: a crash released as handled whose exception the plugin then
// reported as not handled (it reached the top-level filter only after the
// release; a report between the filter's last look and the release is lost,
// since the release clears the incident's signals).
// The exit records it whatever the exit code is, while the release is younger
// than kLateRecoveryMinAliveMs: an older signal may be one left set by code
// that ran on (DoesCapturedCrashOutliveZeroExit).
constexpr bool ShouldReportReleasedCrashOnUnhandledSignal(
  bool unhandledSignalSet,
  std::uint64_t releasedTick64,
  std::uint64_t exitTick64) noexcept
{
  return unhandledSignalSet && exitTick64 >= releasedTick64 &&
         exitTick64 - releasedTick64 < kLateRecoveryMinAliveMs;
}

// Why a committed fault is recorded only as metadata after the game exited.
enum class PostExitEvidenceReason
{
  // Published after the helper's final live-process poll; never captured.
  kPublishedAfterFinalLivePoll,
  // Captured while the game ran, but its dump could not be written.
  kDumpWriteFailed,
  // Captured, but the game had exited before its dump could be written.
  kDumpWriteFailedProcessExited,
  // Released as handled, but the game then exited abnormally
  // (ShouldReportReleasedCrashAtExit).
  kReleasedFaultFollowedByAbnormalExit,
  // Released as handled, but the plugin then reported it as not handled
  // (ShouldReportReleasedCrashOnUnhandledSignal).
  kReleasedFaultReportedUnhandled,
};

CrashEventInfo ExtractCrashInfo(const skydiag::SharedHeader* shm) noexcept;
bool TryCaptureCommittedCrashInfo(
  const skydiag::SharedHeader* shm,
  CrashEventInfo* out) noexcept;
bool TryWritePostExitCrashEvidenceRecord(
  const std::filesystem::path& outBase,
  const CrashEventInfo& info,
  DWORD exitCode,
  PostExitEvidenceReason reason = PostExitEvidenceReason::kPublishedAfterFinalLivePoll);
bool CaptureStableSharedSnapshot(
  const skydiag::SharedLayout* shm,
  std::size_t shmBytes,
  StableSharedSnapshot* out) noexcept;
bool TryClearRecoveredCrashFreeze(
  skydiag::SharedLayout* shm,
  std::uint32_t expectedCrashSeq) noexcept;
// Lets go of the incident: TryClearRecoveredCrashFreeze, after the incident's
// "unhandled" and "dump settled" signals were reset (ADR-0032). The order
// matters: once the freeze is cleared the plugin can publish the next crash,
// which must not find the signals of this one.
bool ThawCrashCapture(
  const skydiag::helper::AttachedProcess& proc,
  std::uint32_t expectedCrashSeq) noexcept;
// Why the WER LocalDumps hint is being written. The two cases need different
// first steps, so the hint must not claim a failed capture when none was tried.
enum class WerFallbackReason
{
  // A crash was recorded but every dump write failed.
  kDumpWriteFailed,
  // The game exited with a non-zero code and no recorded exception, e.g. a
  // plugin ended it after an error dialog, or abort()/TerminateProcess.
  kAbnormalExitWithoutCrash,
};

inline std::string BuildWerFallbackHintText(WerFallbackReason reason, DWORD exitCode)
{
  std::string text;
  if (reason == WerFallbackReason::kDumpWriteFailed) {
    text =
      "SkyrimDiag recorded a crash but could not write its dump. As a fallback, you can enable "
      "Windows Error Reporting LocalDumps.\n";
  } else {
    char code[16]{};
    std::snprintf(code, sizeof(code), "0x%08X", static_cast<unsigned int>(exitCode));
    text =
      "The game exited abnormally (exit code " + std::to_string(exitCode) + " / " + code +
      ") without an exception that SkyrimDiag records, so no dump was taken. This is not a failed capture.\n"
      "This usually means the game was ended on purpose: a plugin showed an error dialog (for example an "
      "incompatible Address Library or game version) and closed the game, or code called abort() or "
      "TerminateProcess. Look for such a dialog and the plugin's own log first.\n"
      "If the game was crashing instead, Windows Error Reporting LocalDumps can capture it:\n";
  }
  text +=
    "Registry path:\n"
    "  HKLM\\SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting\\LocalDumps\\SkyrimSE.exe\n"
    "Recommended values:\n"
    "  DumpType (DWORD) = 2   ; full dump\n"
    "  DumpCount (DWORD) = 10\n"
    "  DumpFolder (EXPAND_SZ) = <your output folder>\n"
    "Reference: https://learn.microsoft.com/windows/win32/wer/collecting-user-mode-dumps\n";
  return text;
}

void WriteWerFallbackHint(const std::filesystem::path& outBase, WerFallbackReason reason, DWORD exitCode);

// The exit code of a program Windows ended as not responding, after the user
// chose to close it ("stopped interacting with Windows and was closed").
inline constexpr std::uint32_t kExitCodeClosedAsNotResponding = 0xCFFFFFFFu;

// A non-zero exit with no crash on record, after the game had stopped
// responding (a hang was captured in this episode, the heartbeat had stalled
// past the hang threshold, or Windows closed it as not responding), is a
// frozen game being closed (by the user, a watchdog or Windows), or a crash
// that a crash handler ended: Crash Logger ends a crash it reports with exit
// code 1, also one SkyrimDiag does not record, and its log can take longer
// than the hang threshold. Only kExitCodeClosedAsNotResponding tells a close
// apart. The abnormal-exit hint would point at a plugin's error dialog and at
// WER LocalDumps, which captures neither case. It still applies when a dialog
// was on screen at the hang capture (a plugin's error dialog that then ends the
// game), and for an NTSTATUS error exit code such as a fail-fast, which WER
// does capture. A stalled heartbeat does not count when the helper held back
// this episode's hang capture because the game window still answered: a
// plugin's error dialog keeps the window answering while the heartbeat stops.
constexpr bool IsCloseOfUnresponsiveGame(
  std::uint32_t exitCode,
  bool crashOnRecord,
  bool hangCapturedThisEpisode,
  bool modalDialogSeenThisEpisode,
  bool heartbeatStalled,
  bool hangSuppressedAsResponsiveThisEpisode) noexcept
{
  if (exitCode == 0u || crashOnRecord || modalDialogSeenThisEpisode) {
    return false;
  }
  if (exitCode == kExitCodeClosedAsNotResponding) {
    return true;
  }
  return exitCode < 0xC0000000u &&
         (hangCapturedThisEpisode || (heartbeatStalled && !hangSuppressedAsResponsiveThisEpisode));
}

// A hang whose episode was still open when the game exited with code 0: the
// game stopped answering while it shut down (a large load order can take
// longer than the hang threshold to quit), which a frozen game that is ended
// does not do (Task Manager, a watchdog and Windows' not-responding close all
// give a non-zero code). Its viewer is not opened. A dump held for it
// (HeldHangDump, a capture in a menu) is removed unless
// KeepSlowShutdownHangDumps; any other dump stays, as a quit hotkey or a
// launcher can also end a game frozen during play with code 0.
constexpr bool IsHangDuringNormalShutdown(std::uint32_t exitCode, bool hangEpisodeOpenAtExit) noexcept
{
  return exitCode == 0u && hangEpisodeOpenAtExit;
}

// The exit the helper treats as a slow shutdown: its viewer is not opened and
// a held dump (HeldHangDump) is removed. On top of IsHangDuringNormalShutdown:
// - the game was quitting. Either the hang was captured where quitting
//   happens (MayBeSlowShutdown: a menu, no load), or after the capture the
//   game was seen in its exit (one thread left, NoteGameClosingAfterCapture),
//   which a quit that starts outside a menu (Alt+F4 during play) goes through
//   too. A freeze during play that is ended from outside with code 0 (a
//   launcher, TerminateProcess) shows neither and keeps its viewer. One ended
//   by ExitProcess inside the game (a quit hotkey's own thread) can show the
//   second; it then loses only the viewer at the exit, as its dump was
//   analyzed at the capture and stays.
// - the heartbeat was still stalled at the exit, so a freeze the game came
//   back from just before a quick quit (before a hang tick saw the recovery)
//   is not taken for one.
// - no dialog window of the game was visible at the capture. A game that is
//   quitting shows none, and a dump with one says whose it was (a plugin's
//   error message that ends the game with code 0 once it is dismissed).
constexpr bool IsSlowShutdownAtExit(
  std::uint32_t exitCode,
  bool hangEpisodeOpenAtExit,
  bool gameWasQuitting,
  bool heartbeatStalledAtExit,
  bool dialogSeenAtCapture) noexcept
{
  return IsHangDuringNormalShutdown(exitCode, hangEpisodeOpenAtExit) && gameWasQuitting && heartbeatStalledAtExit &&
         !dialogSeenAtCapture;
}

// The held dump of a released crash is renamed from SkyrimDiag_Crash_<ts>.dmp
// to SkyrimDiag_Released_<ts>.dmp while it is held, so one a stopped helper
// leaves behind is not listed as a crash (retention bounds those), and back
// when the crash is reported after all. A path with neither prefix comes back
// unchanged.
inline constexpr std::wstring_view kCrashDumpPrefix = L"SkyrimDiag_Crash_";
inline constexpr std::wstring_view kReleasedDumpPrefix = L"SkyrimDiag_Released_";

inline std::wstring SwapDumpFilenamePrefix(
  const std::filesystem::path& dumpPath,
  std::wstring_view fromPrefix,
  std::wstring_view toPrefix)
{
  const std::wstring filename = dumpPath.filename().wstring();
  if (filename.size() <= fromPrefix.size() || filename.compare(0, fromPrefix.size(), fromPrefix) != 0) {
    return dumpPath.wstring();
  }
  return (dumpPath.parent_path() / (std::wstring(toPrefix) + filename.substr(fromPrefix.size()))).wstring();
}

inline std::wstring ReleasedDumpPathFor(const std::filesystem::path& crashDumpPath)
{
  return SwapDumpFilenamePrefix(crashDumpPath, kCrashDumpPrefix, kReleasedDumpPrefix);
}

inline std::wstring CrashDumpPathForReleased(const std::filesystem::path& releasedDumpPath)
{
  return SwapDumpFilenamePrefix(releasedDumpPath, kReleasedDumpPrefix, kCrashDumpPrefix);
}

// Deletes the held dump of a released crash (releasedDumpPath), unless
// filtered dumps are preserved, logging `reason`, and clears it.
void DiscardReleasedCrashDump(
  const std::filesystem::path& outBase,
  CrashCaptureState* crashState,
  std::wstring_view reason);

bool IsCleanExitEvidenceRequired(
  const skydiag::helper::HelperConfig& cfg,
  const CrashCaptureState* crashState) noexcept;
bool TryWriteCleanExitEvidenceRecord(
  const skydiag::helper::HelperConfig& cfg,
  const std::filesystem::path& outBase,
  CrashCaptureState* crashState,
  std::wstring_view context,
  const std::filesystem::path& dumpPath,
  CleanExitDumpState dumpState);

inline bool ShouldPreserveFilteredDump(
  bool preserveConfigured,
  bool evidenceRequired,
  bool evidenceWritten) noexcept
{
  return preserveConfigured || (evidenceRequired && !evidenceWritten);
}

#ifdef _WIN32
// Waits for the crash event or the process: WAIT_OBJECT_0 for the crash
// event, WAIT_OBJECT_0 + 1 for the process exit (also when there is no crash
// event), WAIT_TIMEOUT or WAIT_FAILED otherwise. The crash event wins when
// both are signaled.
DWORD WaitForCrashOrProcess(HANDLE crashEvent, HANDLE process, DWORD waitMs) noexcept;
// ADR-0032: the same, also woken by the plugin's "unhandled" event
// (kWaitUnhandledSignal). Without that event it is WaitForCrashOrProcess.
// The crash event wins, then the process exit: check
// IsUnhandledExceptionSignaled after either.
inline constexpr DWORD kWaitUnhandledSignal = WAIT_OBJECT_0 + 2u;
DWORD WaitForCrashUnhandledOrProcess(
  HANDLE crashEvent,
  HANDLE unhandledEvent,
  HANDLE process,
  DWORD waitMs) noexcept;
bool IsUnhandledExceptionSignaled(HANDLE unhandledEvent) noexcept;

// The verdict a process exit code gives a captured exception; every reader of
// an exit code that decides a verdict comes through here. ADR-0032: the
// plugin's signal is looked at first, in this one place: an exception it
// reported as not handled is kept whatever code the process ended with (and
// the signal is noted for the incident). Without the signal, code 0 releases
// it as handled. `checkIndex` is the heartbeat check it came from, or -1.
FilterVerdict ClassifyExitCodeVerdictWithContext(
  const skydiag::helper::HelperConfig& cfg,
  std::uint32_t exitCode,
  HANDLE unhandledEvent,
  const CrashEventInfo& info,
  const std::filesystem::path& outBase,
  std::wstring_view context,
  int checkIndex,
  CrashCaptureState* crashState);
#endif

bool HandleCrashEventTick(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  DWORD waitMs,
  CrashCaptureState* crashState,
  PendingCrashEtwCapture* pendingCrashEtw,
  PendingCrashAnalysis* pendingCrashAnalysis,
  std::wstring* lastCrashDumpPath,
  std::wstring* pendingHangViewerDumpPath,
  std::wstring* pendingCrashViewerDumpPath);

}
