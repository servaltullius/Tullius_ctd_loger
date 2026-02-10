#pragma once

#include <filesystem>

namespace skydiag::helper {

// Helper-side resolution for the headless dump analyzer executable.
//
// Preference order:
// 1) CLI next to helper (SkyrimDiagDumpToolCli.exe) if present
// 2) Provided WinUI exe path if present (back-compat)
// 3) Optional override path if present
//
// Returns empty path when nothing exists.
inline std::filesystem::path ResolveDumpToolHeadlessExe(
  const std::filesystem::path& helperDir,
  const std::filesystem::path& winuiExe,
  const std::filesystem::path& overrideExe)
{
  std::error_code ec;

  const auto cli = helperDir / "SkyrimDiagDumpToolCli.exe";
  if (std::filesystem::exists(cli, ec) && std::filesystem::is_regular_file(cli, ec)) {
    return cli;
  }

  if (!winuiExe.empty() && std::filesystem::exists(winuiExe, ec) && std::filesystem::is_regular_file(winuiExe, ec)) {
    return winuiExe;
  }

  if (!overrideExe.empty() && std::filesystem::exists(overrideExe, ec) && std::filesystem::is_regular_file(overrideExe, ec)) {
    return overrideExe;
  }

  return {};
}

}  // namespace skydiag::helper

