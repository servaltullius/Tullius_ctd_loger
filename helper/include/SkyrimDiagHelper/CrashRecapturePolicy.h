#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "SkyrimDiagHelper/Config.h"

namespace skydiag::helper {

enum class RecaptureKind {
  kCrash,
  kFreeze,
};

enum class RecaptureReason {
  kUnknownFaultModule,
  kCandidateConflict,
  kReferenceClueOnly,
  kStackwalkDegraded,
  kSymbolRuntimeDegraded,
  kFirstChanceCandidateWeak,
  kFreezeAmbiguous,
  kFreezeSnapshotFallback,
  kFreezeCandidateWeak,
};

enum class RecaptureTargetProfile {
  kNone,
  kCrashRicher,
  kCrashFull,
  kFreezeSnapshotRicher,
};

inline const char* RecaptureKindToString(RecaptureKind kind)
{
  switch (kind) {
    case RecaptureKind::kCrash:
      return "crash";
    case RecaptureKind::kFreeze:
      return "freeze";
  }
  return "crash";
}

inline const char* RecaptureReasonToString(RecaptureReason reason)
{
  switch (reason) {
    case RecaptureReason::kUnknownFaultModule:
      return "unknown_fault_module";
    case RecaptureReason::kCandidateConflict:
      return "candidate_conflict";
    case RecaptureReason::kReferenceClueOnly:
      return "reference_clue_only";
    case RecaptureReason::kStackwalkDegraded:
      return "stackwalk_degraded";
    case RecaptureReason::kSymbolRuntimeDegraded:
      return "symbol_runtime_degraded";
    case RecaptureReason::kFirstChanceCandidateWeak:
      return "first_chance_candidate_weak";
    case RecaptureReason::kFreezeAmbiguous:
      return "freeze_ambiguous";
    case RecaptureReason::kFreezeSnapshotFallback:
      return "freeze_snapshot_fallback";
    case RecaptureReason::kFreezeCandidateWeak:
      return "freeze_candidate_weak";
  }
  return "unknown_fault_module";
}

inline const char* RecaptureTargetProfileToString(RecaptureTargetProfile profile)
{
  switch (profile) {
    case RecaptureTargetProfile::kNone:
      return "none";
    case RecaptureTargetProfile::kCrashRicher:
      return "crash_richer";
    case RecaptureTargetProfile::kCrashFull:
      return "crash_full";
    case RecaptureTargetProfile::kFreezeSnapshotRicher:
      return "freeze_snapshot_richer";
  }
  return "none";
}

struct RecaptureDecision {
  RecaptureKind kind = RecaptureKind::kCrash;
  bool shouldRecapture = false;
  bool shouldRecaptureFullDump = false;
  std::uint32_t normalizedUnknownThreshold = 1;
  std::uint32_t escalationLevel = 0;
  RecaptureTargetProfile targetProfile = RecaptureTargetProfile::kNone;
  std::vector<RecaptureReason> reasons;

  bool triggeredByUnknownFaultModule = false;
  bool triggeredByCandidateConflict = false;
  bool triggeredByReferenceClueOnly = false;
  bool triggeredByStackwalkDegraded = false;
  bool triggeredBySymbolRuntimeDegraded = false;
  bool triggeredByFirstChanceCandidateWeak = false;
  bool triggeredByFreezeAmbiguous = false;
  bool triggeredByFreezeSnapshotFallback = false;
  bool triggeredByFreezeCandidateWeak = false;
};

inline void AppendRecaptureReason(RecaptureDecision& out, RecaptureReason reason)
{
  for (const auto existing : out.reasons) {
    if (existing == reason) {
      return;
    }
  }
  out.reasons.push_back(reason);
}

inline RecaptureDecision DecideCrashRecapture(
  bool enablePolicy,
  bool autoAnalyzeDump,
  bool unknownFaultModule,
  std::uint32_t unknownStreak,
  std::uint32_t bucketSeenCount,
  std::uint32_t unknownThreshold,
  bool candidateConflict,
  bool isolatedReferenceClue,
  bool degradedStackwalk,
  bool symbolRuntimeDegraded,
  bool firstChanceCandidateWeak,
  DumpMode dumpMode)
{
  RecaptureDecision out{};
  out.kind = RecaptureKind::kCrash;
  out.normalizedUnknownThreshold = std::max<std::uint32_t>(1u, unknownThreshold);

  out.triggeredByUnknownFaultModule = unknownFaultModule;
  out.triggeredByCandidateConflict = candidateConflict;
  out.triggeredByReferenceClueOnly = isolatedReferenceClue;
  out.triggeredByStackwalkDegraded = degradedStackwalk;
  out.triggeredBySymbolRuntimeDegraded = symbolRuntimeDegraded;
  out.triggeredByFirstChanceCandidateWeak = firstChanceCandidateWeak;

  if (out.triggeredByUnknownFaultModule) {
    AppendRecaptureReason(out, RecaptureReason::kUnknownFaultModule);
  }
  if (out.triggeredByCandidateConflict) {
    AppendRecaptureReason(out, RecaptureReason::kCandidateConflict);
  }
  if (out.triggeredByReferenceClueOnly) {
    AppendRecaptureReason(out, RecaptureReason::kReferenceClueOnly);
  }
  if (out.triggeredByStackwalkDegraded) {
    AppendRecaptureReason(out, RecaptureReason::kStackwalkDegraded);
  }
  if (out.triggeredBySymbolRuntimeDegraded) {
    AppendRecaptureReason(out, RecaptureReason::kSymbolRuntimeDegraded);
  }
  if (out.triggeredByFirstChanceCandidateWeak) {
    AppendRecaptureReason(out, RecaptureReason::kFirstChanceCandidateWeak);
  }

  const bool repeatedBucket = bucketSeenCount >= out.normalizedUnknownThreshold;
  const bool repeatedUnknown = unknownFaultModule && unknownStreak >= out.normalizedUnknownThreshold;
  const bool repeatedWeakSignal =
    (candidateConflict && repeatedBucket) ||
    (isolatedReferenceClue && repeatedBucket) ||
    (degradedStackwalk && repeatedBucket) ||
    (symbolRuntimeDegraded && repeatedBucket) ||
    (firstChanceCandidateWeak && repeatedBucket);

  out.shouldRecapture =
    enablePolicy &&
    autoAnalyzeDump &&
    dumpMode != DumpMode::kFull &&
    (repeatedUnknown || repeatedWeakSignal);

  if (!out.shouldRecapture) {
    return out;
  }

  out.targetProfile = RecaptureTargetProfile::kCrashRicher;
  out.escalationLevel = 1;

  const bool strongerEscalationNeeded =
    dumpMode != DumpMode::kMini &&
    ((repeatedUnknown && (candidateConflict || degradedStackwalk || symbolRuntimeDegraded || firstChanceCandidateWeak)) ||
     (repeatedBucket && bucketSeenCount > out.normalizedUnknownThreshold &&
      (candidateConflict || isolatedReferenceClue || degradedStackwalk || symbolRuntimeDegraded ||
       firstChanceCandidateWeak)));
  if (strongerEscalationNeeded) {
    out.targetProfile = RecaptureTargetProfile::kCrashFull;
    out.escalationLevel = 2;
    out.shouldRecaptureFullDump = true;
  }

  return out;
}

inline RecaptureDecision DecideFreezeRecapture(
  bool enablePolicy,
  bool autoAnalyzeDump,
  bool freezeAmbiguous,
  bool freezeSnapshotFallback,
  bool freezeCandidateWeak,
  bool strongDeadlock,
  bool snapshotBackedLoaderStall,
  std::uint32_t bucketSeenCount,
  std::uint32_t unknownThreshold)
{
  RecaptureDecision out{};
  out.kind = RecaptureKind::kFreeze;
  out.normalizedUnknownThreshold = std::max<std::uint32_t>(1u, unknownThreshold);

  out.triggeredByFreezeAmbiguous = freezeAmbiguous;
  out.triggeredByFreezeSnapshotFallback = freezeSnapshotFallback;
  out.triggeredByFreezeCandidateWeak = freezeCandidateWeak;

  if (out.triggeredByFreezeAmbiguous) {
    AppendRecaptureReason(out, RecaptureReason::kFreezeAmbiguous);
  }
  if (out.triggeredByFreezeSnapshotFallback) {
    AppendRecaptureReason(out, RecaptureReason::kFreezeSnapshotFallback);
  }
  if (out.triggeredByFreezeCandidateWeak) {
    AppendRecaptureReason(out, RecaptureReason::kFreezeCandidateWeak);
  }

  const bool repeatedBucket = bucketSeenCount >= out.normalizedUnknownThreshold;
  const bool excluded = strongDeadlock || snapshotBackedLoaderStall;
  out.shouldRecapture =
    enablePolicy &&
    autoAnalyzeDump &&
    !excluded &&
    repeatedBucket &&
    (freezeAmbiguous || freezeSnapshotFallback || freezeCandidateWeak);

  if (out.shouldRecapture) {
    out.targetProfile = RecaptureTargetProfile::kFreezeSnapshotRicher;
    out.escalationLevel = 1;
  }

  return out;
}

}  // namespace skydiag::helper
