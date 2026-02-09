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
    /*threshold=*/2,
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
    /*threshold=*/2,
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
    /*threshold=*/2,
    DumpMode::kDefault);
  assert(d.shouldRecaptureFullDump);
}

static void TestKnownFaultModuleSkips()
{
  const auto d = skydiag::helper::DecideCrashFullRecapture(
    /*enablePolicy=*/true,
    /*autoAnalyzeDump=*/true,
    /*unknownFaultModule=*/false,
    /*unknownStreak=*/4,
    /*threshold=*/2,
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
    /*threshold=*/2,
    DumpMode::kFull);
  assert(!d.shouldRecaptureFullDump);
}

int main()
{
  TestPolicyDisabledSkips();
  TestUnknownStreakBelowThresholdSkips();
  TestUnknownStreakAtThresholdTriggers();
  TestKnownFaultModuleSkips();
  TestFullDumpModeSkips();
  return 0;
}

