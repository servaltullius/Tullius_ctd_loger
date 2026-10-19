#include <cassert>
#include <string>

#include "CrashCapture.h"

using skydiag::helper::internal::BuildCrashEventInfo;
using skydiag::helper::internal::ClassifyExitCodeVerdict;
using skydiag::helper::internal::CrashEventInfo;
using skydiag::helper::internal::FilterVerdict;
using skydiag::helper::internal::IsCloseOfUnresponsiveGame;
using skydiag::helper::internal::IsCommittedCrashSequence;
using skydiag::helper::internal::QueueDeferredCrashViewer;
using skydiag::helper::internal::ShouldLatchCrashCapture;
using skydiag::helper::internal::ShouldAttemptDumpWrite;
using skydiag::helper::internal::ShouldPreserveFilteredDump;
using skydiag::helper::internal::ShouldQuarantineCleanExitEvidence;
using skydiag::helper::internal::ShouldReportReleasedCrashAtExit;
using skydiag::helper::internal::kDumpWriteAttempts;
using skydiag::helper::internal::kExitCodeClosedAsNotResponding;

// A C++ exception or breakpoint within a minute of Crash Logger's thread dump
// hotkey (its own keys, Ctrl+Shift+F12 by default, whatever the manual capture
// hotkey is).
static_assert(skydiag::helper::internal::IsInCrashLoggerThreadDumpWindow(0xE06D7363u, 30000u, 1000u));
static_assert(skydiag::helper::internal::IsInCrashLoggerThreadDumpWindow(0x80000003u, 61000u, 1000u));
static_assert(!skydiag::helper::internal::IsInCrashLoggerThreadDumpWindow(0xE06D7363u, 61001u, 1000u), "a minute later");
static_assert(!skydiag::helper::internal::IsInCrashLoggerThreadDumpWindow(0xC0000005u, 2000u, 1000u), "an access violation");
static_assert(!skydiag::helper::internal::IsInCrashLoggerThreadDumpWindow(0xE06D7363u, 2000u, 0u), "no Crash Logger hotkey");
// ...and not on the game's main thread, which a thread dump never probes from
// (exceptionCode, nowTick64, lastHotkeyTick64, faultingTid, mainThreadId).
static_assert(
  skydiag::helper::internal::IsLikelyCrashLoggerThreadDumpFault(0xE06D7363u, 30000u, 1000u, 53376u, 25652u),
  "a worker thread");
static_assert(
  !skydiag::helper::internal::IsLikelyCrashLoggerThreadDumpFault(0xE06D7363u, 30000u, 1000u, 25652u, 25652u),
  "the main thread");
static_assert(
  skydiag::helper::internal::IsLikelyCrashLoggerThreadDumpFault(0xE06D7363u, 30000u, 1000u, 25652u, 0u),
  "the main thread not known: the window still applies");
static_assert(
  !skydiag::helper::internal::IsLikelyCrashLoggerThreadDumpFault(0xE06D7363u, 61001u, 1000u, 53376u, 25652u),
  "a worker thread a minute later");
static_assert(
  !skydiag::helper::internal::IsLikelyCrashLoggerThreadDumpFault(0xC0000005u, 2000u, 1000u, 53376u, 0u),
  "an access violation on a worker thread");

// A frozen game closed with a non-zero exit gets no abnormal-exit WER hint
// (exitCode, crashOnRecord, hangCaptured, modalDialogSeen, heartbeatStalled,
// hangSuppressedAsResponsive).
static_assert(IsCloseOfUnresponsiveGame(1u, false, true, false, false, false), "closed after a hang capture");
static_assert(IsCloseOfUnresponsiveGame(1u, false, false, false, true, false), "closed with a stalled heartbeat");
static_assert(!IsCloseOfUnresponsiveGame(1u, false, false, false, false, false), "a responsive game");
static_assert(!IsCloseOfUnresponsiveGame(0u, false, true, false, true, false), "a normal exit");
static_assert(!IsCloseOfUnresponsiveGame(3u, false, true, true, true, false), "a dialog at the hang capture");
static_assert(!IsCloseOfUnresponsiveGame(0xC0000409u, false, true, false, true, false), "an NTSTATUS crash exit code");
static_assert(IsCloseOfUnresponsiveGame(0xBFFFFFFFu, false, true, false, true, false), "below the NTSTATUS error range");
static_assert(!IsCloseOfUnresponsiveGame(1u, true, true, false, true, false), "a crash on record");
// The hang capture was held back because the window still answered (e.g. a
// plugin's error dialog while the game was in the background).
static_assert(!IsCloseOfUnresponsiveGame(1u, false, false, false, true, true), "a stalled heartbeat, window answering");
static_assert(IsCloseOfUnresponsiveGame(1u, false, true, false, true, true), "a hang captured after the suppression");
// Windows ended the game as not responding.
static_assert(kExitCodeClosedAsNotResponding == 0xCFFFFFFFu, "the hung-program close exit code");
static_assert(IsCloseOfUnresponsiveGame(0xCFFFFFFFu, false, true, false, true, false), "closed as not responding");
static_assert(IsCloseOfUnresponsiveGame(0xCFFFFFFFu, false, false, false, false, false), "before the hang threshold");
static_assert(!IsCloseOfUnresponsiveGame(0xCFFFFFFFu, true, true, false, true, false), "not responding, crash on record");
static_assert(!IsCloseOfUnresponsiveGame(0xCFFFFFFFu, false, true, true, true, false), "not responding, dialog seen");

// A released crash is reported at a non-zero exit only shortly after a
// brief-heartbeat release when the game did not freeze after it, or for its
// own exception code (exitCode, exceptionCode, releasedOnBriefHeartbeat,
// gameFrozeAfterRelease, releasedTick64, exitTick64).
static_assert(ShouldReportReleasedCrashAtExit(1u, 0xC0000005u, true, false, 100'000u, 107'000u), "7 s after a post-dump release");
static_assert(!ShouldReportReleasedCrashAtExit(1u, 0xC0000005u, true, false, 100'000u, 160'000u), "a minute after it");
static_assert(!ShouldReportReleasedCrashAtExit(0u, 0xC0000005u, true, false, 100'000u, 107'000u), "a normal exit");
static_assert(!ShouldReportReleasedCrashAtExit(1u, 0xC0000005u, false, false, 100'000u, 107'000u), "7 s after a late recovery");
// An early thread-dump release: Crash Logger ending a live game with exit 1
// soon after is recorded; a game that froze after the release (a thread dump
// deadlock, then closed) is not.
static_assert(
  ShouldReportReleasedCrashAtExit(1u, 0xE06D7363u, true, false, 100'000u, 107'000u),
  "an early thread-dump release, then a live game ended 7 s later");
static_assert(
  !ShouldReportReleasedCrashAtExit(1u, 0xE06D7363u, true, true, 100'000u, 107'000u),
  "the game froze after the release");
static_assert(
  !ShouldReportReleasedCrashAtExit(1u, 0xE06D7363u, true, true, 100'000u, 140'000u),
  "a thread dump deadlock closed within a minute");
static_assert(!ShouldReportReleasedCrashAtExit(1u, 0xE06D7363u, true, true, 100'000u, 244'000u), "the rc3 field case");
static_assert(ShouldReportReleasedCrashAtExit(0xC0000005u, 0xC0000005u, false, false, 100'000u, 900'000u), "its own code");
static_assert(
  ShouldReportReleasedCrashAtExit(0xE06D7363u, 0xE06D7363u, true, true, 100'000u, 107'000u),
  "its own code, also after a freeze");

// A latched crash holds hang detection until the heartbeat moves past where
// it was at the latch (latched, latchedHeartbeatQpc, heartbeatQpc).
static_assert(
  skydiag::helper::internal::ShouldHoldHangDetectionForCapturedCrash(true, 500u, 500u),
  "the game is still in its crash");
static_assert(
  !skydiag::helper::internal::ShouldHoldHangDetectionForCapturedCrash(true, 500u, 501u),
  "the heartbeat moved: a freeze from here on is a new incident");
static_assert(
  !skydiag::helper::internal::ShouldHoldHangDetectionForCapturedCrash(false, 500u, 500u),
  "no crash captured");

static void TestClassifyExitCodeVerdict_DeleteBenignOnWeakZeroExit()
{
  const auto info = BuildCrashEventInfo(0u, 0u, 0u, 0u);
  assert(ClassifyExitCodeVerdict(0u, info, {}) == FilterVerdict::kDeleteBenign);
}

static void TestClassifyExitCodeVerdict_DeleteBenignOnStrongZeroExit()
{
  const auto info = BuildCrashEventInfo(
    0xC0000005u,
    0x439Eu,
    17120u,
    skydiag::helper::internal::kStateInMenu);
  assert(info.isStrong);
  assert(info.inMenu);
  assert(ClassifyExitCodeVerdict(0u, info, {}) == FilterVerdict::kDeleteBenign);
}

static void TestClassifyExitCodeVerdict_KeepDumpInMenuNonZeroExit()
{
  const auto info = BuildCrashEventInfo(0u, 0u, 0u, skydiag::helper::internal::kStateInMenu);
  assert(ClassifyExitCodeVerdict(1u, info, {}) == FilterVerdict::kKeepDump);
}

static void TestClassifyExitCodeVerdict_KeepDumpOutsideMenuNonZeroExit()
{
  const auto info = BuildCrashEventInfo(0u, 0u, 0u, 0u);
  assert(ClassifyExitCodeVerdict(1u, info, {}) == FilterVerdict::kKeepDump);
}

static void TestClassifyExitCodeVerdict_DeleteBenignForInvalidHandleWeak()
{
  const auto info = BuildCrashEventInfo(skydiag::helper::internal::kStatusInvalidHandle, 0u, 0u, 0u);
  assert(ClassifyExitCodeVerdict(0u, info, {}) == FilterVerdict::kDeleteBenign);
}

static void TestClassifyExitCodeVerdict_DeleteBenignForCppExceptionWeak()
{
  const auto info = BuildCrashEventInfo(skydiag::helper::internal::kStatusCppException, 0u, 0u, 0u);
  assert(ClassifyExitCodeVerdict(0u, info, {}) == FilterVerdict::kDeleteBenign);
}

static void TestClassifyExitCodeVerdict_DeleteBenignForClrExceptionWeak()
{
  const auto info = BuildCrashEventInfo(skydiag::helper::internal::kStatusClrException, 0u, 0u, 0u);
  assert(ClassifyExitCodeVerdict(0u, info, {}) == FilterVerdict::kDeleteBenign);
}

static void TestClassifyExitCodeVerdict_DeleteBenignForBreakpointWeak()
{
  const auto info = BuildCrashEventInfo(skydiag::helper::internal::kStatusBreakpoint, 0u, 0u, 0u);
  assert(ClassifyExitCodeVerdict(0u, info, {}) == FilterVerdict::kDeleteBenign);
}

static void TestClassifyExitCodeVerdict_DeleteBenignForControlCExitWeak()
{
  const auto info = BuildCrashEventInfo(skydiag::helper::internal::kStatusControlCExit, 0u, 0u, 0u);
  assert(ClassifyExitCodeVerdict(0u, info, {}) == FilterVerdict::kDeleteBenign);
}

static void TestClassifyExitCodeVerdict_KeepDumpForAccessViolationNonZeroExit()
{
  const auto info = BuildCrashEventInfo(0xC0000005u, 0u, 0u, 0u);
  assert(ClassifyExitCodeVerdict(0xC0000005u, info, {}) == FilterVerdict::kKeepDump);
}

static void TestExtractLikeInfo_DefaultValues()
{
  const auto info = BuildCrashEventInfo(0u, 0u, 0u, 0u);
  assert(info.exceptionCode == 0u);
  assert(info.exceptionAddr == 0u);
  assert(info.faultingTid == 0u);
  assert(info.stateFlags == 0u);
  assert(!info.isStrong);
  assert(!info.inMenu);
}

static void TestExtractLikeInfo_FieldMapping()
{
  const auto info = BuildCrashEventInfo(0xC0000005u, 0x12345678ull, 777u, 0u);
  assert(info.exceptionCode == 0xC0000005u);
  assert(info.exceptionAddr == 0x12345678ull);
  assert(info.faultingTid == 777u);
}

static void TestExtractLikeInfo_InMenuFlagSet()
{
  const auto info = BuildCrashEventInfo(0u, 0u, 0u, skydiag::helper::internal::kStateInMenu);
  assert(info.inMenu);
}

static void TestExtractLikeInfo_CompositeFlagsParsing()
{
  const auto info = BuildCrashEventInfo(0u, 0u, 0u, skydiag::helper::internal::kStateInMenu | 0x10u);
  assert(info.inMenu);
  assert(info.stateFlags == (skydiag::helper::internal::kStateInMenu | 0x10u));
}

static void TestQueueDeferredCrashViewer_EmptyQueueSetsPath()
{
  std::wstring pending;
  const bool queued = QueueDeferredCrashViewer(L"a.dmp", &pending);
  assert(queued);
  assert(pending == L"a.dmp");
}

static void TestQueueDeferredCrashViewer_IdempotentForSamePath()
{
  std::wstring pending = L"a.dmp";
  const bool queued = QueueDeferredCrashViewer(L"a.dmp", &pending);
  assert(queued);
  assert(pending == L"a.dmp");
}

static void TestQueueDeferredCrashViewer_RejectsDifferentExistingPath()
{
  std::wstring pending = L"a.dmp";
  const bool queued = QueueDeferredCrashViewer(L"b.dmp", &pending);
  assert(!queued);
  assert(pending == L"a.dmp");
}

static void TestQueueDeferredCrashViewer_NullPendingPath()
{
  const bool queued = QueueDeferredCrashViewer(L"a.dmp", nullptr);
  assert(!queued);
}

static void TestFilteredVerdictsNeverLatchCrashCapture()
{
  assert(ShouldLatchCrashCapture(FilterVerdict::kKeepDump));
  assert(!ShouldLatchCrashCapture(FilterVerdict::kDeleteBenign));
  assert(!ShouldLatchCrashCapture(FilterVerdict::kDeleteRecovered));
  assert(!ShouldLatchCrashCapture(FilterVerdict::kRetryNewerCrash));
}

static void TestCommittedCrashSequenceValidation()
{
  assert(!IsCommittedCrashSequence(0u));
  assert(!IsCommittedCrashSequence(1u));
  assert(IsCommittedCrashSequence(2u));
  assert(!IsCommittedCrashSequence(3u));
  assert(IsCommittedCrashSequence(4u));
}

// The crash event is consumed before the dump is written and the game never
// re-signals for the same fault, so a transient write failure must be retried
// in place or the incident is lost.
static void TestDumpWriteRetry_FirstAttemptAlwaysRuns()
{
  // Even if the process is already gone, the first attempt must run: the
  // snapshot may still be writable and this is the only chance to find out.
  assert(ShouldAttemptDumpWrite(0, /*processStillActive=*/false));
  assert(ShouldAttemptDumpWrite(0, /*processStillActive=*/true));
}

static void TestDumpWriteRetry_RetriesOnlyWhileProcessAlive()
{
  // MiniDumpWriteDump reads the target address space; once the process exits
  // there is nothing left to capture.
  assert(ShouldAttemptDumpWrite(1, /*processStillActive=*/true));
  assert(!ShouldAttemptDumpWrite(1, /*processStillActive=*/false));
}

static void TestDumpWriteRetry_BudgetIsBounded()
{
  assert(kDumpWriteAttempts > 1 && "A single attempt would leave no retry at all");
  assert(!ShouldAttemptDumpWrite(kDumpWriteAttempts, /*processStillActive=*/true));
  assert(!ShouldAttemptDumpWrite(kDumpWriteAttempts + 1, /*processStillActive=*/true));
}

// Quarantine marks zero-exit filters that carried real fault evidence. The
// verdict stays filtered, while dump preservation is decided separately from
// the metadata-write result.
static void TestQuarantineCleanExit_StrongCommittedZeroExit()
{
  auto info = BuildCrashEventInfo(0xC0000005u, 0x439Eu, 17120u, 0u);
  info.crashSeq = 2u;
  assert(info.isStrong);
  assert(ShouldQuarantineCleanExitEvidence(0u, info));
  // The delete decision must be unchanged by quarantine eligibility.
  assert(ClassifyExitCodeVerdict(0u, info, {}) == FilterVerdict::kDeleteBenign);
}

static void TestQuarantineCleanExit_RequiresZeroExit()
{
  auto info = BuildCrashEventInfo(0xC0000005u, 0x439Eu, 17120u, 0u);
  info.crashSeq = 2u;
  // A non-zero exit keeps the dump, so there is nothing to preserve separately.
  assert(!ShouldQuarantineCleanExitEvidence(1u, info));
}

static void TestQuarantineCleanExit_RequiresStrongException()
{
  auto info = BuildCrashEventInfo(skydiag::helper::internal::kStatusCppException, 0u, 0u, 0u);
  info.crashSeq = 2u;
  assert(!info.isStrong);
  assert(!ShouldQuarantineCleanExitEvidence(0u, info));
}

static void TestQuarantineCleanExit_RequiresCommittedSequence()
{
  auto info = BuildCrashEventInfo(0xC0000005u, 0x439Eu, 17120u, 0u);
  assert(info.isStrong);

  // Never published.
  info.crashSeq = 0u;
  assert(!ShouldQuarantineCleanExitEvidence(0u, info));

  // Publication still in flight; the record may be half-written.
  info.crashSeq = 3u;
  assert(!ShouldQuarantineCleanExitEvidence(0u, info));
}

static void TestQuarantineWriteFailurePreservesDumpAsFailSafe()
{
  assert(ShouldPreserveFilteredDump(
    /*preserveConfigured=*/false,
    /*evidenceRequired=*/true,
    /*evidenceWritten=*/false));
  assert(!ShouldPreserveFilteredDump(
    /*preserveConfigured=*/false,
    /*evidenceRequired=*/true,
    /*evidenceWritten=*/true));
  assert(!ShouldPreserveFilteredDump(
    /*preserveConfigured=*/false,
    /*evidenceRequired=*/false,
    /*evidenceWritten=*/false));
  assert(ShouldPreserveFilteredDump(
    /*preserveConfigured=*/true,
    /*evidenceRequired=*/false,
    /*evidenceWritten=*/false));
}

// Field case (v0.2.59-rc4): a plugin's error dialog ended the game with exit
// code 3, and the hint claimed "dump capture failed" although no crash was
// recorded and no dump was attempted.
static void TestWerFallbackHintTellsTheTwoCasesApart()
{
  using skydiag::helper::internal::BuildWerFallbackHintText;
  using skydiag::helper::internal::WerFallbackReason;

  const auto abnormal = BuildWerFallbackHintText(WerFallbackReason::kAbnormalExitWithoutCrash, 3u);
  assert(abnormal.find("dump capture failed") == std::string::npos);
  assert(abnormal.find("exit code 3 / 0x00000003") != std::string::npos);
  assert(abnormal.find("This is not a failed capture.") != std::string::npos);
  assert(abnormal.find("error dialog") != std::string::npos);
  assert(abnormal.find("LocalDumps\\SkyrimSE.exe") != std::string::npos);

  const auto failed = BuildWerFallbackHintText(WerFallbackReason::kDumpWriteFailed, 0u);
  assert(failed.find("could not write its dump") != std::string::npos);
  assert(failed.find("LocalDumps\\SkyrimSE.exe") != std::string::npos);
}

int main()
{
  TestWerFallbackHintTellsTheTwoCasesApart();
  TestClassifyExitCodeVerdict_DeleteBenignOnWeakZeroExit();
  TestClassifyExitCodeVerdict_DeleteBenignOnStrongZeroExit();
  TestClassifyExitCodeVerdict_KeepDumpInMenuNonZeroExit();
  TestClassifyExitCodeVerdict_KeepDumpOutsideMenuNonZeroExit();
  TestClassifyExitCodeVerdict_DeleteBenignForInvalidHandleWeak();
  TestClassifyExitCodeVerdict_DeleteBenignForCppExceptionWeak();
  TestClassifyExitCodeVerdict_DeleteBenignForClrExceptionWeak();
  TestClassifyExitCodeVerdict_DeleteBenignForBreakpointWeak();
  TestClassifyExitCodeVerdict_DeleteBenignForControlCExitWeak();
  TestClassifyExitCodeVerdict_KeepDumpForAccessViolationNonZeroExit();

  TestExtractLikeInfo_DefaultValues();
  TestExtractLikeInfo_FieldMapping();
  TestExtractLikeInfo_InMenuFlagSet();
  TestExtractLikeInfo_CompositeFlagsParsing();

  TestQueueDeferredCrashViewer_EmptyQueueSetsPath();
  TestQueueDeferredCrashViewer_IdempotentForSamePath();
  TestQueueDeferredCrashViewer_RejectsDifferentExistingPath();
  TestQueueDeferredCrashViewer_NullPendingPath();
  TestFilteredVerdictsNeverLatchCrashCapture();
  TestCommittedCrashSequenceValidation();

  TestDumpWriteRetry_FirstAttemptAlwaysRuns();
  TestDumpWriteRetry_RetriesOnlyWhileProcessAlive();
  TestDumpWriteRetry_BudgetIsBounded();

  TestQuarantineCleanExit_StrongCommittedZeroExit();
  TestQuarantineCleanExit_RequiresZeroExit();
  TestQuarantineCleanExit_RequiresStrongException();
  TestQuarantineCleanExit_RequiresCommittedSequence();
  TestQuarantineWriteFailurePreservesDumpAsFailSafe();
  return 0;
}
