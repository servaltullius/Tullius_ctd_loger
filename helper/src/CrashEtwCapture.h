#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace skydiag::helper {
struct AttachedProcess;
struct HelperConfig;
}

namespace skydiag::helper::internal {

struct PendingCrashEtwCapture
{
  bool active = false;
  std::filesystem::path etwPath;
  std::filesystem::path manifestPath;
  ULONGLONG startedAtTick64 = 0;
  std::uint32_t captureSeconds = 0;
  std::wstring profileUsed;
  bool cleanupPending = false;
  std::uint32_t stopAttempts = 0;
  ULONGLONG nextCleanupAttemptTick64 = 0;
};

void MaybeStopPendingCrashEtwCapture(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  bool force,
  PendingCrashEtwCapture* pending);

}  // namespace skydiag::helper::internal
