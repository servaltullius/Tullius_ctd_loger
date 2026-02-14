#include "CrashEtwCapture.h"

#include <Windows.h>

#include <filesystem>
#include <string>

#include "EtwCapture.h"
#include "HelperLog.h"
#include "IncidentManifest.h"
#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/ProcessAttach.h"
#include "SkyrimDiagHelper/Retention.h"

namespace skydiag::helper::internal {

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
  bool timeUp = false;
  if (pending->captureSeconds > 0 && nowTick >= pending->startedAtTick64) {
    const ULONGLONG elapsedMs = nowTick - pending->startedAtTick64;
    timeUp = elapsedMs >= (static_cast<ULONGLONG>(pending->captureSeconds) * 1000ull);
  }

  if (!force && !procExited && !timeUp) {
    return;
  }

  std::wstring etwErr;
  if (StopEtwCaptureToPath(cfg, outBase, pending->etwPath, &etwErr)) {
    AppendLogLine(outBase, L"ETW crash capture written: " + pending->etwPath.wstring());
    if (cfg.enableIncidentManifest && !pending->manifestPath.empty()) {
      std::wstring updErr;
      if (!TryUpdateIncidentManifestEtw(pending->manifestPath, pending->etwPath, "written", &updErr)) {
        AppendLogLine(outBase, L"Incident manifest ETW update failed: " + updErr);
      }
    }
  } else {
    AppendLogLine(outBase, L"ETW crash capture stop failed: " + etwErr);
    if (cfg.enableIncidentManifest && !pending->manifestPath.empty()) {
      std::wstring updErr;
      if (!TryUpdateIncidentManifestEtw(pending->manifestPath, pending->etwPath, "stop_failed", &updErr)) {
        AppendLogLine(outBase, L"Incident manifest ETW update failed: " + updErr);
      }
    }
  }

  pending->active = false;

  skydiag::helper::RetentionLimits limits{};
  limits.maxCrashDumps = cfg.maxCrashDumps;
  limits.maxHangDumps = cfg.maxHangDumps;
  limits.maxManualDumps = cfg.maxManualDumps;
  limits.maxEtwTraces = cfg.maxEtwTraces;
  skydiag::helper::ApplyRetentionToOutputDir(outBase, limits);
}

}  // namespace skydiag::helper::internal
