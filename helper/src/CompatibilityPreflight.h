#pragma once

#include <filesystem>

#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/ProcessAttach.h"

namespace skydiag::helper::internal {

void RunCompatibilityPreflight(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase);

}  // namespace skydiag::helper::internal

