#include <cassert>
#include <string>

#include "CrashCapture.h"

using skydiag::helper::internal::BuildCrashEventInfo;
using skydiag::helper::internal::ClassifyExitCodeVerdict;
using skydiag::helper::internal::CrashEventInfo;
using skydiag::helper::internal::FilterVerdict;
using skydiag::helper::internal::QueueDeferredCrashViewer;

static void TestClassifyExitCodeVerdict_DeleteBenignOnWeakZeroExit()
{
  const auto info = BuildCrashEventInfo(0u, 0u, 0u, 0u);
  assert(ClassifyExitCodeVerdict(0u, info, {}) == FilterVerdict::kDeleteBenign);
}

static void TestClassifyExitCodeVerdict_KeepDumpOnStrongZeroExit()
{
  const auto info = BuildCrashEventInfo(0xC0000005u, 0u, 0u, 0u);
  assert(ClassifyExitCodeVerdict(0u, info, {}) == FilterVerdict::kKeepDump);
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

static void TestClassifyExitCodeVerdict_KeepDumpForAccessViolationStrong()
{
  const auto info = BuildCrashEventInfo(0xC0000005u, 0u, 0u, 0u);
  assert(ClassifyExitCodeVerdict(0u, info, {}) == FilterVerdict::kKeepDump);
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

int main()
{
  TestClassifyExitCodeVerdict_DeleteBenignOnWeakZeroExit();
  TestClassifyExitCodeVerdict_KeepDumpOnStrongZeroExit();
  TestClassifyExitCodeVerdict_KeepDumpInMenuNonZeroExit();
  TestClassifyExitCodeVerdict_KeepDumpOutsideMenuNonZeroExit();
  TestClassifyExitCodeVerdict_DeleteBenignForInvalidHandleWeak();
  TestClassifyExitCodeVerdict_KeepDumpForAccessViolationStrong();

  TestExtractLikeInfo_DefaultValues();
  TestExtractLikeInfo_FieldMapping();
  TestExtractLikeInfo_InMenuFlagSet();
  TestExtractLikeInfo_CompositeFlagsParsing();

  TestQueueDeferredCrashViewer_EmptyQueueSetsPath();
  TestQueueDeferredCrashViewer_IdempotentForSamePath();
  TestQueueDeferredCrashViewer_RejectsDifferentExistingPath();
  TestQueueDeferredCrashViewer_NullPendingPath();
  return 0;
}
