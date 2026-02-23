#pragma once

#include <filesystem>
#include <string>
#include <string_view>

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

inline std::string CollectPluginScanJson(
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  std::wstring_view resolveFailureMessage = L"PluginScanner skipped: failed to resolve game exe directory.")
{
  std::filesystem::path gameExeDir;
  if (!skydiag::helper::TryResolveGameExeDir(proc.process, gameExeDir)) {
    AppendLogLine(outBase, resolveFailureMessage);
    return {};
  }

  const auto moduleNames = skydiag::helper::CollectModuleFilenamesBestEffort(proc.pid);
  auto scanResult = skydiag::helper::ScanPlugins(gameExeDir, moduleNames);
  return skydiag::helper::SerializePluginScanResult(scanResult);
}

}  // namespace skydiag::helper::internal
