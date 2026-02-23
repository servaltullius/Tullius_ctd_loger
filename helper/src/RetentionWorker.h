#pragma once

#include <filesystem>

#include "SkyrimDiagHelper/Retention.h"

namespace skydiag::helper::internal {

void QueueRetentionSweep(const std::filesystem::path& outBase, const skydiag::helper::RetentionLimits& limits);
void ShutdownRetentionWorker();

}  // namespace skydiag::helper::internal
