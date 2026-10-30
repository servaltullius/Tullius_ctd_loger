#pragma once

#include <cstddef>
#include <cstdint>

#include "I18nCore.h"

namespace skydiag::dump_tool::internal::policy {

// Callstack frame weights: frames closer to the crash point are weighted higher
// to reflect their higher likelihood of being the actual crash cause.
inline constexpr std::uint32_t kWeightDepth0      = 16;  // crash frame itself
inline constexpr std::uint32_t kWeightDepth1      = 12;  // immediate caller
inline constexpr std::uint32_t kWeightDepth2      = 8;   // 2nd caller
inline constexpr std::uint32_t kWeightDepth3to5   = 4;   // near callers
inline constexpr std::uint32_t kWeightDepth6to10  = 2;   // mid-range callers
inline constexpr std::uint32_t kWeightDeepFrame   = 1;   // deep callers (depth > 10)

// Confidence thresholds: empirically tuned against real Skyrim crash dumps.
// "High" requires a near-top suspect with a clear lead over the runner-up.
inline constexpr std::size_t   kHighConfMaxDepth     = 2;
inline constexpr std::uint32_t kHighConfMinScore     = 24;
inline constexpr std::uint32_t kHighConfMinMargin    = 12;
inline constexpr std::size_t   kMedConfMaxDepth      = 6;
inline constexpr std::uint32_t kMedConfMinScore      = 12;
inline constexpr std::uint32_t kMedConfMinMargin     = 6;

constexpr std::uint32_t CallstackFrameWeight(std::size_t depth) noexcept
{
  if (depth == 0) return kWeightDepth0;
  if (depth == 1) return kWeightDepth1;
  if (depth == 2) return kWeightDepth2;
  if (depth <= 5) return kWeightDepth3to5;
  if (depth <= 10) return kWeightDepth6to10;
  return kWeightDeepFrame;
}

// The top callstack suspect's confidence. A strong score alone is High only
// when it also leads the runner-up: two modules tied on the stack are not
// told apart with High (Medium at most).
constexpr i18n::ConfidenceLevel TopSuspectCallstackConfidence(
  std::uint32_t topScore,
  std::uint32_t secondScore,
  std::size_t firstDepth) noexcept
{
  const std::uint64_t top = topScore;
  const std::uint64_t second = secondScore;
  if (firstDepth <= kHighConfMaxDepth &&
      ((top >= kHighConfMinScore && top > second) || top >= second + kHighConfMinMargin)) {
    return i18n::ConfidenceLevel::kHigh;
  }
  if (firstDepth <= kMedConfMaxDepth && (top >= kMedConfMinScore || top >= second + kMedConfMinMargin)) {
    return i18n::ConfidenceLevel::kMedium;
  }
  return i18n::ConfidenceLevel::kLow;
}

// The capture's authoritative thread (the exception thread for CTDs or the
// inferred game main thread for hangs) is the primary stack whenever it
// produced a usable candidate. Numeric TID ordering and a higher score on an
// auxiliary thread must not replace it.
constexpr bool ShouldSelectStackwalkCandidate(
  bool currentHasCandidate,
  std::uint32_t currentTid,
  std::uint32_t currentScore,
  bool candidateHasCandidate,
  std::uint32_t candidateTid,
  std::uint32_t candidateScore,
  std::uint32_t preferredTid) noexcept
{
  if (!candidateHasCandidate) {
    return false;
  }
  if (!currentHasCandidate) {
    return true;
  }

  if (preferredTid != 0u) {
    const bool currentIsPreferred = currentTid == preferredTid;
    const bool candidateIsPreferred = candidateTid == preferredTid;
    if (currentIsPreferred != candidateIsPreferred) {
      return candidateIsPreferred;
    }
  }

  return candidateScore > currentScore;
}

// A hook-framework frame owner is demoted only when the alternative has its
// own meaningful evidence and is close enough to the original score.
constexpr bool ShouldPromoteHookFallback(
  std::uint32_t topScore,
  std::uint32_t fallbackScore,
  std::uint32_t minimumFallbackScore,
  std::uint32_t nearTieThreshold) noexcept
{
  return fallbackScore >= minimumFallbackScore &&
         static_cast<std::uint64_t>(fallbackScore) + nearTieThreshold >= topScore;
}

// Crash Logger can corroborate the selected stackwalk module, but it is not an
// independent reason to turn a raw pointer-density scan into a stronger claim.
// Preserve confidence already earned by a real stackwalk and otherwise allow
// corroboration to provide only a Medium floor.
constexpr i18n::ConfidenceLevel ConfidenceAfterCrashLoggerCorroboration(
  i18n::ConfidenceLevel currentConfidence,
  bool hasCrashLoggerSupport,
  bool suspectsFromStackwalk) noexcept
{
  if (!suspectsFromStackwalk) {
    return i18n::ConfidenceLevel::kLow;
  }
  if (currentConfidence == i18n::ConfidenceLevel::kHigh) {
    return i18n::ConfidenceLevel::kHigh;
  }
  if (hasCrashLoggerSupport &&
      (currentConfidence == i18n::ConfidenceLevel::kLow ||
       currentConfidence == i18n::ConfidenceLevel::kUnknown)) {
    return i18n::ConfidenceLevel::kMedium;
  }
  return currentConfidence;
}

constexpr bool SecondaryCallstackCanBeMedium(
  std::uint32_t score,
  std::size_t firstDepth,
  std::uint32_t minimumScore,
  std::size_t maximumDepth) noexcept
{
  return score >= minimumScore && firstDepth <= maximumDepth;
}

// Candidate-consensus weights must not silently upgrade a Low/Unknown
// stackwalk suspect into the standalone Medium threshold (weight >= 5).
constexpr std::uint32_t ActionableStackSignalWeight(
  bool fromStackwalk,
  bool symbolRuntimeDegraded,
  bool suspectAtLeastMedium,
  bool firstVisibleSuspect) noexcept
{
  if (!fromStackwalk) {
    return 2u;
  }
  if (symbolRuntimeDegraded || !suspectAtLeastMedium) {
    return firstVisibleSuspect ? 3u : 2u;
  }
  return firstVisibleSuspect ? 5u : 4u;
}

}  // namespace skydiag::dump_tool::internal::policy
