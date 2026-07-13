#pragma once

#include <cstddef>
#include <cstdint>

namespace skydiag::dump_tool::internal::policy {

// For CTD analysis the exception thread is the authoritative primary stack
// whenever it produced a usable candidate. Numeric TID ordering and a higher
// score on an auxiliary/main thread must not replace it.
constexpr bool ShouldSelectStackwalkCandidate(
  bool currentHasCandidate,
  std::uint32_t currentTid,
  std::uint32_t currentScore,
  bool candidateHasCandidate,
  std::uint32_t candidateTid,
  std::uint32_t candidateScore,
  std::uint32_t exceptionTid) noexcept
{
  if (!candidateHasCandidate) {
    return false;
  }
  if (!currentHasCandidate) {
    return true;
  }

  if (exceptionTid != 0u) {
    const bool currentIsException = currentTid == exceptionTid;
    const bool candidateIsException = candidateTid == exceptionTid;
    if (currentIsException != candidateIsException) {
      return candidateIsException;
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

constexpr bool SecondaryCallstackCanBeMedium(
  std::uint32_t score,
  std::size_t firstDepth,
  std::uint32_t minimumScore,
  std::size_t maximumDepth) noexcept
{
  return score >= minimumScore && firstDepth <= maximumDepth;
}

constexpr bool SecondaryStackScanCanBeMedium(
  std::uint32_t score,
  std::uint32_t minimumScore) noexcept
{
  return score >= minimumScore;
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
