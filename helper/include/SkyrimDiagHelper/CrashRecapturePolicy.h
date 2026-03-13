#pragma once

#include <algorithm>
#include <cstdint>

#include "SkyrimDiagHelper/Config.h"

namespace skydiag::helper {

struct CrashRecaptureDecision {
  bool shouldRecaptureFullDump = false;
  std::uint32_t normalizedUnknownThreshold = 1;
  bool triggeredByUnknownFaultModule = false;
  bool triggeredByCandidateConflict = false;
  bool triggeredByReferenceClueOnly = false;
  bool triggeredByStackwalkDegraded = false;
};

inline CrashRecaptureDecision DecideCrashFullRecapture(
  bool enablePolicy,
  bool autoAnalyzeDump,
  bool unknownFaultModule,
  std::uint32_t unknownStreak,
  std::uint32_t bucketSeenCount,
  std::uint32_t unknownThreshold,
  bool candidateConflict,
  bool isolatedReferenceClue,
  bool degradedStackwalk,
  DumpMode dumpMode)
{
  CrashRecaptureDecision out{};
  out.normalizedUnknownThreshold = std::max<std::uint32_t>(1u, unknownThreshold);
  const bool repeatedBucket = bucketSeenCount >= out.normalizedUnknownThreshold;
  out.triggeredByUnknownFaultModule =
    unknownFaultModule && unknownStreak >= out.normalizedUnknownThreshold;
  out.triggeredByCandidateConflict =
    candidateConflict && repeatedBucket;
  out.triggeredByReferenceClueOnly =
    isolatedReferenceClue && repeatedBucket;
  out.triggeredByStackwalkDegraded =
    degradedStackwalk && repeatedBucket;
  out.shouldRecaptureFullDump =
    enablePolicy &&
    autoAnalyzeDump &&
    dumpMode != DumpMode::kFull &&
    (out.triggeredByUnknownFaultModule ||
     out.triggeredByCandidateConflict ||
     out.triggeredByReferenceClueOnly ||
     out.triggeredByStackwalkDegraded);
  return out;
}

}  // namespace skydiag::helper
