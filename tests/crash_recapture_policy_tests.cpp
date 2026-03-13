#include <cassert>
#include <cstdint>

#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/CrashRecapturePolicy.h"

using skydiag::helper::DumpMode;

static void TestPolicyDisabledSkips()
{
  const auto d = skydiag::helper::DecideCrashFullRecapture(
    /*enablePolicy=*/false,
    /*autoAnalyzeDump=*/true,
    /*unknownFaultModule=*/true,
    /*unknownStreak=*/5,
    /*bucketSeenCount=*/5,
    /*threshold=*/2,
    /*candidateConflict=*/false,
    /*isolatedReferenceClue=*/false,
    /*degradedStackwalk=*/false,
    DumpMode::kDefault);
  assert(!d.shouldRecaptureFullDump);
}

static void TestUnknownStreakBelowThresholdSkips()
{
  const auto d = skydiag::helper::DecideCrashFullRecapture(
    /*enablePolicy=*/true,
    /*autoAnalyzeDump=*/true,
    /*unknownFaultModule=*/true,
    /*unknownStreak=*/1,
    /*bucketSeenCount=*/1,
    /*threshold=*/2,
    /*candidateConflict=*/false,
    /*isolatedReferenceClue=*/false,
    /*degradedStackwalk=*/false,
    DumpMode::kDefault);
  assert(!d.shouldRecaptureFullDump);
}

static void TestUnknownStreakAtThresholdTriggers()
{
  const auto d = skydiag::helper::DecideCrashFullRecapture(
    /*enablePolicy=*/true,
    /*autoAnalyzeDump=*/true,
    /*unknownFaultModule=*/true,
    /*unknownStreak=*/2,
    /*bucketSeenCount=*/2,
    /*threshold=*/2,
    /*candidateConflict=*/false,
    /*isolatedReferenceClue=*/false,
    /*degradedStackwalk=*/false,
    DumpMode::kDefault);
  assert(d.shouldRecaptureFullDump);
  assert(d.triggeredByUnknownFaultModule);
}

static void TestKnownFaultModuleSkips()
{
  const auto d = skydiag::helper::DecideCrashFullRecapture(
    /*enablePolicy=*/true,
    /*autoAnalyzeDump=*/true,
    /*unknownFaultModule=*/false,
    /*unknownStreak=*/4,
    /*bucketSeenCount=*/4,
    /*threshold=*/2,
    /*candidateConflict=*/false,
    /*isolatedReferenceClue=*/false,
    /*degradedStackwalk=*/false,
    DumpMode::kDefault);
  assert(!d.shouldRecaptureFullDump);
}

static void TestFullDumpModeSkips()
{
  const auto d = skydiag::helper::DecideCrashFullRecapture(
    /*enablePolicy=*/true,
    /*autoAnalyzeDump=*/true,
    /*unknownFaultModule=*/true,
    /*unknownStreak=*/3,
    /*bucketSeenCount=*/3,
    /*threshold=*/2,
    /*candidateConflict=*/false,
    /*isolatedReferenceClue=*/false,
    /*degradedStackwalk=*/false,
    DumpMode::kFull);
  assert(!d.shouldRecaptureFullDump);
}

static void TestCandidateConflictAtThresholdTriggers()
{
  const auto d = skydiag::helper::DecideCrashFullRecapture(
    /*enablePolicy=*/true,
    /*autoAnalyzeDump=*/true,
    /*unknownFaultModule=*/false,
    /*unknownStreak=*/0,
    /*bucketSeenCount=*/2,
    /*threshold=*/2,
    /*candidateConflict=*/true,
    /*isolatedReferenceClue=*/false,
    /*degradedStackwalk=*/false,
    DumpMode::kDefault);
  assert(d.shouldRecaptureFullDump);
  assert(d.triggeredByCandidateConflict);
}

static void TestIsolatedReferenceClueAtThresholdTriggers()
{
  const auto d = skydiag::helper::DecideCrashFullRecapture(
    /*enablePolicy=*/true,
    /*autoAnalyzeDump=*/true,
    /*unknownFaultModule=*/false,
    /*unknownStreak=*/0,
    /*bucketSeenCount=*/2,
    /*threshold=*/2,
    /*candidateConflict=*/false,
    /*isolatedReferenceClue=*/true,
    /*degradedStackwalk=*/false,
    DumpMode::kDefault);
  assert(d.shouldRecaptureFullDump);
  assert(d.triggeredByReferenceClueOnly);
}

static void TestDegradedStackwalkAtThresholdTriggers()
{
  const auto d = skydiag::helper::DecideCrashFullRecapture(
    /*enablePolicy=*/true,
    /*autoAnalyzeDump=*/true,
    /*unknownFaultModule=*/false,
    /*unknownStreak=*/0,
    /*bucketSeenCount=*/2,
    /*threshold=*/2,
    /*candidateConflict=*/false,
    /*isolatedReferenceClue=*/false,
    /*degradedStackwalk=*/true,
    DumpMode::kDefault);
  assert(d.shouldRecaptureFullDump);
  assert(d.triggeredByStackwalkDegraded);
}

int main()
{
  TestPolicyDisabledSkips();
  TestUnknownStreakBelowThresholdSkips();
  TestUnknownStreakAtThresholdTriggers();
  TestKnownFaultModuleSkips();
  TestFullDumpModeSkips();
  TestCandidateConflictAtThresholdTriggers();
  TestIsolatedReferenceClueAtThresholdTriggers();
  TestDegradedStackwalkAtThresholdTriggers();
  return 0;
}
