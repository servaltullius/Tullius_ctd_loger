#pragma once

#include <algorithm>
#include <cstdint>

#include "SkyrimDiagHelper/Config.h"

namespace skydiag::helper {

struct CrashRecaptureDecision {
  bool shouldRecaptureFullDump = false;
  std::uint32_t normalizedUnknownThreshold = 1;
};

inline CrashRecaptureDecision DecideCrashFullRecapture(
  bool enablePolicy,
  bool autoAnalyzeDump,
  bool unknownFaultModule,
  std::uint32_t unknownStreak,
  std::uint32_t unknownThreshold,
  DumpMode dumpMode)
{
  CrashRecaptureDecision out{};
  out.normalizedUnknownThreshold = std::max<std::uint32_t>(1u, unknownThreshold);
  out.shouldRecaptureFullDump = enablePolicy &&
    autoAnalyzeDump &&
    unknownFaultModule &&
    unknownStreak >= out.normalizedUnknownThreshold &&
    dumpMode != DumpMode::kFull;
  return out;
}

}  // namespace skydiag::helper

