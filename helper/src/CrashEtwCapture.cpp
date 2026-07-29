#include "CrashEtwCapture.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "CaptureCommon.h"
#include "EtwCapture.h"
#include "HelperLog.h"
#include "IncidentManifest.h"
#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/ProcessAttach.h"

namespace skydiag::helper::internal {
namespace {

constexpr std::uint32_t kMaxCrashEtwStopAttempts = 3u;
constexpr ULONGLONG kCrashEtwRetryDelayMs = 1000u;

void UpdateManifestStatus(
  const skydiag::helper::HelperConfig& cfg,
  const std::filesystem::path& outBase,
  const PendingCrashEtwCapture& pending,
  std::string_view status)
{
  if (!cfg.enableIncidentManifest || pending.manifestPath.empty()) {
    return;
  }
  std::wstring updErr;
  if (!TryUpdateIncidentManifestEtw(
        pending.manifestPath,
        pending.etwPath,
        status,
        &updErr)) {
    AppendLogLine(outBase, L"Incident manifest ETW update failed: " + updErr);
  }
}

}  // namespace

void MaybeStopPendingCrashEtwCapture(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  bool force,
  PendingCrashEtwCapture* pending)
{
  if (!pending || !pending->active) {
    return;
  }

  bool procExited = false;
  if (proc.process) {
    const DWORD w = WaitForSingleObject(proc.process, 0);
    procExited = (w == WAIT_OBJECT_0);
  }

  const ULONGLONG nowTick = GetTickCount64();
  if (pending->cleanupPending &&
      !force &&
      nowTick < pending->nextCleanupAttemptTick64) {
    return;
  }
  bool timeUp = false;
  if (pending->captureSeconds > 0 && nowTick >= pending->startedAtTick64) {
    const ULONGLONG elapsedMs = nowTick - pending->startedAtTick64;
    timeUp = elapsedMs >= (static_cast<ULONGLONG>(pending->captureSeconds) * 1000ull);
  }

  if (!force && !procExited && !timeUp) {
    return;
  }

  const std::uint32_t attemptsThisCall = force
    ? std::max<std::uint32_t>(
        1u,
        kMaxCrashEtwStopAttempts - std::min(
          pending->stopAttempts,
          kMaxCrashEtwStopAttempts))
    : 1u;
  std::wstring lastStopErr;
  for (std::uint32_t attempt = 0;
       attempt < attemptsThisCall &&
       pending->stopAttempts < kMaxCrashEtwStopAttempts;
       ++attempt) {
    if (StopEtwCaptureToPath(cfg, outBase, pending->etwPath, &lastStopErr)) {
      AppendLogLine(outBase, L"ETW crash capture written: " + pending->etwPath.wstring());
      UpdateManifestStatus(cfg, outBase, *pending, "written");
      pending->active = false;
      pending->cleanupPending = false;
      pending->nextCleanupAttemptTick64 = 0;
      ApplyRetentionFromConfig(cfg, outBase);
      return;
    }
    ++pending->stopAttempts;
    pending->cleanupPending = true;
    pending->nextCleanupAttemptTick64 = GetTickCount64() + kCrashEtwRetryDelayMs;
    AppendLogLine(
      outBase,
      L"ETW crash capture stop failed (attempt "
        + std::to_wstring(pending->stopAttempts)
        + L" of "
        + std::to_wstring(kMaxCrashEtwStopAttempts)
        + L"): "
        + lastStopErr);
    if (!force) {
      break;
    }
  }

  if (pending->stopAttempts < kMaxCrashEtwStopAttempts) {
    UpdateManifestStatus(cfg, outBase, *pending, "stop_retry_pending");
    return;
  }

  std::wstring cancelErr;
  if (CancelEtwCapture(cfg, outBase, &cancelErr)) {
    AppendLogLine(
      outBase,
      L"ETW crash capture stop failed repeatedly; WPR cancellation was confirmed.");
    UpdateManifestStatus(cfg, outBase, *pending, "cancelled_after_stop_failure");
    pending->active = false;
    pending->cleanupPending = false;
    pending->nextCleanupAttemptTick64 = 0;
    ApplyRetentionFromConfig(cfg, outBase);
    return;
  }

  pending->active = true;
  pending->cleanupPending = true;
  pending->nextCleanupAttemptTick64 = GetTickCount64() + kCrashEtwRetryDelayMs;
  AppendLogLine(
    outBase,
    L"ETW crash capture cleanup remains unconfirmed after bounded stop retries and wpr -cancel: "
      + cancelErr);
  UpdateManifestStatus(cfg, outBase, *pending, "cleanup_unconfirmed");
}

}  // namespace skydiag::helper::internal
