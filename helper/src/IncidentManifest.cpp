#include "IncidentManifest.h"

#include <Windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "HelperCommon.h"
#include "SkyrimDiagHelper/Config.h"

namespace skydiag::helper::internal {
namespace {

std::string DumpModeToString(skydiag::helper::DumpMode mode)
{
  switch (mode) {
    case skydiag::helper::DumpMode::kMini:
      return "mini";
    case skydiag::helper::DumpMode::kDefault:
      return "default";
    case skydiag::helper::DumpMode::kFull:
      return "full";
  }
  return "default";
}

nlohmann::json MakeIncidentConfigSnapshotSafe(const skydiag::helper::HelperConfig& cfg)
{
  // Keep this snapshot privacy-safe: avoid absolute paths (OutputDir, DumpToolExe, EtwWprExe).
  nlohmann::json j = nlohmann::json::object();
  j["dump_mode"] = DumpModeToString(cfg.dumpMode);

  j["hang_threshold_in_game_sec"] = cfg.hangThresholdInGameSec;
  j["hang_threshold_in_menu_sec"] = cfg.hangThresholdInMenuSec;
  j["hang_threshold_loading_sec"] = cfg.hangThresholdLoadingSec;
  j["suppress_hang_when_not_foreground"] = cfg.suppressHangWhenNotForeground;
  j["foreground_grace_sec"] = cfg.foregroundGraceSec;

  j["enable_manual_capture_hotkey"] = cfg.enableManualCaptureHotkey;
  j["auto_analyze_dump"] = cfg.autoAnalyzeDump;
  j["allow_online_symbols"] = cfg.allowOnlineSymbols;

  j["auto_open_viewer_on_crash"] = cfg.autoOpenViewerOnCrash;
  j["auto_open_crash_only_if_process_exited"] = cfg.autoOpenCrashOnlyIfProcessExited;
  j["auto_open_crash_wait_for_exit_ms"] = cfg.autoOpenCrashWaitForExitMs;
  j["enable_auto_recapture_on_unknown_crash"] = cfg.enableAutoRecaptureOnUnknownCrash;
  j["auto_recapture_unknown_bucket_threshold"] = cfg.autoRecaptureUnknownBucketThreshold;
  j["auto_recapture_analysis_timeout_sec"] = cfg.autoRecaptureAnalysisTimeoutSec;

  j["auto_open_viewer_on_hang"] = cfg.autoOpenViewerOnHang;
  j["auto_open_viewer_on_manual_capture"] = cfg.autoOpenViewerOnManualCapture;
  j["auto_open_hang_after_process_exit"] = cfg.autoOpenHangAfterProcessExit;
  j["auto_open_hang_delay_ms"] = cfg.autoOpenHangDelayMs;
  j["auto_open_viewer_beginner_mode"] = cfg.autoOpenViewerBeginnerMode;

  j["enable_adaptive_loading_threshold"] = cfg.enableAdaptiveLoadingThreshold;
  j["adaptive_loading_min_sec"] = cfg.adaptiveLoadingMinSec;
  j["adaptive_loading_min_extra_sec"] = cfg.adaptiveLoadingMinExtraSec;
  j["adaptive_loading_max_sec"] = cfg.adaptiveLoadingMaxSec;

  j["enable_etw_capture_on_hang"] = cfg.enableEtwCaptureOnHang;
  j["etw_hang_profile"] = WideToUtf8(cfg.etwHangProfile);
  j["etw_hang_fallback_profile"] = WideToUtf8(cfg.etwHangFallbackProfile);
  j["etw_max_duration_sec"] = cfg.etwMaxDurationSec;

  j["enable_etw_capture_on_crash"] = cfg.enableEtwCaptureOnCrash;
  j["etw_crash_profile"] = WideToUtf8(cfg.etwCrashProfile);
  j["etw_crash_capture_seconds"] = cfg.etwCrashCaptureSeconds;

  j["enable_incident_manifest"] = cfg.enableIncidentManifest;
  j["incident_manifest_include_config_snapshot"] = cfg.incidentManifestIncludeConfigSnapshot;

  j["retention"] = {
    { "max_crash_dumps", cfg.maxCrashDumps },
    { "max_hang_dumps", cfg.maxHangDumps },
    { "max_manual_dumps", cfg.maxManualDumps },
    { "max_etw_traces", cfg.maxEtwTraces },
  };

  return j;
}

std::string MakeIncidentId(std::string_view captureKind, std::wstring_view ts, DWORD pid)
{
  return std::string(captureKind) + "_" + WideToUtf8(ts) + "_pid" + std::to_string(static_cast<std::uint32_t>(pid));
}

}  // namespace

nlohmann::json MakeIncidentManifestV1(
  std::string_view captureKind,
  std::wstring_view ts,
  DWORD pid,
  const std::filesystem::path& dumpPath,
  const std::optional<std::filesystem::path>& wctPath,
  const std::optional<std::filesystem::path>& etwPath,
  std::string_view etwStatus,
  std::uint32_t stateFlags,
  const nlohmann::json& context,
  const skydiag::helper::HelperConfig& cfg,
  bool includeConfigSnapshot)
{
  nlohmann::json j = nlohmann::json::object();
  j["schema"] = {
    { "name", "SkyrimDiagIncident" },
    { "version", 1 },
  };
  j["incident_id"] = MakeIncidentId(captureKind, ts, pid);
  j["capture_kind"] = std::string(captureKind);
  j["timestamp_local"] = WideToUtf8(ts);
  j["pid"] = static_cast<std::uint32_t>(pid);
  j["state_flags"] = stateFlags;
  j["artifacts"] = nlohmann::json::object();
  j["artifacts"]["dump"] = WideToUtf8(dumpPath.filename().wstring());
  j["artifacts"]["wct"] = wctPath ? WideToUtf8(wctPath->filename().wstring()) : "";
  j["artifacts"]["etw"] = etwPath ? WideToUtf8(etwPath->filename().wstring()) : "";
  j["artifacts"]["etw_status"] = std::string(etwStatus);
  j["artifacts"]["helper_log"] = "SkyrimDiagHelper.log";
  j["privacy"] = {
    { "paths", "filenames_only" },
    { "contains_user_paths", false },
    { "contains_config_paths", false },
  };
  if (context.is_object() && !context.empty()) {
    j["context"] = context;
  }
  if (includeConfigSnapshot) {
    j["config_snapshot"] = MakeIncidentConfigSnapshotSafe(cfg);
  }
  return j;
}

bool TryUpdateIncidentManifestEtw(
  const std::filesystem::path& manifestPath,
  const std::filesystem::path& etwPath,
  std::string_view etwStatus,
  std::wstring* err)
{
  std::string txt;
  if (!ReadTextFileUtf8(manifestPath, &txt)) {
    if (err) {
      *err = L"manifest read failed";
    }
    return false;
  }
  auto j = nlohmann::json::parse(txt, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    if (err) {
      *err = L"manifest parse failed";
    }
    return false;
  }
  if (!j.contains("artifacts") || !j["artifacts"].is_object()) {
    j["artifacts"] = nlohmann::json::object();
  }
  j["artifacts"]["etw"] = WideToUtf8(etwPath.filename().wstring());
  j["artifacts"]["etw_status"] = std::string(etwStatus);
  WriteTextFileUtf8(manifestPath, j.dump(2));
  if (err) {
    err->clear();
  }
  return true;
}

}  // namespace skydiag::helper::internal

