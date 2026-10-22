#pragma once

#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "HelperLog.h"
#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/DumpWriter.h"
#include "SkyrimDiagHelper/PluginScanner.h"
#include "SkyrimDiagHelper/ProcessAttach.h"
#include "SkyrimDiagHelper/Retention.h"
#include "RetentionWorker.h"

namespace skydiag::helper::internal {

inline skydiag::helper::RetentionLimits BuildRetentionLimits(const skydiag::helper::HelperConfig& cfg)
{
  skydiag::helper::RetentionLimits limits{};
  limits.maxCrashDumps = cfg.maxCrashDumps;
  limits.maxHangDumps = cfg.maxHangDumps;
  limits.maxManualDumps = cfg.maxManualDumps;
  limits.maxEtwTraces = cfg.maxEtwTraces;
  return limits;
}

inline void ApplyRetentionFromConfig(const skydiag::helper::HelperConfig& cfg, const std::filesystem::path& outBase)
{
  QueueRetentionSweep(outBase, BuildRetentionLimits(cfg));
}

// One line per freeze or manual dump: what the object memory added to it.
inline void LogDumpObjectMemory(const std::filesystem::path& outBase, const skydiag::helper::DumpObjectMemoryStats& stats)
{
  if (stats.threads_selected == 0u) {
    return;  // not collected (PSS snapshot or full-memory dump)
  }
  AppendLogLine(outBase,
    L"Object memory: threads=" + std::to_wstring(stats.threads_read) + L"/" + std::to_wstring(stats.threads_selected) +
      L" ranges=" + std::to_wstring(stats.ranges) + L" bytes=" + std::to_wstring(stats.bytes) +
      L" time_us=" + std::to_wstring(stats.elapsed_us) + (stats.budget_reached ? L" (budget reached)" : L""));
}

// What the plugin scan needs from the live game process. The exe path and the
// module list can only be read while the process exists, so a crash capture
// collects them right after the dump write: by the time the crash is confirmed
// the game has usually exited.
struct PluginScanInputs
{
  bool gameExeDirResolved = false;
  std::filesystem::path gameExeDir;
  std::vector<std::wstring> moduleNames;
  std::vector<std::wstring> modulePaths;
  // Identifies the session in skse64.log, which keeps no timestamps.
  std::uint64_t gameImageBase = 0;
  // A skse64.log written before this is from an earlier launch.
  std::optional<std::filesystem::file_time_type> gameProcessStart;
};

inline PluginScanInputs CollectPluginScanInputs(const skydiag::helper::AttachedProcess& proc)
{
  PluginScanInputs inputs{};
  inputs.gameExeDirResolved = skydiag::helper::TryResolveGameExeDir(proc.process, inputs.gameExeDir);
  if (inputs.gameExeDirResolved) {
    inputs.moduleNames = skydiag::helper::CollectModuleFilenamesBestEffort(proc.pid);
    inputs.modulePaths = skydiag::helper::CollectModulePathsBestEffort(proc.pid);
    inputs.gameImageBase = skydiag::helper::QueryMainModuleBaseBestEffort(proc.pid);
    inputs.gameProcessStart = skydiag::helper::QueryProcessStartTimeBestEffort(proc.process);
  }
  return inputs;
}

inline constexpr std::wstring_view kPluginScanResolveFailureMessage =
  L"PluginScanner skipped: failed to resolve game exe directory.";

inline std::string CollectPluginScanJson(
  const PluginScanInputs& inputs,
  const std::filesystem::path& outBase,
  std::wstring_view resolveFailureMessage = kPluginScanResolveFailureMessage)
{
  if (!inputs.gameExeDirResolved) {
    AppendLogLine(outBase, resolveFailureMessage);
    return {};
  }

  // The scan is context for the dump, never a reason to lose it: a hang dump
  // is written after this, so an exception here must not escape.
  try {
    auto scanResult = skydiag::helper::ScanPlugins(inputs.gameExeDir, inputs.moduleNames, &inputs.modulePaths);
    if (const auto& mo2 = scanResult.mo2; !mo2.status.empty()) {
      AppendLogLine(outBase,
        L"MO2 capture: " + std::wstring(mo2.status.begin(), mo2.status.end()) +
        L" (enabled mods=" + std::to_wstring(mo2.enabled_mods.size()) + L")");
    }
    // A failure while reading skse64.log keeps the finished plugin scan.
    try {
      scanResult.skse_log = skydiag::helper::CollectSkseLogBestEffort(inputs.gameImageBase, inputs.gameProcessStart);
    } catch (const std::exception& ex) {
      scanResult.skse_log = {};
      scanResult.skse_log.status = "error";
      AppendLogLine(outBase, L"SKSE log scan failed: " + AnsiExceptionText(ex));
    }
    const auto& skseLog = scanResult.skse_log;
    std::wstring skseLine = L"SKSE log: " + std::wstring(skseLog.status.begin(), skseLog.status.end());
    if (skseLog.status == "matched") {
      skseLine += L" (checked=" + std::to_wstring(skseLog.summary.checked_count) +
        L", loaded=" + std::to_wstring(skseLog.summary.loaded_count) +
        L", not_loaded=" + std::to_wstring(skseLog.summary.issues.size()) +
        (skseLog.time_checked ? L", written after the game started)" : L", start time unknown)");
    }
    AppendLogLine(outBase, skseLine);
    return skydiag::helper::SerializePluginScanResult(scanResult);
  } catch (const std::exception& ex) {
    AppendLogLine(outBase, L"PluginScanner failed; capture continues without it: " + AnsiExceptionText(ex));
  } catch (...) {
    AppendLogLine(outBase, L"PluginScanner failed; capture continues without it.");
  }
  return {};
}

inline std::string CollectPluginScanJson(
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  std::wstring_view resolveFailureMessage = kPluginScanResolveFailureMessage)
{
  return CollectPluginScanJson(CollectPluginScanInputs(proc), outBase, resolveFailureMessage);
}

}  // namespace skydiag::helper::internal
