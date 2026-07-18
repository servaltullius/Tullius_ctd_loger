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
  return 0;
}
