#pragma once

#include <cstddef>
#include <cstdint>

#include "I18nCore.h"

namespace skydiag::dump_tool::internal::policy {

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
