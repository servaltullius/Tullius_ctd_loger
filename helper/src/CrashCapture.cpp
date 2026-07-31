#include "CrashCapture.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <new>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "CrashEtwCapture.h"
#include "CaptureCommon.h"
#include "DumpToolLaunch.h"
#include "EtwCapture.h"
#include "HelperCommon.h"
#include "HelperLog.h"
#include "IncidentManifest.h"
#include "PendingCrashAnalysis.h"
#include "PluginScanner.h"
#include "HexFormat.h"
#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/DumpWriter.h"
#include "SkyrimDiagHelper/HeadlessAnalysisPolicy.h"
#include "SkyrimDiagHelper/ProcessAttach.h"
#include "SkyrimDiagShared.h"

namespace skydiag::helper::internal {
void WriteWerFallbackHint(const std::filesystem::path& outBase)
{
  const std::string hint =
    "SkyrimDiag dump capture failed. As a fallback, you can enable Windows Error Reporting LocalDumps.\n"
    "Registry path:\n"
    "  HKLM\\SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting\\LocalDumps\\SkyrimSE.exe\n"
    "Recommended values:\n"
    "  DumpType (DWORD) = 2   ; full dump\n"
    "  DumpCount (DWORD) = 10\n"
    "  DumpFolder (EXPAND_SZ) = <your output folder>\n"
    "Reference: https://learn.microsoft.com/windows/win32/wer/collecting-user-mode-dumps\n";
  WriteTextFileUtf8(outBase / L"SkyrimDiag_WER_LocalDumps_Hint.txt", hint);
}

namespace {

constexpr DWORD kShutdownWaitMs = 3000;
constexpr int kMaxHeartbeatChecks = 4;
constexpr DWORD kHeartbeatCheckIntervalMs = 2000;
constexpr int kRequiredHeartbeatAdvances = 2;
constexpr int kStableSnapshotAttempts = 64;
constexpr int kRingEntrySnapshotAttempts = 8;

// The retry window is deliberately short: the faulting game process is usually
// seconds from exiting, and once it does the dump can no longer be produced.
constexpr DWORD kDumpRetryBackoffMs = 250;

bool IsProcessStillActive(HANDLE process) noexcept
{
  if (!process) {
    return false;
  }
  return WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}

struct FileRemovalObservation {
  bool existedBefore = false;
  bool existsAfter = false;
  std::error_code error;
};

FileRemovalObservation RemoveFileAndObserve(const std::filesystem::path& path) noexcept
{
  FileRemovalObservation result{};
  std::error_code ec;
  result.existedBefore = std::filesystem::exists(path, ec);
  if (ec) {
    result.error = ec;
    result.existsAfter = true;
    return result;
  }

  std::filesystem::remove(path, ec);
  if (ec) {
    result.error = ec;
  }
  ec.clear();
  result.existsAfter = std::filesystem::exists(path, ec);
  if (ec) {
    result.error = ec;
    // Fail closed when the final state cannot be observed.
    result.existsAfter = true;
  }
  return result;
}

std::uint32_t ReadCrashSequence(const skydiag::SharedHeader* header) noexcept
{
  if (!header) {
    return 0u;
  }
  auto* const sequence = reinterpret_cast<volatile LONG*>(
    const_cast<volatile std::uint32_t*>(&header->crash_seq));
  return static_cast<std::uint32_t>(InterlockedCompareExchange(sequence, 0, 0));
}

template <class Entry>
std::uint32_t ReadEntrySequence(const Entry* entry) noexcept
{
  auto* const sequence = reinterpret_cast<volatile LONG*>(
    const_cast<volatile std::uint32_t*>(&entry->seq));
  return static_cast<std::uint32_t>(InterlockedCompareExchange(sequence, 0, 0));
}

template <class Entry>
bool CopyStableSeqlockEntry(const Entry* source, Entry* destination) noexcept
{
  if (!source || !destination) {
    return false;
  }
  for (int attempt = 0; attempt < kRingEntrySnapshotAttempts; ++attempt) {
    const std::uint32_t before = ReadEntrySequence(source);
    if ((before & 1u) != 0u) {
      SwitchToThread();
      continue;
    }

    Entry local{};
    std::memcpy(&local, source, sizeof(local));
    MemoryBarrier();
    const std::uint32_t after = ReadEntrySequence(source);
    if (before == after && (after & 1u) == 0u) {
      std::memcpy(destination, &local, sizeof(local));
      return true;
    }
    SwitchToThread();
  }

  std::memset(destination, 0, sizeof(*destination));
  destination->seq = 1u;  // odd => intentionally invalid/unstable
  return false;
}

bool TryCaptureDumpIdentity(
  const std::filesystem::path& dumpPath,
  CleanExitDumpIdentity* out) noexcept
{
  if (out) {
    *out = CleanExitDumpIdentity{};
  }
  if (!out || dumpPath.empty()) {
    return false;
  }

  WIN32_FILE_ATTRIBUTE_DATA attributes{};
  if (!GetFileAttributesExW(
        dumpPath.c_str(),
        GetFileExInfoStandard,
        &attributes)) {
    return false;
  }

  ULARGE_INTEGER size{};
  size.HighPart = attributes.nFileSizeHigh;
  size.LowPart = attributes.nFileSizeLow;
  ULARGE_INTEGER lastWrite{};
  lastWrite.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
  lastWrite.LowPart = attributes.ftLastWriteTime.dwLowDateTime;

  out->valid = true;
  out->filename = dumpPath.filename().wstring();
  out->sizeBytes = size.QuadPart;
  out->lastWriteTimeUtc100ns = lastWrite.QuadPart;
  return true;
}

const char* CleanExitDumpStateId(CleanExitDumpState state) noexcept
{
  switch (state) {
    case CleanExitDumpState::kPendingDelete:
      return "pending_delete";
    case CleanExitDumpState::kDiscarded:
      return "discarded";
    case CleanExitDumpState::kPreserved:
      return "preserved";
    case CleanExitDumpState::kDeleteFailed:
      return "delete_failed";
    case CleanExitDumpState::kNotCaptured:
      return "not_captured";
  }
  return "not_captured";
}

const wchar_t* CleanExitDumpStateWideId(CleanExitDumpState state) noexcept
{
  switch (state) {
    case CleanExitDumpState::kPendingDelete:
      return L"pending_delete";
    case CleanExitDumpState::kDiscarded:
      return L"discarded";
    case CleanExitDumpState::kPreserved:
      return L"preserved";
    case CleanExitDumpState::kDeleteFailed:
      return L"delete_failed";
    case CleanExitDumpState::kNotCaptured:
      return L"not_captured";
  }
  return L"not_captured";
}

bool IsFinalCleanExitDumpState(CleanExitDumpState state) noexcept
{
  return state != CleanExitDumpState::kPendingDelete;
}

// Records the evidence of a strong fault that a zero exit code caused us to
// filter. Deletion uses a two-phase record: `pending_delete` is committed before
// touching the dump, then the same file is atomically replaced with the observed
// final state. A crash or disk failure can therefore leave "pending", but can
// never leave a false claim that the dump was discarded.
bool WriteCleanExitEvidenceRecord(
  const std::filesystem::path& recordPath,
  const CrashEventInfo& info,
  const CleanExitDumpIdentity& dumpIdentity,
  std::wstring_view context,
  CleanExitDumpState dumpState)
{
  nlohmann::json j = nlohmann::json::object();
  j["schema"] = "skydiag.clean_exit_evidence.v1";
  j["dump_state"] = CleanExitDumpStateId(dumpState);
  switch (dumpState) {
    case CleanExitDumpState::kPendingDelete:
      j["reason"] = "strong_fault_published_but_process_exited_zero_dump_delete_pending";
      j["dump_preserved"] = nullptr;
      break;
    case CleanExitDumpState::kDiscarded:
      j["reason"] = "strong_fault_published_but_process_exited_zero_dump_discarded";
      j["dump_preserved"] = false;
      break;
    case CleanExitDumpState::kPreserved:
      j["reason"] = "strong_fault_published_but_process_exited_zero_dump_preserved";
      j["dump_preserved"] = true;
      break;
    case CleanExitDumpState::kDeleteFailed:
      j["reason"] = "strong_fault_published_but_process_exited_zero_dump_delete_failed";
      j["dump_preserved"] = true;
      break;
    case CleanExitDumpState::kNotCaptured:
      j["reason"] = "strong_fault_published_after_final_event_poll_dump_not_captured";
      j["dump_preserved"] = false;
      break;
  }
  j["captured_at"] = WideToUtf8(Timestamp());
  j["filter_context"] = WideToUtf8(context);
  j["exception_code"] = info.exceptionCode;
  j["exception_addr"] = info.exceptionAddr;
  j["faulting_tid"] = info.faultingTid;
  j["state_flags"] = info.stateFlags;
  j["crash_seq"] = info.crashSeq;
  j["in_menu"] = info.inMenu;
  if (dumpIdentity.valid) {
    j["dump_filename"] = WideToUtf8(dumpIdentity.filename);
    j["dump_size_bytes"] = dumpIdentity.sizeBytes;
    j["dump_last_write_time_utc_100ns"] = dumpIdentity.lastWriteTimeUtc100ns;
  } else {
    j["dump_filename"] = nullptr;
    j["dump_size_bytes"] = nullptr;
    j["dump_last_write_time_utc_100ns"] = nullptr;
  }
  if (dumpState == CleanExitDumpState::kPendingDelete) {
    j["note"] =
      "The evidence record was committed before dump deletion. A final atomic update "
      "will record whether the dump was discarded or remained on disk.";
  } else if (dumpState == CleanExitDumpState::kPreserved) {
    j["note"] =
      "The game published a strong fault record but exited with code 0. The filtered "
      "dump remains available; derived crash actions were suppressed.";
  } else if (dumpState == CleanExitDumpState::kDeleteFailed) {
    j["note"] =
      "The game published a strong fault record but exited with code 0. Dump deletion "
      "failed, so the file remains available and the observed state is preserved.";
  } else if (dumpState == CleanExitDumpState::kNotCaptured) {
    j["note"] =
      "The strong fault was published after the helper's final live-process event poll. "
      "The process had already exited, so only immutable metadata could be preserved.";
  } else {
    j["note"] =
      "The game published a strong fault record but exited with code 0. The dump was "
      "successfully discarded and derived crash actions were suppressed.";
  }

  if (!WriteTextFileUtf8(recordPath, j.dump(2))) {
    AppendLogLine(
      recordPath.parent_path(),
      L"Failed to atomically update clean-exit evidence metadata; any existing record remains authoritative: "
        + recordPath.wstring());
    return false;
  }
  AppendLogLine(
    recordPath.parent_path(),
    L"Zero-exit filter wrote evidence metadata (dump_state="
      + std::wstring(CleanExitDumpStateWideId(dumpState))
      + L"): "
      + recordPath.wstring());
  return true;
}

FilterVerdict ClassifyExitCodeVerdictWithContext(
  const skydiag::helper::HelperConfig& cfg,
  std::uint32_t exitCode,
  const CrashEventInfo& info,
  const std::filesystem::path& outBase,
  std::wstring_view context,
  int checkIndex,
  CrashCaptureState* crashState)
{
  const auto verdict = ClassifyExitCodeVerdict(exitCode, info, outBase);
  const std::wstring checkSuffix = (checkIndex >= 0)
    ? (L", check=" + std::to_wstring(checkIndex + 1))
    : L"";

  if (verdict == FilterVerdict::kDeleteBenign) {
    AppendLogLine(
      outBase,
      L"Crash event received but process exited normally (context=" + std::wstring(context)
        + L", exit_code=0"
        + checkSuffix
        + L"); deleting dump (handled first-chance or shutdown exception)."
    );
    if (crashState && cfg.enableCleanExitEvidenceQuarantine &&
        ShouldQuarantineCleanExitEvidence(exitCode, info)) {
      crashState->cleanExitFilterContext.assign(context);
    }
    return verdict;
  }

  if (info.inMenu && exitCode != 0) {
    AppendLogLine(
      outBase,
      L"Crash event reached menu/shutdown boundary with non-zero exit (context="
        + std::wstring(context)
        + L", exit_code="
        + std::to_wstring(exitCode)
        + checkSuffix
        + L", state_flags="
        + std::to_wstring(info.stateFlags)
        + L"); keeping dump and preserving crash auto-actions.");
  }

  return verdict;
}

bool HasNewerCrashRecord(
  const skydiag::SharedHeader* shm,
  std::uint32_t expectedCrashSeq) noexcept
{
  const auto current = ReadCrashSequence(shm);
  return current != 0u && current != expectedCrashSeq;
}

DWORD WaitForCrashOrProcess(HANDLE crashEvent, HANDLE process, DWORD waitMs) noexcept
{
  if (crashEvent && process) {
    // Crash event first: if both handles become signaled together, Windows
    // returns the lowest index and we get a chance to snapshot the newer crash
    // before the process address space disappears.
    const HANDLE handles[] = { crashEvent, process };
    return WaitForMultipleObjects(static_cast<DWORD>(std::size(handles)), handles, FALSE, waitMs);
  }
  if (process) {
    const DWORD result = WaitForSingleObject(process, waitMs);
    if (result == WAIT_OBJECT_0) {
      return WAIT_OBJECT_0 + 1u;
    }
    return result;
  }
  return WAIT_FAILED;
}

// Reads the exit code, reporting whether it is trustworthy. A failed query used
// to leave the STILL_ACTIVE initializer in place, which reads as a non-zero
// (crash-like) exit; callers must not treat that as an observed exit code.
bool TryReadExitCode(
  HANDLE process,
  const std::filesystem::path& outBase,
  DWORD* outExitCode) noexcept
{
  DWORD exitCode = STILL_ACTIVE;
  if (!GetExitCodeProcess(process, &exitCode)) {
    AppendLogLine(
      outBase,
      L"GetExitCodeProcess failed (error=" + std::to_wstring(GetLastError())
        + L"); treating the exit code as unknown and keeping the dump.");
    return false;
  }
  if (outExitCode) {
    *outExitCode = exitCode;
  }
  return true;
}

FilterVerdict FilterShutdownException(
  const skydiag::helper::HelperConfig& cfg,
  HANDLE process,
  HANDLE crashEvent,
  const skydiag::SharedHeader* shm,
  const CrashEventInfo& info,
  const std::filesystem::path& outBase,
  CrashCaptureState* crashState)
{
  const DWORD pw = WaitForCrashOrProcess(crashEvent, process, kShutdownWaitMs);
  if (pw == WAIT_OBJECT_0 && crashEvent) {
    if (HasNewerCrashRecord(shm, info.crashSeq)) {
      AppendLogLine(outBase, L"A newer crash record arrived during shutdown filtering; prioritizing the newer CTD.");
      return FilterVerdict::kRetryNewerCrash;
    }
  }
  if (pw == WAIT_OBJECT_0 + 1u) {
    DWORD exitCode = STILL_ACTIVE;
    if (!TryReadExitCode(process, outBase, &exitCode)) {
      return FilterVerdict::kKeepDump;
    }
    return ClassifyExitCodeVerdictWithContext(
      cfg, exitCode, info, outBase, L"shutdown", -1, crashState);
  }
  if (pw == WAIT_TIMEOUT) {
    return FilterVerdict::kKeepDump;
  }
  return FilterVerdict::kKeepDump;
}

FilterVerdict FilterFirstChanceException(
  const skydiag::helper::HelperConfig& cfg,
  HANDLE process,
  HANDLE crashEvent,
  const skydiag::SharedHeader* shm,
  const CrashEventInfo& info,
  const std::filesystem::path& outBase,
  CrashCaptureState* crashState)
{
  if (!shm) {
    return FilterVerdict::kKeepDump;
  }

  int heartbeatAdvanceCount = 0;
  for (int attempt = 0; attempt < kMaxHeartbeatChecks; ++attempt) {
    const auto hb0 = shm->last_heartbeat_qpc;
    const DWORD waitResult = WaitForCrashOrProcess(crashEvent, process, kHeartbeatCheckIntervalMs);
    if (waitResult == WAIT_OBJECT_0 && crashEvent) {
      if (HasNewerCrashRecord(shm, info.crashSeq)) {
        AppendLogLine(
          outBase,
          L"A newer crash record arrived during first-chance recovery filtering; prioritizing the newer CTD.");
        return FilterVerdict::kRetryNewerCrash;
      }
    }
    if (waitResult == WAIT_OBJECT_0 + 1u) {
      DWORD exitCode = STILL_ACTIVE;
      if (!TryReadExitCode(process, outBase, &exitCode)) {
        return FilterVerdict::kKeepDump;
      }
      return ClassifyExitCodeVerdictWithContext(
        cfg, exitCode, info, outBase, L"heartbeat_check", attempt, crashState);
    }

    const auto hb1 = shm->last_heartbeat_qpc;
    if (hb1 > hb0) {
      ++heartbeatAdvanceCount;
      if (heartbeatAdvanceCount >= kRequiredHeartbeatAdvances) {
        AppendLogLine(
          outBase,
          L"Crash event received but heartbeat is still advancing across multiple checks (hb0="
            + std::to_wstring(hb0)
            + L" hb1="
            + std::to_wstring(hb1)
            + L", check="
            + std::to_wstring(attempt + 1)
            + L", advances="
            + std::to_wstring(heartbeatAdvanceCount)
            + L"); deleting dump (likely handled first-chance exception)."
        );
        return FilterVerdict::kDeleteRecovered;
      }
    }
  }

  return FilterVerdict::kKeepDump;
}

void ProcessValidCrashDump(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  const std::wstring& dumpPath,
  const std::wstring& ts,
  const CrashEventInfo& info,
  PendingCrashEtwCapture* pendingCrashEtw,
  PendingCrashAnalysis* pendingCrashAnalysis,
  std::wstring* pendingCrashViewerDumpPath)
{
  const auto etwPath = outBase / (L"SkyrimDiag_Crash_" + ts + L".etl");
  const auto manifestPath = outBase / (L"SkyrimDiag_Incident_Crash_" + ts + L".json");
  const std::filesystem::path dumpFs(dumpPath);

  std::wcout << L"[SkyrimDiagHelper] Crash dump written: " << dumpPath << L"\n";

  {
    const std::string pluginScanJson = CollectPluginScanJson(proc, outBase);
    if (!pluginScanJson.empty()) {
      const auto pluginScanPath = dumpFs.parent_path() / (dumpFs.stem().wstring() + L"_PluginScan.json");
      WriteTextFileUtf8(pluginScanPath, pluginScanJson);
      AppendLogLine(outBase, L"Plugin scan sidecar written: " + pluginScanPath.wstring());
    }
  }

  bool etwStarted = false;
  std::string etwStatus = cfg.enableEtwCaptureOnCrash ? "start_failed" : "disabled";
  if (cfg.enableEtwCaptureOnCrash && pendingCrashEtw && !pendingCrashEtw->active) {
    const std::wstring effectiveProfile = cfg.etwCrashProfile.empty() ? L"GeneralProfile" : cfg.etwCrashProfile;
    std::wstring etwErr;
    if (StartEtwCaptureWithProfile(cfg, outBase, effectiveProfile, &etwErr)) {
      etwStarted = true;
      etwStatus = "recording";

      pendingCrashEtw->active = true;
      pendingCrashEtw->etwPath = etwPath;
      pendingCrashEtw->manifestPath = cfg.enableIncidentManifest ? manifestPath : std::filesystem::path{};
      pendingCrashEtw->startedAtTick64 = GetTickCount64();
      pendingCrashEtw->captureSeconds = cfg.etwCrashCaptureSeconds;
      pendingCrashEtw->profileUsed = effectiveProfile;
      pendingCrashEtw->cleanupPending = false;
      pendingCrashEtw->stopAttempts = 0;
      pendingCrashEtw->nextCleanupAttemptTick64 = 0;

      AppendLogLine(
        outBase,
        L"ETW crash capture started (profile=" + effectiveProfile
          + L", seconds="
          + std::to_wstring(pendingCrashEtw->captureSeconds)
          + L").");
    } else {
      AppendLogLine(outBase, L"ETW crash capture start failed: " + etwErr);
    }
  }

  if (cfg.enableIncidentManifest) {
    nlohmann::json ctx = nlohmann::json::object();
    ctx["reason"] = "crash_event";
    const auto dumpProfile = skydiag::helper::ResolveDumpProfile(
      cfg.dumpMode,
      skydiag::helper::CaptureKind::Crash);
    const auto manifest = MakeIncidentManifestV1(
      "crash",
      ts,
      proc.pid,
      std::filesystem::path(dumpPath),
      std::nullopt,
      etwStarted ? std::optional<std::filesystem::path>(etwPath) : std::nullopt,
      etwStatus,
      info.stateFlags,
      ctx,
      &dumpProfile,
      /*recaptureDecision=*/nullptr,
      cfg,
      cfg.incidentManifestIncludeConfigSnapshot);
    WriteTextFileUtf8(manifestPath, manifest.dump(2));
    AppendLogLine(outBase, L"Incident manifest written: " + manifestPath.wstring());
  }

  bool crashAnalysisQueued = false;
  if (cfg.autoAnalyzeDump && cfg.enableAutoRecaptureOnUnknownCrash) {
    std::wstring analyzeQueueErr;
    if (StartPendingCrashAnalysisTask(cfg, dumpPath, outBase, pendingCrashAnalysis, &analyzeQueueErr)) {
      crashAnalysisQueued = true;
      AppendLogLine(outBase, L"Crash headless analysis queued for unknown-bucket recapture policy.");
    } else {
      AppendLogLine(outBase, L"Crash headless analysis queue failed: " + analyzeQueueErr);
    }
  }

  bool viewerNow = false;
  if (cfg.autoOpenViewerOnCrash) {
    if (!cfg.autoOpenCrashOnlyIfProcessExited) {
      const auto launch = StartDumpToolViewer(cfg, dumpPath, outBase, L"crash");
      viewerNow = (launch == DumpToolViewerLaunchResult::kLaunched);
    } else if (proc.process) {
      const DWORD waitExitMs = static_cast<DWORD>(std::min<std::uint32_t>(cfg.autoOpenCrashWaitForExitMs, 10000u));
      const DWORD wExit = WaitForSingleObject(proc.process, waitExitMs);
      if (wExit == WAIT_OBJECT_0) {
        DWORD processExitCode = STILL_ACTIVE;
        if (!GetExitCodeProcess(proc.process, &processExitCode)) {
          processExitCode = 0xFFFFFFFFu;
        }
        if (processExitCode == 0) {
          AppendLogLine(
            outBase,
            L"Process exited with exit_code=0 during wait window; "
              L"suppressing deferred crash viewer; normal-exit cleanup will remove filtered crash artifacts (wait_ms="
              + std::to_wstring(waitExitMs)
              + L", dump="
              + dumpFs.filename().wstring()
              + L").");
        } else {
          const auto launch = StartDumpToolViewer(cfg, dumpPath, outBase, L"crash_exit");
          viewerNow = (launch == DumpToolViewerLaunchResult::kLaunched);
          if (viewerNow) {
            AppendLogLine(
              outBase,
              L"Auto-opened DumpTool viewer for crash after process exit during wait window (wait_ms="
                + std::to_wstring(waitExitMs)
                + L", dump="
                + dumpFs.filename().wstring()
                + L").");
          } else {
            AppendLogLine(
              outBase,
              L"Crash viewer auto-open attempt failed after process exit during wait window (wait_ms="
                + std::to_wstring(waitExitMs)
                + L", dump="
                + dumpFs.filename().wstring()
                + L").");
          }
        }
      } else if (wExit == WAIT_TIMEOUT) {
        const bool deferred = QueueDeferredCrashViewer(dumpPath, pendingCrashViewerDumpPath);
        AppendLogLine(
          outBase,
          L"Crash dump captured but process is still running after auto-open wait (wait_ms="
            + std::to_wstring(waitExitMs)
            + L", dump="
            + dumpFs.filename().wstring()
            + L"); "
            + (deferred ? L"deferring viewer to process exit." : L"deferred viewer queue unchanged."));
      } else {
        const DWORD le = GetLastError();
        const bool deferred = QueueDeferredCrashViewer(dumpPath, pendingCrashViewerDumpPath);
        AppendLogLine(
          outBase,
          L"Crash viewer auto-open wait failed (wait_ms="
            + std::to_wstring(waitExitMs)
            + L", err="
            + std::to_wstring(le)
            + L"); "
            + (deferred ? L"deferring viewer to process exit." : L"deferred viewer queue unchanged."));
      }
    } else {
      AppendLogLine(outBase, L"Crash viewer auto-open suppressed: missing process handle.");
    }
  }

  if (!crashAnalysisQueued) {
    if (ShouldRunHeadlessDumpAnalysis(cfg, viewerNow, false)) {
      std::wstring analyzeQueueErr;
      if (StartPendingCrashAnalysisTask(cfg, dumpPath, outBase, pendingCrashAnalysis, &analyzeQueueErr)) {
        crashAnalysisQueued = true;
        AppendLogLine(outBase, L"Crash headless analysis queued with tracked process lifetime.");
      } else {
        AppendLogLine(outBase, L"Crash headless analysis queue failed: " + analyzeQueueErr);
      }
    } else if (viewerNow && cfg.autoAnalyzeDump) {
      AppendLogLine(outBase, L"Skipped headless analysis: viewer auto-open is enabled.");
    }
  }

  ApplyRetentionFromConfig(cfg, outBase);
}

}

bool IsCleanExitEvidenceRequired(
  const skydiag::helper::HelperConfig& cfg,
  const CrashCaptureState* crashState) noexcept
{
  return crashState &&
         cfg.enableCleanExitEvidenceQuarantine &&
         ShouldQuarantineCleanExitEvidence(0u, crashState->capturedInfo);
}

bool TryWriteCleanExitEvidenceRecord(
  const skydiag::helper::HelperConfig& cfg,
  const std::filesystem::path& outBase,
  CrashCaptureState* crashState,
  std::wstring_view context,
  const std::filesystem::path& dumpPath,
  CleanExitDumpState dumpState)
{
  if (!IsCleanExitEvidenceRequired(cfg, crashState)) {
    return false;
  }
  if (crashState->cleanExitEvidenceFinalized) {
    return true;
  }

  if (crashState->cleanExitEvidencePath.empty()) {
    crashState->cleanExitEvidencePath =
      outBase
      / (L"SkyrimDiag_CleanExitEvidence_"
         + Timestamp()
         + L"_"
         + std::to_wstring(crashState->capturedInfo.crashSeq)
         + L".json");
  }
  if (!crashState->cleanExitDumpIdentity.valid && !dumpPath.empty()) {
    TryCaptureDumpIdentity(dumpPath, &crashState->cleanExitDumpIdentity);
  }

  if (!WriteCleanExitEvidenceRecord(
        crashState->cleanExitEvidencePath,
        crashState->capturedInfo,
        crashState->cleanExitDumpIdentity,
        context,
        dumpState)) {
    return false;
  }
  crashState->cleanExitEvidenceWritten = true;
  crashState->cleanExitEvidenceFinalized = IsFinalCleanExitDumpState(dumpState);
  return true;
}

const skydiag::SharedLayout* StableSharedSnapshot::layout() const noexcept
{
  if (byteSize < sizeof(skydiag::SharedLayout) || !storage) {
    return nullptr;
  }
  return std::launder(reinterpret_cast<const skydiag::SharedLayout*>(storage.get()));
}

std::size_t StableSharedSnapshot::size() const noexcept
{
  return byteSize;
}

void StableSharedSnapshot::AlignedByteDeleter::operator()(std::byte* ptr) const noexcept
{
  if (ptr) {
    ::operator delete(ptr, std::align_val_t{alignment});
  }
}

bool CaptureStableSharedSnapshot(
  const skydiag::SharedLayout* shm,
  std::size_t shmBytes,
  StableSharedSnapshot* out) noexcept
{
  if (!shm || !out || shmBytes < sizeof(skydiag::SharedLayout)) {
    return false;
  }

  out->storage.reset();
  out->byteSize = 0;
  constexpr std::size_t kSnapshotAlignment = alignof(skydiag::SharedLayout);
  auto* rawStorage = static_cast<std::byte*>(::operator new(
    sizeof(skydiag::SharedLayout),
    std::align_val_t{kSnapshotAlignment},
    std::nothrow));
  if (!rawStorage) {
    return false;
  }
  out->storage = StableSharedSnapshot::Storage(
    rawStorage,
    StableSharedSnapshot::AlignedByteDeleter{kSnapshotAlignment});
  out->byteSize = sizeof(skydiag::SharedLayout);

  for (int attempt = 0; attempt < kStableSnapshotAttempts; ++attempt) {
    const std::uint32_t before = ReadCrashSequence(&shm->header);
    if ((before & 1u) != 0u) {
      SwitchToThread();
      continue;
    }

    auto* const snapshot =
      std::launder(reinterpret_cast<skydiag::SharedLayout*>(out->storage.get()));
    std::memset(snapshot, 0, sizeof(*snapshot));
    std::memcpy(&snapshot->header, &shm->header, sizeof(snapshot->header));

    for (std::size_t i = 0; i < skydiag::kEventCapacity; ++i) {
      (void)CopyStableSeqlockEntry(&shm->events[i], &snapshot->events[i]);
    }

    auto* const resourceWriteIndex = reinterpret_cast<volatile LONG*>(
      const_cast<volatile std::uint32_t*>(&shm->resources.write_index));
    snapshot->resources.write_index = static_cast<std::uint32_t>(
      InterlockedCompareExchange(resourceWriteIndex, 0, 0));
    snapshot->resources.reserved = shm->resources.reserved;
    for (std::size_t i = 0; i < skydiag::kResourceCapacity; ++i) {
      (void)CopyStableSeqlockEntry(
        &shm->resources.entries[i],
        &snapshot->resources.entries[i]);
    }

    MemoryBarrier();
    const std::uint32_t after = ReadCrashSequence(&shm->header);
    if (before == after &&
        (after & 1u) == 0u &&
        snapshot->header.crash_seq == after) {
      return true;
    }
    SwitchToThread();
  }

  out->storage.reset();
  out->byteSize = 0;
  return false;
}

bool TryClearRecoveredCrashFreeze(
  skydiag::SharedLayout* shm,
  std::uint32_t expectedCrashSeq) noexcept
{
  if (!shm || expectedCrashSeq == 0u || (expectedCrashSeq & 1u) != 0u) {
    return false;
  }

  if (ReadCrashSequence(&shm->header) != expectedCrashSeq) {
    return false;
  }

  InterlockedAnd(
    reinterpret_cast<volatile LONG*>(&shm->header.state_flags),
    ~static_cast<LONG>(skydiag::kState_Frozen));
  MemoryBarrier();

  if (ReadCrashSequence(&shm->header) != expectedCrashSeq) {
    // A newer crash began or committed while the recovered record was being
    // thawed. Restore the freeze so its evidence cannot be overwritten.
    InterlockedOr(
      reinterpret_cast<volatile LONG*>(&shm->header.state_flags),
      static_cast<LONG>(skydiag::kState_Frozen));
    return false;
  }
  return true;
}

CrashEventInfo ExtractCrashInfo(const skydiag::SharedHeader* shm) noexcept
{
  if (!shm) {
    return {};
  }
  auto info = BuildCrashEventInfo(
    shm->crash.exception_code,
    shm->crash.exception_addr,
    shm->crash.faulting_tid,
    shm->state_flags);
  info.crashSeq = ReadCrashSequence(shm);
  return info;
}

bool TryCaptureCommittedCrashInfo(
  const skydiag::SharedHeader* shm,
  CrashEventInfo* out) noexcept
{
  if (out) {
    *out = CrashEventInfo{};
  }
  if (!shm || !out) {
    return false;
  }

  for (int attempt = 0; attempt < kStableSnapshotAttempts; ++attempt) {
    const std::uint32_t before = ReadCrashSequence(shm);
    if (!IsCommittedCrashSequence(before)) {
      if ((before & 1u) != 0u) {
        SwitchToThread();
        continue;
      }
      return false;
    }

    skydiag::CrashInfo crash{};
    std::memcpy(&crash, &shm->crash, sizeof(crash));
    auto* const stateFlagsWord = reinterpret_cast<volatile LONG*>(
      const_cast<volatile std::uint32_t*>(&shm->state_flags));
    const auto stateFlags = static_cast<std::uint32_t>(
      InterlockedCompareExchange(stateFlagsWord, 0, 0));
    MemoryBarrier();
    const std::uint32_t after = ReadCrashSequence(shm);
    if (before == after && IsCommittedCrashSequence(after)) {
      *out = BuildCrashEventInfo(
        crash.exception_code,
        crash.exception_addr,
        crash.faulting_tid,
        stateFlags);
      out->crashSeq = after;
      return true;
    }
    SwitchToThread();
  }
  return false;
}

bool TryWritePostExitCrashEvidenceRecord(
  const std::filesystem::path& outBase,
  const CrashEventInfo& info,
  DWORD exitCode)
{
  const auto ts = Timestamp();
  nlohmann::json j = nlohmann::json::object();
  j["schema"] = "skydiag.post_exit_crash_evidence.v1";
  j["reason"] = "committed_fault_published_after_final_live_process_poll";
  j["captured_at"] = WideToUtf8(ts);
  j["capture_status"] = "metadata_only_process_already_exited";
  j["dump_captured"] = false;
  j["exit_code"] = exitCode;
  j["exception_code"] = info.exceptionCode;
  j["exception_addr"] = info.exceptionAddr;
  j["faulting_tid"] = info.faultingTid;
  j["state_flags"] = info.stateFlags;
  j["crash_seq"] = info.crashSeq;
  j["in_menu"] = info.inMenu;
  const auto path =
    outBase
    / (L"SkyrimDiag_PostExitCrashEvidence_"
       + ts
       + L"_"
       + std::to_wstring(info.crashSeq)
       + L".json");
  if (!WriteTextFileUtf8(path, j.dump(2))) {
    AppendLogLine(
      outBase,
      L"Failed to write post-exit crash evidence metadata: " + path.wstring());
    return false;
  }
  AppendLogLine(
    outBase,
    L"Captured committed crash metadata after process exit; no live address space "
    L"remained for dump capture: "
      + path.wstring());
  return true;
}

bool HandleCrashEventTick(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  DWORD waitMs,
  CrashCaptureState* crashState,
  PendingCrashEtwCapture* pendingCrashEtw,
  PendingCrashAnalysis* pendingCrashAnalysis,
  std::wstring* lastCrashDumpPath,
  std::wstring* pendingHangViewerDumpPath,
  std::wstring* pendingCrashViewerDumpPath)
{
  if (!proc.crashEvent) {
    Sleep(waitMs);
    return false;
  }

  const DWORD w = WaitForSingleObject(proc.crashEvent, waitMs);
  if (w == WAIT_FAILED) {
    AppendLogLine(outBase, L"Crash event wait failed: " + std::to_wstring(GetLastError()));
    if (waitMs > 0) {
      Sleep(waitMs);
    }
    return false;
  }
  if (w != WAIT_OBJECT_0) {
    return false;
  }

  if (!ResetEvent(proc.crashEvent)) {
    AppendLogLine(outBase, L"Failed to reset crash event: " + std::to_wstring(GetLastError()));
  }

  if (crashState && crashState->latched) {
    AppendLogLine(outBase, L"Crash event signaled again; ignoring (already captured).");
    return true;
  }

  StableSharedSnapshot stableSnapshot{};
  const skydiag::SharedLayout* dumpSnapshot = nullptr;
  std::size_t dumpSnapshotBytes = 0;
  if (proc.shm) {
    if (!CaptureStableSharedSnapshot(proc.shm, proc.shmSize, &stableSnapshot)) {
      const bool processStillActive =
        proc.process && WaitForSingleObject(proc.process, 0) == WAIT_TIMEOUT;
      AppendLogLine(
        outBase,
        processStillActive
          ? L"Crash event snapshot was not stable; scheduling a retry before dump capture."
          : L"Crash event snapshot was not stable after process exit; allowing exit fallback handling.");
      if (processStillActive) {
        SetEvent(proc.crashEvent);
      }
      return false;
    }
    dumpSnapshot = stableSnapshot.layout();
    dumpSnapshotBytes = stableSnapshot.size();
  }

  const auto info = ExtractCrashInfo(dumpSnapshot ? &dumpSnapshot->header : nullptr);
  if (!IsCommittedCrashSequence(info.crashSeq)) {
    AppendLogLine(
      outBase,
      L"Crash event rejected before dump capture because crash_seq is not a non-zero committed sequence "
        L"(crash_seq=" + std::to_wstring(info.crashSeq) + L").");
    return false;
  }
  if (crashState) {
    crashState->capturedInfo = info;
    crashState->cleanExitEvidenceWritten = false;
    crashState->cleanExitEvidenceFinalized = false;
    crashState->cleanExitEvidencePath.clear();
    crashState->cleanExitDumpIdentity = CleanExitDumpIdentity{};
    crashState->cleanExitFilterContext.clear();
  }
  AppendLogLine(
    outBase,
    L"Crash event signaled (exception_code=" + Hex32(info.exceptionCode)
      + L", exception_addr=" + Hex64(info.exceptionAddr)
      + L", tid=" + std::to_wstring(info.faultingTid)
      + L", crash_seq=" + std::to_wstring(info.crashSeq)
      + L").");

  const auto ts = Timestamp();
  const auto dumpPath = (outBase / (L"SkyrimDiag_Crash_" + ts + L".dmp")).wstring();
  const auto dumpProfile = skydiag::helper::ResolveDumpProfile(
    cfg.dumpMode,
    skydiag::helper::CaptureKind::Crash);
  if (pendingHangViewerDumpPath) {
    pendingHangViewerDumpPath->clear();
  }

  // The crash event was already consumed and reset, and the game has committed
  // its record and will not signal again for this fault. Nothing would drive a
  // later attempt, so a transient failure (sharing violation, momentary disk
  // pressure) has to be retried here or the incident is lost outright.
  std::wstring dumpErr;
  bool dumpOk = false;
  for (int attempt = 0; attempt < kDumpWriteAttempts; ++attempt) {
    if (attempt > 0) {
      if (!ShouldAttemptDumpWrite(attempt, IsProcessStillActive(proc.process))) {
        AppendLogLine(
          outBase,
          L"Skipping remaining crash dump retries because the game process already exited; "
            L"its address space is gone and no dump can be produced.");
        break;
      }
      Sleep(kDumpRetryBackoffMs);
      std::error_code retryEc;
      std::filesystem::remove(dumpPath, retryEc);
      AppendLogLine(
        outBase,
        L"Retrying crash dump write (attempt " + std::to_wstring(attempt + 1)
          + L" of " + std::to_wstring(kDumpWriteAttempts) + L").");
    }

    dumpErr.clear();
    dumpOk = skydiag::helper::WriteDumpWithStreams(
      proc.process,
      proc.pid,
      dumpPath,
      dumpSnapshot,
      dumpSnapshotBytes,
      {},
      {},
      true,
      dumpProfile,
      /*isProcessSnapshot=*/false,
      &dumpErr);
    if (dumpOk) {
      if (attempt > 0) {
        AppendLogLine(
          outBase,
          L"Crash dump succeeded on attempt " + std::to_wstring(attempt + 1) + L".");
      }
      break;
    }

    AppendLogLine(
      outBase,
      L"Crash dump attempt " + std::to_wstring(attempt + 1) + L" failed: " + dumpErr);
  }

  if (!dumpOk) {
    AppendLogLine(outBase, L"Crash dump failed: " + dumpErr);
    std::wcerr << L"[SkyrimDiagHelper] Crash dump failed: " << dumpErr << L"\n";
    if (cfg.enableWerDumpFallbackHint) {
      WriteWerFallbackHint(outBase);
      AppendLogLine(
        outBase,
        L"Wrote WER LocalDumps fallback hint: " + (outBase / L"SkyrimDiag_WER_LocalDumps_Hint.txt").wstring());
    }
    std::error_code ec;
    std::filesystem::remove(dumpPath, ec);
    if (lastCrashDumpPath) {
      lastCrashDumpPath->clear();
    }

    // Terminal state for this incident: no dump exists, so there is no evidence
    // left to protect. Leaving the game frozen would keep blackbox recording
    // stopped and, with strong-fault preservation active, would block every
    // later CTD from being published. Thawing restores normal capture.
    if (proc.shmWritable) {
      const bool thawed = TryClearRecoveredCrashFreeze(proc.shmWritable, info.crashSeq);
      AppendLogLine(
        outBase,
        thawed
          ? L"Crash dump write failed after all retries; thawed capture so a later CTD can still be recorded."
          : L"Crash dump write failed after all retries; capture was not thawed because a newer crash record "
            L"is already present.");
    }
    if (crashState) {
      crashState->latched = false;
    }
    return true;
  }

  if (lastCrashDumpPath) {
    *lastCrashDumpPath = dumpPath;
  }

  auto verdict = FilterVerdict::kKeepDump;
  if (proc.process) {
    verdict = FilterShutdownException(
      cfg,
      proc.process,
      proc.crashEvent,
      proc.shm ? &proc.shm->header : nullptr,
      info,
      outBase,
      crashState);
    if (verdict == FilterVerdict::kKeepDump && proc.shm) {
      verdict = FilterFirstChanceException(
        cfg, proc.process, proc.crashEvent, &proc.shm->header, info, outBase, crashState);
    }
    if (verdict != FilterVerdict::kKeepDump) {
      if (verdict == FilterVerdict::kDeleteRecovered) {
        const bool thawed = TryClearRecoveredCrashFreeze(proc.shmWritable, info.crashSeq);
        AppendLogLine(
          outBase,
          thawed
            ? L"Recovered first-chance exception thawed blackbox/resource capture for a later real CTD."
            : L"Recovered first-chance exception was not thawed because a newer crash record is already present.");
      } else if (verdict == FilterVerdict::kRetryNewerCrash) {
        AppendLogLine(
          outBase,
          L"Discarding the superseded first-chance dump state and immediately retrying the newer crash event.");
      }

      bool preserveFilteredDump = cfg.preserveFilteredCrashDumps;
      bool dumpRemovalHandled = false;
      if (verdict == FilterVerdict::kDeleteBenign) {
        const std::wstring_view context =
          (crashState && !crashState->cleanExitFilterContext.empty())
            ? std::wstring_view(crashState->cleanExitFilterContext)
            : std::wstring_view(L"filtered_exit");
        const bool evidenceRequired = IsCleanExitEvidenceRequired(cfg, crashState);
        if (evidenceRequired && cfg.preserveFilteredCrashDumps) {
          (void)TryWriteCleanExitEvidenceRecord(
            cfg,
            outBase,
            crashState,
            context,
            dumpPath,
            CleanExitDumpState::kPreserved);
          preserveFilteredDump = true;
        } else if (evidenceRequired) {
          const bool pendingWritten = TryWriteCleanExitEvidenceRecord(
            cfg,
            outBase,
            crashState,
            context,
            dumpPath,
            CleanExitDumpState::kPendingDelete);
          if (!pendingWritten) {
            // Without a durable pending record, keep the dump as the fail-safe.
            preserveFilteredDump = true;
          } else {
            const auto removal = RemoveFileAndObserve(dumpPath);
            dumpRemovalHandled = true;
            preserveFilteredDump = removal.existsAfter;
            const auto finalState = removal.existsAfter
              ? CleanExitDumpState::kDeleteFailed
              : CleanExitDumpState::kDiscarded;
            if (!TryWriteCleanExitEvidenceRecord(
                  cfg,
                  outBase,
                  crashState,
                  context,
                  dumpPath,
                  finalState)) {
              AppendLogLine(
                outBase,
                L"Clean-exit evidence finalization failed; the durable pending_delete record "
                L"does not claim an unobserved dump state.");
            }
            if (removal.existsAfter) {
              AppendLogLine(
                outBase,
                L"Filtered crash dump removal failed or could not be verified; preserving observed file state"
                  + (removal.error
                       ? L" (err=" + std::to_wstring(removal.error.value()) + L")."
                       : L"."));
            }
          }
        }
      }
      if (preserveFilteredDump) {
        AppendLogLine(
          outBase,
          cfg.preserveFilteredCrashDumps
            ? L"Filtered dump file preserved without crash post-processing or capture latch "
              L"(PreserveFilteredCrashDumps=1)."
            : L"Clean-exit evidence metadata write failed; preserved the filtered dump as a fail-safe "
              L"without crash post-processing or capture latch.");
      } else if (!dumpRemovalHandled) {
        const auto removal = RemoveFileAndObserve(dumpPath);
        if (removal.existsAfter) {
          preserveFilteredDump = true;
          AppendLogLine(
            outBase,
            L"Filtered crash dump removal failed or could not be verified; file remains on disk"
              + (removal.error
                   ? L" (err=" + std::to_wstring(removal.error.value()) + L")."
                   : L"."));
        }
      }
      if (lastCrashDumpPath) {
        lastCrashDumpPath->clear();
      }
      if (crashState) {
        crashState->latched = false;
      }
      AppendLogLine(outBase, L"Filtered crash event consumed; capture remains ready for a later CTD.");
      return true;
    }
  }

  ProcessValidCrashDump(
    cfg,
    proc,
    outBase,
    dumpPath,
    ts,
    info,
    pendingCrashEtw,
    pendingCrashAnalysis,
    pendingCrashViewerDumpPath);

  if (crashState) {
    crashState->latched = ShouldLatchCrashCapture(verdict);
  }
  AppendLogLine(outBase, L"Crash captured; waiting for process exit.");
  return true;
}

}
