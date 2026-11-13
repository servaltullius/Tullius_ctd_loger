#include "CrashCapture.h"
#include "SkyrimDiagLongPath.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
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
#include "SeqlockCopy.h"
#include "HexFormat.h"
#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/DumpWriter.h"
#include "SkyrimDiagHelper/HeadlessAnalysisPolicy.h"
#include "SkyrimDiagHelper/ProcessAttach.h"
#include "SkyrimDiagShared.h"

namespace skydiag::helper::internal {
void WriteWerFallbackHint(const std::filesystem::path& outBase, WerFallbackReason reason, DWORD exitCode)
{
  WriteTextFileUtf8(outBase / L"SkyrimDiag_WER_LocalDumps_Hint.txt", BuildWerFallbackHintText(reason, exitCode));
}

namespace {

constexpr DWORD kShutdownWaitMs = 3000;
constexpr int kMaxHeartbeatChecks = 4;
constexpr DWORD kHeartbeatCheckIntervalMs = 2000;
constexpr int kRequiredHeartbeatAdvances = 2;
constexpr int kStableSnapshotAttempts = 64;

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
  try {
    std::error_code ec;
    // ExistsForIo: under MO2 a dump past MAX_PATH read as already gone.
    result.existedBefore = ExistsForIo(path, ec);
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
    result.existsAfter = ExistsForIo(path, ec);
    if (ec) {
      result.error = ec;
      // Fail closed when the final state cannot be observed.
      result.existsAfter = true;
    }
  } catch (...) {
    // ExistsForIo builds paths (out of memory): fail closed as above.
    result.error = std::make_error_code(std::errc::not_enough_memory);
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
  // late_recovery: the process did not exit; it kept running past the
  // late-recovery window, which proves the exception was handled.
  const std::string why = context == L"late_recovery"
    ? "strong_fault_published_but_game_kept_running"
    : "strong_fault_published_but_process_exited_zero";
  j["dump_state"] = CleanExitDumpStateId(dumpState);
  switch (dumpState) {
    case CleanExitDumpState::kPendingDelete:
      j["reason"] = why + "_dump_delete_pending";
      j["dump_preserved"] = nullptr;
      break;
    case CleanExitDumpState::kDiscarded:
      j["reason"] = why + "_dump_discarded";
      j["dump_preserved"] = false;
      break;
    case CleanExitDumpState::kPreserved:
      j["reason"] = why + "_dump_preserved";
      j["dump_preserved"] = true;
      break;
    case CleanExitDumpState::kDeleteFailed:
      j["reason"] = why + "_dump_delete_failed";
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
    j["note"] = context == L"late_recovery"
      ? "The game published a strong fault record but kept running for over a minute with "
        "its heartbeat advancing. The dump remains available; derived crash actions were suppressed."
      : "The game published a strong fault record but exited with code 0. The filtered "
        "dump remains available; derived crash actions were suppressed.";
  } else if (dumpState == CleanExitDumpState::kDeleteFailed) {
    j["note"] = context == L"late_recovery"
      ? "The game published a strong fault record but kept running. Dump deletion "
        "failed, so the file remains available and the observed state is preserved."
      : "The game published a strong fault record but exited with code 0. Dump deletion "
        "failed, so the file remains available and the observed state is preserved.";
  } else if (dumpState == CleanExitDumpState::kNotCaptured) {
    j["note"] =
      "The strong fault was published after the helper's final live-process event poll. "
      "The process had already exited, so only immutable metadata could be preserved.";
  } else {
    j["note"] = context == L"late_recovery"
      ? "The game published a strong fault record but kept running. The dump was "
        "discarded and derived crash actions were suppressed."
      : "The game published a strong fault record but exited with code 0. The dump was "
        "successfully discarded and derived crash actions were suppressed.";
  }

  if (!WriteTextFileUtf8(recordPath, DumpJsonForFile(j))) {
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

bool HasNewerCrashRecord(
  const skydiag::SharedHeader* shm,
  std::uint32_t expectedCrashSeq) noexcept
{
  const auto current = ReadCrashSequence(shm);
  return current != 0u && current != expectedCrashSeq;
}

}  // namespace

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

DWORD WaitForCrashUnhandledOrProcess(
  HANDLE crashEvent,
  HANDLE unhandledEvent,
  HANDLE process,
  DWORD waitMs) noexcept
{
  if (crashEvent && process && unhandledEvent) {
    const HANDLE handles[] = { crashEvent, process, unhandledEvent };
    return WaitForMultipleObjects(static_cast<DWORD>(std::size(handles)), handles, FALSE, waitMs);
  }
  return WaitForCrashOrProcess(crashEvent, process, waitMs);
}

bool IsUnhandledExceptionSignaled(HANDLE unhandledEvent) noexcept
{
  return unhandledEvent && WaitForSingleObject(unhandledEvent, 0) == WAIT_OBJECT_0;
}

namespace {

// ADR-0032: the plugin said this exception reached the top-level filter, so
// nothing in the game handled it. Logged once per incident.
void NoteUnhandledSignal(
  const std::filesystem::path& outBase,
  const CrashEventInfo& info,
  CrashCaptureState* crashState)
{
  if (crashState) {
    if (crashState->unhandledSignalCrashSeq == info.crashSeq) {
      return;
    }
    crashState->unhandledSignalCrashSeq = info.crashSeq;
  }
  AppendLogLine(
    outBase,
    L"The game did not handle this exception: it reached the process's top-level exception filter "
    L"(crash_seq=" + std::to_wstring(info.crashSeq) + L"). It is a crash; not waiting for the heartbeat.");
}

}  // namespace

FilterVerdict ClassifyExitCodeVerdictWithContext(
  const skydiag::helper::HelperConfig& cfg,
  std::uint32_t exitCode,
  HANDLE unhandledEvent,
  const CrashEventInfo& info,
  const std::filesystem::path& outBase,
  std::wstring_view context,
  int checkIndex,
  CrashCaptureState* crashState)
{
  // ADR-0032: before the exit code, here for every caller.
  if (IsUnhandledExceptionSignaled(unhandledEvent)) {
    NoteUnhandledSignal(outBase, info, crashState);
    return FilterVerdict::kKeepDump;
  }
  const auto verdict = ClassifyExitCodeVerdict(exitCode, info, outBase);
  const std::wstring checkSuffix = (checkIndex >= 0)
    ? (L", check=" + std::to_wstring(checkIndex + 1))
    : L"";

  // The filters only give the verdict; the caller logs what happens to the
  // dump, since the early check runs before any dump is written.
  if (verdict == FilterVerdict::kDeleteBenign) {
    AppendLogLine(
      outBase,
      L"Crash event received but process exited normally (context=" + std::wstring(context)
        + L", exit_code=0"
        + checkSuffix
        + L"); treating it as a handled first-chance or shutdown exception."
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
        + L"); treating it as a crash and preserving crash auto-actions.");
  }

  return verdict;
}

namespace {

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
        + L"); treating the exit code as unknown, not as a normal exit.");
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
  HANDLE unhandledEvent,
  const skydiag::SharedHeader* shm,
  const CrashEventInfo& info,
  const std::filesystem::path& outBase,
  CrashCaptureState* crashState)
{
  const DWORD pw = WaitForCrashUnhandledOrProcess(crashEvent, unhandledEvent, process, kShutdownWaitMs);
  if (pw == WAIT_OBJECT_0 && crashEvent) {
    if (HasNewerCrashRecord(shm, info.crashSeq)) {
      AppendLogLine(outBase, L"A newer crash record arrived during shutdown filtering; prioritizing the newer CTD.");
      return FilterVerdict::kRetryNewerCrash;
    }
  }
  // ADR-0032: before the exit code is looked at. An unhandled exception that
  // some filter ended with exit code 0 is still a crash.
  if (IsUnhandledExceptionSignaled(unhandledEvent)) {
    NoteUnhandledSignal(outBase, info, crashState);
    return FilterVerdict::kKeepDump;
  }
  if (pw == WAIT_OBJECT_0 + 1u) {
    DWORD exitCode = STILL_ACTIVE;
    if (!TryReadExitCode(process, outBase, &exitCode)) {
      return FilterVerdict::kKeepDump;
    }
    return ClassifyExitCodeVerdictWithContext(
      cfg, exitCode, unhandledEvent, info, outBase, L"shutdown", -1, crashState);
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
  HANDLE unhandledEvent,
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
    const DWORD waitResult =
      WaitForCrashUnhandledOrProcess(crashEvent, unhandledEvent, process, kHeartbeatCheckIntervalMs);
    if (waitResult == WAIT_OBJECT_0 && crashEvent) {
      if (HasNewerCrashRecord(shm, info.crashSeq)) {
        AppendLogLine(
          outBase,
          L"A newer crash record arrived during first-chance recovery filtering; prioritizing the newer CTD.");
        return FilterVerdict::kRetryNewerCrash;
      }
    }
    // ADR-0032: the plugin knows what the heartbeat can only suggest. Before
    // the exit code is looked at, as in FilterShutdownException.
    if (IsUnhandledExceptionSignaled(unhandledEvent)) {
      NoteUnhandledSignal(outBase, info, crashState);
      return FilterVerdict::kKeepDump;
    }
    if (waitResult == WAIT_OBJECT_0 + 1u) {
      DWORD exitCode = STILL_ACTIVE;
      if (!TryReadExitCode(process, outBase, &exitCode)) {
        return FilterVerdict::kKeepDump;
      }
      return ClassifyExitCodeVerdictWithContext(
        cfg, exitCode, unhandledEvent, info, outBase, L"heartbeat_check", attempt, crashState);
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
            + L"); likely a handled first-chance exception."
        );
        return FilterVerdict::kDeleteRecovered;
      }
    }
    // The checks left cannot reach the required advances: keep the dump now
    // rather than wait them out (a stalled main thread, a real crash).
    if (heartbeatAdvanceCount + (kMaxHeartbeatChecks - attempt - 1) < kRequiredHeartbeatAdvances) {
      break;
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
  const skydiag::ForcedCloseMatch& forcedClose,
  const skydiag::helper::DumpProfile& writtenProfile,
  const PluginScanInputs& pluginScanInputs,
  PendingCrashEtwCapture* pendingCrashEtw,
  PendingCrashAnalysis* pendingCrashAnalysis,
  std::wstring* pendingCrashViewerDumpPath)
{
  const auto etwPath = outBase / (L"SkyrimDiag_Crash_" + ts + L".etl");
  const auto manifestPath = outBase / (L"SkyrimDiag_Incident_Crash_" + ts + L".json");
  const std::filesystem::path dumpFs(dumpPath);

  std::wcout << L"[SkyrimDiagHelper] Crash dump written: " << dumpPath << L"\n";

  {
    const std::string pluginScanJson = CollectPluginScanJson(pluginScanInputs, outBase);
    if (!pluginScanJson.empty()) {
      const auto pluginScanPath = dumpFs.parent_path() / (dumpFs.stem().wstring() + L"_PluginScan.json");
      WriteTextFileUtf8(pluginScanPath, pluginScanJson);
      AppendLogLine(outBase, L"Plugin scan sidecar written: " + pluginScanPath.wstring());
    }
  }
  // A driver reset moments before the crash can be why it crashed (ADR-0029).
  WriteGpuDriverEventsSidecar(outBase, dumpFs, proc.pid);

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
    if (forcedClose.matched()) {
      // ADR-0022: an error of a game being closed. Retention keeps few of
      // these (IsForcedCloseCrashManifest), apart from the real crashes.
      ctx["forced_close"] = true;
    }
    if (IsUnhandledExceptionSignaled(proc.unhandledEvent)) {
      // ADR-0032: the plugin saw the exception reach the top-level filter.
      ctx["unhandled_exception_signal"] = true;
    }
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
      &writtenProfile,
      /*recaptureDecision=*/nullptr,
      cfg,
      cfg.incidentManifestIncludeConfigSnapshot);
    WriteTextFileUtf8(manifestPath, DumpJsonForFile(manifest));
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
  if (cfg.autoOpenViewerOnCrash && forcedClose.matched()) {
    // ADR-0022: the player closed the game; this is no CTD to pop up for.
    // The headless analysis below still writes the report and summary.
    AppendLogLine(outBase, L"Skipped viewer auto-open: " + DescribeForcedCloseForLog(forcedClose) + L".");
  } else if (cfg.autoOpenViewerOnCrash) {
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
        // ADR-0032: this dump was just kept as a crash (it is latched right
        // after this, and its late recovery has not begun); an exit with code
        // 0 undoes that only when the plugin does not report it as not handled.
        const bool crashOutlivesZeroExit = DoesCapturedCrashOutliveZeroExit(
          /*crashLatched=*/true, IsUnhandledExceptionSignaled(proc.unhandledEvent), /*lateRecoveryWithheld=*/false);
        if (IsNormalExitForCapturedCrash(processExitCode, crashOutlivesZeroExit)) {
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

void DiscardReleasedCrashDump(
  const std::filesystem::path& outBase,
  CrashCaptureState* crashState,
  std::wstring_view reason)
{
  if (!crashState || crashState->releasedDumpPath.empty()) {
    return;
  }
  const std::filesystem::path dumpPath(crashState->releasedDumpPath);
  const std::wstring filename = dumpPath.filename().wstring();
  if (crashState->releasedDumpPreserve) {
    AppendLogLine(
      outBase,
      L"Released crash dump kept (PreserveFilteredCrashDumps=1); " + std::wstring(reason) + L": " + filename);
  } else {
    const auto removal = RemoveFileAndObserve(dumpPath);
    AppendLogLine(
      outBase,
      (removal.existsAfter ? L"Released crash dump could not be removed; " : L"Released crash dump deleted; ")
        + std::wstring(reason) + L": " + filename);
  }
  crashState->releasedDumpPath.clear();
  crashState->releasedDumpExceptionCode = 0;
  crashState->releasedDumpExceptionAddr = 0;
  crashState->releasedDumpPreserve = false;
  crashState->releasedDumpRecovery = LateRecoveryState{};
  crashState->releasedDumpContext.reset();
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

bool ThawCrashCapture(
  const skydiag::helper::AttachedProcess& proc,
  std::uint32_t expectedCrashSeq) noexcept
{
  if (!proc.shmWritable) {
    return false;
  }
  // Only for the incident being let go: when a newer record is already there,
  // TryClearRecoveredCrashFreeze refuses and its signals must stay.
  if (ReadCrashSequence(&proc.shmWritable->header) == expectedCrashSeq) {
    if (proc.unhandledEvent) {
      ResetEvent(proc.unhandledEvent);
    }
    if (proc.dumpSettledEvent) {
      ResetEvent(proc.dumpSettledEvent);
    }
  }
  return TryClearRecoveredCrashFreeze(proc.shmWritable, expectedCrashSeq);
}

skydiag::ForcedCloseMatch FindForcedCloseInSnapshot(
  const skydiag::SharedLayout* snapshot,
  std::size_t snapshotBytes) noexcept
{
  if (!snapshot || snapshotBytes < sizeof(skydiag::SharedLayout) || snapshot->header.magic != skydiag::kMagic ||
      snapshot->header.qpc_freq == 0u) {
    return {};
  }
  const auto& header = snapshot->header;
  std::uint32_t capacity = header.capacity;
  if (capacity == 0u || capacity > skydiag::kEventCapacity) {
    capacity = skydiag::kEventCapacity;
  }
  const auto toMs = [&](std::uint64_t qpc) {
    const std::uint64_t since = qpc > header.start_qpc ? qpc - header.start_qpc : 0u;
    return 1000.0 * static_cast<double>(since) / static_cast<double>(header.qpc_freq);
  };
  skydiag::ForcedCloseScanner scanner;
  const std::uint32_t writeIndex = header.write_index;
  const std::uint32_t begin = writeIndex > capacity ? writeIndex - capacity : 0u;
  for (std::uint32_t i = begin; i < writeIndex; ++i) {
    const auto& event = snapshot->events[i % capacity];
    // The snapshot marks an entry it could not copy stably with an odd seq.
    if ((event.seq & 1u) != 0u) {
      continue;
    }
    if (event.type == static_cast<std::uint16_t>(skydiag::EventType::kCloseRequest)) {
      scanner.AddCloseRequest(toMs(event.qpc), event.payload.a);
    } else if (event.type == static_cast<std::uint16_t>(skydiag::EventType::kCrash)) {
      scanner.AddCrash(toMs(event.qpc));
    }
  }
  return scanner.Result();
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
  DWORD exitCode,
  PostExitEvidenceReason reason)
{
  const auto ts = Timestamp();
  nlohmann::json j = nlohmann::json::object();
  j["schema"] = "skydiag.post_exit_crash_evidence.v1";
  switch (reason) {
    case PostExitEvidenceReason::kPublishedAfterFinalLivePoll:
      j["reason"] = "committed_fault_published_after_final_live_process_poll";
      break;
    case PostExitEvidenceReason::kDumpWriteFailed:
      j["reason"] = "committed_fault_dump_write_failed";
      break;
    case PostExitEvidenceReason::kDumpWriteFailedProcessExited:
      j["reason"] = "committed_fault_dump_write_failed_process_exited";
      break;
    case PostExitEvidenceReason::kReleasedFaultFollowedByAbnormalExit:
      j["reason"] = "released_fault_followed_by_abnormal_exit";
      break;
    case PostExitEvidenceReason::kReleasedFaultReportedUnhandled:
      j["reason"] = "released_fault_reported_unhandled";
      break;
  }
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
  if (!WriteTextFileUtf8(path, DumpJsonForFile(j))) {
    AppendLogLine(
      outBase,
      L"Failed to write post-exit crash evidence metadata: " + path.wstring());
    return false;
  }
  std::wstring logLine;
  switch (reason) {
    case PostExitEvidenceReason::kPublishedAfterFinalLivePoll:
      logLine =
        L"Captured committed crash metadata after process exit; no live address space "
        L"remained for dump capture: ";
      break;
    case PostExitEvidenceReason::kDumpWriteFailed:
    case PostExitEvidenceReason::kDumpWriteFailedProcessExited:
      logLine = L"Recorded crash metadata after process exit for a crash whose dump could not be written: ";
      break;
    case PostExitEvidenceReason::kReleasedFaultFollowedByAbnormalExit:
      logLine =
        L"Recorded crash metadata after process exit for an exception released as handled before the "
        L"game exited abnormally: ";
      break;
    case PostExitEvidenceReason::kReleasedFaultReportedUnhandled:
      logLine =
        L"Recorded crash metadata after process exit for an exception released as handled that the "
        L"plugin then reported as not handled: ";
      break;
  }
  AppendLogLine(outBase, logLine + path.wstring());
  return true;
}

namespace {
std::atomic<std::uint64_t> g_lastCrashLoggerHotkeyTick64{ 0 };

enum class ThreadDumpWindowOutcome
{
  kWriteDump,
  kExitedBeforeDump,
  kReleased,
};

  // Right after Crash Logger's thread dump hotkey, a C++ exception or
  // breakpoint is most likely one of its thread dump's probes
  // (IsLikelyCrashLoggerThreadDumpFault): check that the game keeps running
  // before writing a dump, not after. Not for one on the game's main thread:
  // that is a crash Crash Logger is reporting, and it may end the game before
  // the check is done.
ThreadDumpWindowOutcome CheckCrashLoggerThreadDumpWindow(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  const CrashEventInfo& info,
  const skydiag::SharedLayout* dumpSnapshot,
  std::size_t dumpSnapshotBytes,
  const skydiag::ForcedCloseMatch& forcedClose,
  CrashCaptureState* crashState)
{
  const std::uint64_t nowTick64 = GetTickCount64();
  const std::uint64_t lastCrashLoggerHotkeyTick64 = g_lastCrashLoggerHotkeyTick64.load(std::memory_order_relaxed);
  const bool inThreadDumpWindow = proc.process && proc.shm &&
    IsInCrashLoggerThreadDumpWindow(info.exceptionCode, nowTick64, lastCrashLoggerHotkeyTick64);
  const std::uint32_t mainThreadId = inThreadDumpWindow
    ? skydiag::helper::InferMainThreadIdFromSnapshot(dumpSnapshot, dumpSnapshotBytes).value_or(0u)
    : 0u;
  if (inThreadDumpWindow &&
      !IsLikelyCrashLoggerThreadDumpFault(
        info.exceptionCode, nowTick64, lastCrashLoggerHotkeyTick64, info.faultingTid, mainThreadId)) {
    AppendLogLine(
      outBase,
      L"This exception came within a minute of Crash Logger's thread dump hotkey, but on the game's main thread "
      L"(tid=" + std::to_wstring(mainThreadId)
        + L"), so it is not one of its thread dump's probes: writing the crash dump now.");
  } else if (inThreadDumpWindow) {
    FILETIME eventTime{};
    GetSystemTimeAsFileTime(&eventTime);
    const auto early = FilterFirstChanceException(
      cfg, proc.process, proc.crashEvent, proc.unhandledEvent, &proc.shm->header, info, outBase, crashState);
    if (early == FilterVerdict::kDeleteRecovered || early == FilterVerdict::kDeleteBenign) {
      const bool thawed = early == FilterVerdict::kDeleteRecovered && proc.shmWritable &&
        ThawCrashCapture(proc, info.crashSeq);
      if (crashState) {
        crashState->latched = false;
        crashState->releasedCrashSeq = info.crashSeq;
        crashState->releasedCrashTick64 = GetTickCount64();
        // As weak as the release after a dump (ShouldReportReleasedCrashAtExit),
        // and a non-zero exit also needs Crash Logger's crash log to record it.
        crashState->releasedOnBriefHeartbeat = early == FilterVerdict::kDeleteRecovered;
        crashState->releasedInThreadDumpWindow = true;
        crashState->releasedForcedClose = forcedClose;
        crashState->releasedCrashEventFileTimeUtc =
          (static_cast<std::uint64_t>(eventTime.dwHighDateTime) << 32) | eventTime.dwLowDateTime;
      }
      AppendLogLine(
        outBase,
        std::wstring(L"No dump written: this exception came within a minute of Crash Logger's thread dump hotkey, "
                     L"whose thread dump probes each thread, and the game ")
          + (early == FilterVerdict::kDeleteRecovered ? L"kept running" : L"exited normally")
          + (early == FilterVerdict::kDeleteRecovered && !thawed ? L" (capture not thawed: a newer crash record is present)." : L"."));
      return ThreadDumpWindowOutcome::kReleased;
    }
    // The filter keeps the dump when the game exited with a non-zero or
    // unreadable exit code. Its address space is gone then: MiniDumpWriteDump
    // can only fail and leave an empty file.
    const bool exitedBeforeDump = WaitForSingleObject(proc.process, 0) == WAIT_OBJECT_0;
    AppendLogLine(
      outBase,
      exitedBeforeDump
        ? L"The game exited after this exception, before a crash dump could be written."
        : L"The game did not keep running after this exception; writing the crash dump.");
    return exitedBeforeDump ? ThreadDumpWindowOutcome::kExitedBeforeDump : ThreadDumpWindowOutcome::kWriteDump;
  }
  return ThreadDumpWindowOutcome::kWriteDump;
}

enum class RepeatedRecoveredFaultOutcome
{
  kNotApplicable,
  kWriteDumpAfterCheck,
  kExitedBeforeDump,
  kReleased,
  kSuperseded,
};

// ADR-0031: once the same exception code and address was already released as
// handled at least kRepeatedRecoveredFaultMinCount times in this session (main
// thread included), check whether the game recovers before calling
// MiniDumpWriteDump, which suspends every thread while writing.
RepeatedRecoveredFaultOutcome CheckRepeatedRecoveredFault(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  const CrashEventInfo& info,
  const skydiag::ForcedCloseMatch& forcedClose,
  CrashCaptureState* crashState)
{
  if (!crashState || !proc.process || !proc.shm ||
      !IsRepeatedRecoveredFault(crashState->recoveredFaults, info.exceptionCode, info.exceptionAddr)) {
    return RepeatedRecoveredFaultOutcome::kNotApplicable;
  }
  AppendLogLine(
    outBase,
    L"This exception (same code and address) was already released as handled at least "
      + std::to_wstring(kRepeatedRecoveredFaultMinCount)
      + L" times in this session; checking whether the game recovers before writing a crash dump.");
  const auto early = FilterFirstChanceException(
    cfg, proc.process, proc.crashEvent, proc.unhandledEvent, &proc.shm->header, info, outBase, crashState);
  if (early == FilterVerdict::kDeleteRecovered || early == FilterVerdict::kDeleteBenign) {
    const bool thawed = early == FilterVerdict::kDeleteRecovered && proc.shmWritable &&
      ThawCrashCapture(proc, info.crashSeq);
    crashState->latched = false;
    crashState->releasedCrashSeq = info.crashSeq;
    crashState->releasedCrashTick64 = GetTickCount64();
    // Keep ShouldReportReleasedCrashAtExit active for a worker-thread crash
    // whose main loop kept ticking briefly, without requiring Crash Logger's
    // crash log (releasedInThreadDumpWindow stays false).
    crashState->releasedOnBriefHeartbeat = early == FilterVerdict::kDeleteRecovered;
    crashState->releasedInThreadDumpWindow = false;
    crashState->releasedForcedClose = forcedClose;
    if (early == FilterVerdict::kDeleteRecovered) {
      NoteRecoveredFault(&crashState->recoveredFaults, info.exceptionCode, info.exceptionAddr);
    }
    AppendLogLine(
      outBase,
      std::wstring(L"No dump written: this repeated exception was handled again and the game ")
        + (early == FilterVerdict::kDeleteRecovered ? L"kept running" : L"exited normally")
        + (early == FilterVerdict::kDeleteRecovered && !thawed
             ? L" (capture not thawed: a newer crash record is present)."
             : L"."));
    return RepeatedRecoveredFaultOutcome::kReleased;
  }
  if (early == FilterVerdict::kRetryNewerCrash) {
    AppendLogLine(
      outBase,
      L"No dump written for the superseded repeated exception; immediately retrying the newer crash event.");
    return RepeatedRecoveredFaultOutcome::kSuperseded;
  }
  const bool exitedBeforeDump = WaitForSingleObject(proc.process, 0) == WAIT_OBJECT_0;
  const bool unhandledSignal = crashState->unhandledSignalCrashSeq == info.crashSeq;
  AppendLogLine(
    outBase,
    exitedBeforeDump
      ? L"The game exited after this repeated exception, before a crash dump could be written."
      : (unhandledSignal
           ? L"The game did not handle this repeated exception; writing the crash dump now."
           : L"The game did not recover from this repeated exception; writing the crash dump now."));
  return exitedBeforeDump
    ? RepeatedRecoveredFaultOutcome::kExitedBeforeDump
    : RepeatedRecoveredFaultOutcome::kWriteDumpAfterCheck;
}

  // The crash event was already consumed and reset, and the game has committed
  // its record and will not signal again for this fault. Nothing would drive a
  // later attempt, so a transient failure (sharing violation, momentary disk
  // pressure) has to be retried here or the incident is lost outright.
bool WriteCrashDumpWithRetries(
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  const std::wstring& dumpPath,
  const skydiag::SharedLayout* dumpSnapshot,
  std::size_t dumpSnapshotBytes,
  const skydiag::helper::DumpProfile& dumpProfile,
  bool exitedBeforeDump,
  std::wstring* dumpErrOut,
  skydiag::helper::DumpObjectMemoryStats* dumpMemoryOut)
{
  std::wstring dumpErr = exitedBeforeDump ? L"the game process had already exited" : L"";
  bool dumpOk = false;
  skydiag::helper::DumpObjectMemoryStats dumpMemory{};
  for (int attempt = 0; attempt < kDumpWriteAttempts && !exitedBeforeDump; ++attempt) {
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
      &dumpErr,
      &dumpMemory);
    if (dumpOk) {
      if (attempt > 0) {
        AppendLogLine(
          outBase,
          L"Crash dump succeeded on attempt " + std::to_wstring(attempt + 1) + L".");
      }
      LogDumpCodeMemory(outBase, dumpMemory.code);
      break;
    }

    AppendLogLine(
      outBase,
      L"Crash dump attempt " + std::to_wstring(attempt + 1) + L" failed: " + dumpErr);
  }
  *dumpErrOut = dumpErr;
  *dumpMemoryOut = dumpMemory;
  return dumpOk;
}

// No dump could be written for this crash event: the WER hint, and a thaw so
// a later CTD can still be recorded.
void HandleCrashDumpWriteFailure(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  const std::wstring& dumpPath,
  const std::wstring& dumpErr,
  const CrashEventInfo& info,
  CrashCaptureState* crashState,
  std::wstring* lastCrashDumpPath)
{
  AppendLogLine(outBase, L"Crash dump failed: " + dumpErr);
  std::wcerr << L"[SkyrimDiagHelper] Crash dump failed: " << dumpErr << L"\n";
  if (cfg.enableWerDumpFallbackHint) {
    WriteWerFallbackHint(outBase, WerFallbackReason::kDumpWriteFailed, 0);
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
    const bool thawed = ThawCrashCapture(proc, info.crashSeq);
    AppendLogLine(
      outBase,
      thawed
        ? L"Crash dump could not be written; thawed capture so a later CTD can still be recorded."
        : L"Crash dump could not be written; capture was not thawed because a newer crash record "
          L"is already present.");
  }
  if (crashState) {
    crashState->latched = false;
    crashState->dumpWriteFailedCrashSeq = info.crashSeq;
    crashState->dumpWriteFailedProcessExited =
      proc.process && WaitForSingleObject(proc.process, 0) == WAIT_OBJECT_0;
    crashState->dumpWriteFailedTick64 = GetTickCount64();
  }
}

// A crash event the shutdown or first-chance filter released: thaw the
// capture, keep, hold or delete the dump, and record the release.
void ReleaseFilteredCrashDump(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  const std::wstring& dumpPath,
  const CrashEventInfo& info,
  const skydiag::ForcedCloseMatch& forcedClose,
  FilterVerdict verdict,
  const PluginScanInputs& pluginScanInputs,
  std::wstring_view ts,
  const skydiag::helper::DumpProfile& writtenProfile,
  CrashCaptureState* crashState,
  std::wstring* lastCrashDumpPath)
{
  if (verdict == FilterVerdict::kDeleteRecovered) {
    const bool thawed = ThawCrashCapture(proc, info.crashSeq);
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
      // Without a durable pending record, keep the dump as the fail-safe.
      preserveFilteredDump =
        ShouldPreserveFilteredDump(cfg.preserveFilteredCrashDumps, evidenceRequired, pendingWritten);
      if (!preserveFilteredDump) {
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
  // A release on a few seconds of heartbeat may still be followed by the
  // crash it was (ShouldReportReleasedCrashAtExit), so its dump is held
  // until that is settled (CrashCaptureState::releasedDumpPath).
  const bool holdReleasedDump = verdict == FilterVerdict::kDeleteRecovered && crashState;
  std::wstring heldDumpPath = dumpPath;
  if (preserveFilteredDump) {
    AppendLogLine(
      outBase,
      cfg.preserveFilteredCrashDumps
        ? L"Filtered dump file preserved without crash post-processing or capture latch "
          L"(PreserveFilteredCrashDumps=1)."
        : L"Clean-exit evidence metadata write failed; preserved the filtered dump as a fail-safe "
          L"without crash post-processing or capture latch.");
  } else if (holdReleasedDump) {
    // Held under its own name (ReleasedDumpPathFor): a helper that stops
    // before the release is settled leaves no dump that reads as a crash.
    const std::wstring releasedPath = ReleasedDumpPathFor(dumpPath);
    if (MoveFileExW(dumpPath.c_str(), releasedPath.c_str(), 0)) {
      heldDumpPath = releasedPath;
    } else {
      AppendLogLine(
        outBase,
        L"Could not rename the released dump while it is held (err=" + std::to_wstring(GetLastError())
          + L"); keeping its crash name.");
    }
    AppendLogLine(
      outBase,
      L"Keeping the released dump until the game exits or outlives this exception: "
        + std::filesystem::path(heldDumpPath).filename().wstring());
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
    if (verdict != FilterVerdict::kRetryNewerCrash) {
      crashState->releasedCrashSeq = info.crashSeq;
      crashState->releasedCrashTick64 = GetTickCount64();
      crashState->releasedOnBriefHeartbeat = verdict == FilterVerdict::kDeleteRecovered;
      crashState->releasedInThreadDumpWindow = false;
      crashState->releasedForcedClose = forcedClose;
    }
    if (holdReleasedDump) {
      NoteRecoveredFault(&crashState->recoveredFaults, info.exceptionCode, info.exceptionAddr);
      crashState->releasedDumpPath = heldDumpPath;
      crashState->releasedDumpExceptionCode = info.exceptionCode;
      crashState->releasedDumpExceptionAddr = info.exceptionAddr;
      crashState->releasedDumpPreserve = preserveFilteredDump;
      // For the exit that reports this fault after all (PromoteReleasedCrashDump).
      crashState->releasedDumpContext = std::make_shared<const ReleasedDumpContext>(ReleasedDumpContext{
        pluginScanInputs,
        std::wstring(ts),
        proc.pid,
        info.stateFlags,
        writtenProfile,
      });
      StartLateRecovery(
        &crashState->releasedDumpRecovery,
        GetTickCount64(),
        proc.shm ? proc.shm->header.last_heartbeat_qpc : 0u);
    }
  }
  AppendLogLine(
    outBase,
    (preserveFilteredDump || holdReleasedDump)
      ? L"Filtered crash event consumed; dump kept, capture remains ready for a later CTD."
      : L"Filtered crash event consumed; dump deleted, capture remains ready for a later CTD.");
}

}  // namespace

void NoteCrashLoggerThreadDumpHotkeyPressed(std::uint64_t tick64) noexcept
{
  g_lastCrashLoggerHotkeyTick64.store(tick64, std::memory_order_relaxed);
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
    // An earlier released exception's held dump is not needed once this
    // newer crash has a dump of its own (the exit path only ever reports the
    // newest committed crash). Until then it is kept: if this dump cannot be
    // written (the game is already gone), it is the only dump of the crash.
  }
  AppendLogLine(
    outBase,
    L"Crash event signaled (exception_code=" + Hex32(info.exceptionCode)
      + L", exception_addr=" + Hex64(info.exceptionAddr)
      + L", tid=" + std::to_wstring(info.faultingTid)
      + L", crash_seq=" + std::to_wstring(info.crashSeq)
      + L").");
  const auto forcedClose = FindForcedCloseInSnapshot(dumpSnapshot, dumpSnapshotBytes);
  if (crashState) {
    crashState->forcedClose = forcedClose;
  }
  if (forcedClose.matched()) {
    AppendLogLine(
      outBase,
      L"This " + DescribeForcedCloseForLog(forcedClose)
        + L": an error while the game was being closed, not a CTD (the viewer will not open).");
  }

  bool exitedBeforeDump = false;
  bool checkedRecoveryBeforeDump = false;
  switch (CheckCrashLoggerThreadDumpWindow(
    cfg, proc, outBase, info, dumpSnapshot, dumpSnapshotBytes, forcedClose, crashState)) {
    case ThreadDumpWindowOutcome::kReleased:
      // Released without a dump: the exit path would take an older held dump
      // for this exception's.
      DiscardReleasedCrashDump(outBase, crashState, L"a newer crash event arrived");
      return true;
    case ThreadDumpWindowOutcome::kExitedBeforeDump:
      exitedBeforeDump = true;
      break;
    case ThreadDumpWindowOutcome::kWriteDump:
      switch (CheckRepeatedRecoveredFault(cfg, proc, outBase, info, forcedClose, crashState)) {
        case RepeatedRecoveredFaultOutcome::kNotApplicable:
          break;
        case RepeatedRecoveredFaultOutcome::kReleased:
          DiscardReleasedCrashDump(outBase, crashState, L"a newer crash event arrived");
          return true;
        case RepeatedRecoveredFaultOutcome::kSuperseded:
          return true;
        case RepeatedRecoveredFaultOutcome::kExitedBeforeDump:
          exitedBeforeDump = true;
          checkedRecoveryBeforeDump = true;
          break;
        case RepeatedRecoveredFaultOutcome::kWriteDumpAfterCheck:
          checkedRecoveryBeforeDump = true;
          break;
      }
      break;
  }

  const auto ts = Timestamp();
  const auto dumpPath = (outBase / (L"SkyrimDiag_Crash_" + ts + L".dmp")).wstring();
  const auto dumpProfile = skydiag::helper::ResolveDumpProfile(
    cfg.dumpMode,
    skydiag::helper::CaptureKind::Crash,
    cfg.dumpAllModuleCode);
  std::wstring dumpErr;
  skydiag::helper::DumpObjectMemoryStats dumpMemory{};
  const bool dumpOk = WriteCrashDumpWithRetries(
    proc, outBase, dumpPath, dumpSnapshot, dumpSnapshotBytes, dumpProfile, exitedBeforeDump, &dumpErr, &dumpMemory);
  // ADR-0032: written or given up, the dump is settled. The plugin holds the
  // crashing thread in the top-level filter until this, so that the game does
  // not end while the dump is still to be written.
  if (proc.dumpSettledEvent) {
    SetEvent(proc.dumpSettledEvent);
  }

  if (!dumpOk) {
    if (checkedRecoveryBeforeDump && crashState && !crashState->releasedDumpPath.empty() &&
        crashState->releasedDumpExceptionCode == info.exceptionCode &&
        crashState->releasedDumpExceptionAddr == info.exceptionAddr) {
      // An earlier held dump of this same repeated fault came from a handled
      // probe, not a dying game; do not promote it as this crash's dump.
      DiscardReleasedCrashDump(
        outBase,
        crashState,
        L"this repeated handled exception was not recovered and had no live process to dump");
    }
    HandleCrashDumpWriteFailure(cfg, proc, outBase, dumpPath, dumpErr, info, crashState, lastCrashDumpPath);
    if (crashState && !crashState->releasedDumpPath.empty()) {
      AppendLogLine(
        outBase,
        L"Keeping the dump held from the exception released before this one: it may be the only dump of this "
        L"crash (" + std::filesystem::path(crashState->releasedDumpPath).filename().wstring() + L").");
    }
    return true;
  }
  DiscardReleasedCrashDump(outBase, crashState, L"a newer crash's dump was written");

  if (lastCrashDumpPath) {
    *lastCrashDumpPath = dumpPath;
  }

  // Read these while the game is still alive: the filters below wait for it to
  // exit, after which its exe path and module list can no longer be queried.
  const PluginScanInputs pluginScanInputs = CollectPluginScanInputs(proc);

  auto verdict = FilterVerdict::kKeepDump;
  if (proc.process) {
    if (checkedRecoveryBeforeDump) {
      if (WaitForSingleObject(proc.process, 0) == WAIT_OBJECT_0) {
        DWORD exitCode = STILL_ACTIVE;
        if (TryReadExitCode(proc.process, outBase, &exitCode)) {
          verdict = ClassifyExitCodeVerdictWithContext(
            cfg, exitCode, proc.unhandledEvent, info, outBase, L"post_dump_exit_check", -1, crashState);
        }
      }
    } else {
      verdict = FilterShutdownException(
        cfg,
        proc.process,
        proc.crashEvent,
        proc.unhandledEvent,
        proc.shm ? &proc.shm->header : nullptr,
        info,
        outBase,
        crashState);
      if (verdict == FilterVerdict::kKeepDump && proc.shm) {
        verdict = FilterFirstChanceException(
          cfg, proc.process, proc.crashEvent, proc.unhandledEvent, &proc.shm->header, info, outBase, crashState);
      }
    }
    if (verdict != FilterVerdict::kKeepDump) {
      ReleaseFilteredCrashDump(
        cfg,
        proc,
        outBase,
        dumpPath,
        info,
        forcedClose,
        verdict,
        pluginScanInputs,
        ts,
        WrittenDumpProfile(dumpProfile, dumpMemory),
        crashState,
        lastCrashDumpPath);
      return true;
    }
  }
  // A kept crash takes the exit's viewer from an earlier hang capture. One
  // released above as a handled exception does not: the hang's viewer was
  // dropped for it before, and then no viewer opened at the exit at all.
  if (pendingHangViewerDumpPath) {
    pendingHangViewerDumpPath->clear();
  }

  // The profile the kept dump was written with, for its manifest (ADR-0023).
  const auto writtenProfile = WrittenDumpProfile(dumpProfile, dumpMemory);

  ProcessValidCrashDump(
    cfg,
    proc,
    outBase,
    dumpPath,
    ts,
    info,
    forcedClose,
    writtenProfile,
    pluginScanInputs,
    pendingCrashEtw,
    pendingCrashAnalysis,
    pendingCrashViewerDumpPath);

  if (crashState) {
    crashState->latched = ShouldLatchCrashCapture(verdict);
    if (crashState->latched && proc.shm) {
      crashState->latchedHeartbeatQpc = proc.shm->header.last_heartbeat_qpc;
      StartLateRecovery(&crashState->lateRecovery, GetTickCount64(), proc.shm->header.last_heartbeat_qpc);
    }
  }
  AppendLogLine(outBase, L"Crash captured; waiting for process exit.");
  return true;
}

}
