#include <cassert>
#include <cstdint>

#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/CrashRecapturePolicy.h"

using skydiag::helper::DumpMode;

namespace {

bool HasReason(
  const skydiag::helper::RecaptureDecision& decision,
  skydiag::helper::RecaptureReason reason)
{
  for (const auto& entry : decision.reasons) {
    if (entry == reason) {
      return true;
    }
  }
  return false;
}

void TestCrashPolicyDisabledSkips()
{
  const auto d = skydiag::helper::DecideCrashRecapture(
    /*enablePolicy=*/false,
    /*autoAnalyzeDump=*/true,
    /*unknownFaultModule=*/true,
    /*unknownStreak=*/5,
    /*bucketSeenCount=*/5,
    /*threshold=*/2,
    /*candidateConflict=*/true,
    /*isolatedReferenceClue=*/false,
    /*degradedStackwalk=*/true,
    /*symbolRuntimeDegraded=*/true,
    /*firstChanceCandidateWeak=*/true,
    DumpMode::kMini);
  assert(!d.shouldRecapture);
  assert(d.targetProfile == skydiag::helper::RecaptureTargetProfile::kNone);
}

void TestCrashRicherEscalationIncludesNewWeakSignals()
{
  const auto d = skydiag::helper::DecideCrashRecapture(
    /*enablePolicy=*/true,
    /*autoAnalyzeDump=*/true,
    /*unknownFaultModule=*/false,
    /*unknownStreak=*/0,
    /*bucketSeenCount=*/2,
    /*threshold=*/2,
    /*candidateConflict=*/false,
    /*isolatedReferenceClue=*/false,
    /*degradedStackwalk=*/true,
    /*symbolRuntimeDegraded=*/true,
    /*firstChanceCandidateWeak=*/true,
    DumpMode::kMini);
  assert(d.shouldRecapture);
  assert(d.kind == skydiag::helper::RecaptureKind::kCrash);
  assert(d.targetProfile == skydiag::helper::RecaptureTargetProfile::kCrashRicher);
  assert(d.escalationLevel == 1u);
  assert(HasReason(d, skydiag::helper::RecaptureReason::kStackwalkDegraded));
  assert(HasReason(d, skydiag::helper::RecaptureReason::kSymbolRuntimeDegraded));
  assert(HasReason(d, skydiag::helper::RecaptureReason::kFirstChanceCandidateWeak));
}

void TestCrashRicherEscalationForRepeatedUnknownModule()
{
  const auto d = skydiag::helper::DecideCrashRecapture(
    /*enablePolicy=*/true,
    /*autoAnalyzeDump=*/true,
    /*unknownFaultModule=*/true,
    /*unknownStreak=*/3,
    /*bucketSeenCount=*/3,
    /*threshold=*/2,
    /*candidateConflict=*/false,
    /*isolatedReferenceClue=*/false,
    /*degradedStackwalk=*/false,
    /*symbolRuntimeDegraded=*/false,
    /*firstChanceCandidateWeak=*/false,
    DumpMode::kDefault);
  assert(d.shouldRecapture);
  assert(d.targetProfile == skydiag::helper::RecaptureTargetProfile::kCrashRicher);
  assert(d.escalationLevel == 1u);
  assert(HasReason(d, skydiag::helper::RecaptureReason::kUnknownFaultModule));
}

void TestCrashFullEscalationNeedsAdditionalWeakSignals()
{
  const auto d = skydiag::helper::DecideCrashRecapture(
    /*enablePolicy=*/true,
    /*autoAnalyzeDump=*/true,
    /*unknownFaultModule=*/true,
    /*unknownStreak=*/3,
    /*bucketSeenCount=*/3,
    /*threshold=*/2,
    /*candidateConflict=*/true,
    /*isolatedReferenceClue=*/false,
    /*degradedStackwalk=*/false,
    /*symbolRuntimeDegraded=*/true,
    /*firstChanceCandidateWeak=*/false,
    DumpMode::kDefault);
  assert(d.shouldRecapture);
  assert(d.targetProfile == skydiag::helper::RecaptureTargetProfile::kCrashFull);
  assert(d.escalationLevel == 2u);
  assert(HasReason(d, skydiag::helper::RecaptureReason::kUnknownFaultModule));
  assert(HasReason(d, skydiag::helper::RecaptureReason::kCandidateConflict));
  assert(HasReason(d, skydiag::helper::RecaptureReason::kSymbolRuntimeDegraded));
}

void TestFreezeAmbiguousSelectsSnapshotProfile()
{
  const auto d = skydiag::helper::DecideFreezeRecapture(
    /*enablePolicy=*/true,
    /*autoAnalyzeDump=*/true,
    /*freezeAmbiguous=*/true,
    /*freezeSnapshotFallback=*/false,
    /*freezeCandidateWeak=*/false,
    /*strongDeadlock=*/false,
    /*snapshotBackedLoaderStall=*/false,
    /*bucketSeenCount=*/2,
    /*threshold=*/2);
  assert(d.shouldRecapture);
  assert(d.kind == skydiag::helper::RecaptureKind::kFreeze);
  assert(d.targetProfile == skydiag::helper::RecaptureTargetProfile::kFreezeSnapshotRicher);
  assert(HasReason(d, skydiag::helper::RecaptureReason::kFreezeAmbiguous));
}

void TestFreezeSnapshotFallbackSelectsSnapshotProfile()
{
  const auto d = skydiag::helper::DecideFreezeRecapture(
    /*enablePolicy=*/true,
    /*autoAnalyzeDump=*/true,
    /*freezeAmbiguous=*/false,
    /*freezeSnapshotFallback=*/true,
    /*freezeCandidateWeak=*/true,
    /*strongDeadlock=*/false,
    /*snapshotBackedLoaderStall=*/false,
    /*bucketSeenCount=*/2,
    /*threshold=*/2);
  assert(d.shouldRecapture);
  assert(d.targetProfile == skydiag::helper::RecaptureTargetProfile::kFreezeSnapshotRicher);
  assert(HasReason(d, skydiag::helper::RecaptureReason::kFreezeSnapshotFallback));
  assert(HasReason(d, skydiag::helper::RecaptureReason::kFreezeCandidateWeak));
}

void TestStrongFreezeStateSkipsRecapture()
{
  const auto d = skydiag::helper::DecideFreezeRecapture(
    /*enablePolicy=*/true,
    /*autoAnalyzeDump=*/true,
    /*freezeAmbiguous=*/true,
    /*freezeSnapshotFallback=*/true,
    /*freezeCandidateWeak=*/true,
    /*strongDeadlock=*/true,
    /*snapshotBackedLoaderStall=*/false,
    /*bucketSeenCount=*/4,
    /*threshold=*/2);
  assert(!d.shouldRecapture);
  assert(d.targetProfile == skydiag::helper::RecaptureTargetProfile::kNone);
}

}  // namespace

int main()
{
  TestCrashPolicyDisabledSkips();
  TestCrashRicherEscalationIncludesNewWeakSignals();
  TestCrashRicherEscalationForRepeatedUnknownModule();
  TestCrashFullEscalationNeedsAdditionalWeakSignals();
  TestFreezeAmbiguousSelectsSnapshotProfile();
  TestFreezeSnapshotFallbackSelectsSnapshotProfile();
  TestStrongFreezeStateSkipsRecapture();
  return 0;
}
