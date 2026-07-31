#include <cassert>
#include <string>

#include "CrashCapture.h"

using skydiag::helper::internal::BuildCrashEventInfo;
using skydiag::helper::internal::ClassifyExitCodeVerdict;
using skydiag::helper::internal::CrashEventInfo;
using skydiag::helper::internal::FilterVerdict;
using skydiag::helper::internal::IsCommittedCrashSequence;
using skydiag::helper::internal::QueueDeferredCrashViewer;
using skydiag::helper::internal::ShouldLatchCrashCapture;
using skydiag::helper::internal::ShouldAttemptDumpWrite;
using skydiag::helper::internal::ShouldPreserveFilteredDump;
using skydiag::helper::internal::ShouldQuarantineCleanExitEvidence;
using skydiag::helper::internal::kDumpWriteAttempts;

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

int main()
{
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
