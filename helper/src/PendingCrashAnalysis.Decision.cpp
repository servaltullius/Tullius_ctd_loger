#include "PendingCrashAnalysisInternal.h"

#include <filesystem>
#include <string>

#include "SkyrimDiagHelper/Config.h"

namespace skydiag::helper::internal {
namespace {

std::filesystem::path CrashManifestPathForDump(
  const std::wstring& dumpPath,
  const std::filesystem::path& outBase)
{
  const auto dumpFs = std::filesystem::path(dumpPath);
  const auto stem = dumpFs.stem().wstring();
  constexpr wchar_t kPrefix[] = L"SkyrimDiag_Crash_";
  if (stem.rfind(kPrefix, 0) != 0) {
    return {};
  }
  const std::wstring suffix = stem.substr((sizeof(kPrefix) / sizeof(kPrefix[0])) - 1u);
  return outBase / (L"SkyrimDiag_Incident_Crash_" + suffix + L".json");
}

}  // namespace

DWORD CrashAnalysisTimeoutMs(const skydiag::helper::HelperConfig& cfg)
{
  std::uint32_t sec = cfg.autoRecaptureAnalysisTimeoutSec;
  if (sec < 5u) {
    sec = 5u;
  }
  if (sec > 180u) {
    sec = 180u;
  }
  return static_cast<DWORD>(sec * 1000u);
}

bool TryEvaluateCrashRecapture(
  const skydiag::helper::HelperConfig& cfg,
  const PendingCrashAnalysis& task,
  const std::filesystem::path& outBase,
  PendingCrashRecaptureContext* out,
  std::wstring* err)
{
  if (out) {
    *out = PendingCrashRecaptureContext{};
  }
  if (!out) {
    if (err) {
      *err = L"missing recapture context output";
    }
    return false;
  }

  const auto summaryPath = SummaryPathForDump(task.dumpPath, outBase);
  if (!TryLoadCrashSummaryInfo(summaryPath, &out->summaryInfo, err)) {
    return false;
  }
  if (out->summaryInfo.bucketKey.empty()) {
    if (err) {
      *err = L"Crash summary has empty crash_bucket_key; recapture policy skipped.";
    }
    return false;
  }

  if (!UpdateCrashBucketStats(
        outBase,
        out->summaryInfo,
        &out->unknownStreak,
        &out->bucketSeenCount,
        err)) {
    return false;
  }

  out->recaptureDecision = skydiag::helper::DecideCrashRecapture(
    cfg.enableAutoRecaptureOnUnknownCrash,
    cfg.autoAnalyzeDump,
    out->summaryInfo.unknownFaultModule,
    out->unknownStreak,
    out->bucketSeenCount,
    cfg.autoRecaptureUnknownBucketThreshold,
    out->summaryInfo.candidateConflict,
    out->summaryInfo.referenceClueOnly,
    out->summaryInfo.stackwalkDegraded,
    out->summaryInfo.symbolRuntimeDegraded,
    out->summaryInfo.firstChanceCandidateWeak,
    cfg.dumpMode);
  out->manifestPath = CrashManifestPathForDump(task.dumpPath, outBase);

  if (err) {
    err->clear();
  }
  return true;
}

}  // namespace skydiag::helper::internal
