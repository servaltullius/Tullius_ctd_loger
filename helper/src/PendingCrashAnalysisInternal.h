#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include "HelperCommon.h"
#include "PendingCrashAnalysis.h"
#include "SkyrimDiagHelper/CrashRecapturePolicy.h"

namespace skydiag::helper {
struct AttachedProcess;
struct HelperConfig;
}

namespace skydiag::helper::internal {

struct PendingCrashRecaptureContext
{
  CrashSummaryInfo summaryInfo{};
  std::uint32_t unknownStreak = 0;
  std::uint32_t bucketSeenCount = 0;
  RecaptureDecision recaptureDecision{};
  std::filesystem::path manifestPath;
};

DWORD CrashAnalysisTimeoutMs(const skydiag::helper::HelperConfig& cfg);

bool TryEvaluateCrashRecapture(
  const skydiag::helper::HelperConfig& cfg,
  const PendingCrashAnalysis& task,
  const std::filesystem::path& outBase,
  PendingCrashRecaptureContext* out,
  std::wstring* err);

void ApplyCrashRecaptureDecision(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  const PendingCrashAnalysis& task,
  const PendingCrashRecaptureContext& context);

}  // namespace skydiag::helper::internal
