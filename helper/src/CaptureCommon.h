#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "HelperLog.h"
#include "SkyrimDiagHelper/Config.h"
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
};

inline PluginScanInputs CollectPluginScanInputs(const skydiag::helper::AttachedProcess& proc)
{
  PluginScanInputs inputs{};
  inputs.gameExeDirResolved = skydiag::helper::TryResolveGameExeDir(proc.process, inputs.gameExeDir);
  if (inputs.gameExeDirResolved) {
    inputs.moduleNames = skydiag::helper::CollectModuleFilenamesBestEffort(proc.pid);
    inputs.modulePaths = skydiag::helper::CollectModulePathsBestEffort(proc.pid);
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

  auto scanResult = skydiag::helper::ScanPlugins(inputs.gameExeDir, inputs.moduleNames, &inputs.modulePaths);
  return skydiag::helper::SerializePluginScanResult(scanResult);
}

inline std::string CollectPluginScanJson(
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  std::wstring_view resolveFailureMessage = kPluginScanResolveFailureMessage)
{
  return CollectPluginScanJson(CollectPluginScanInputs(proc), outBase, resolveFailureMessage);
}

}  // namespace skydiag::helper::internal
