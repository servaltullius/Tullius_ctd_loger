#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/LoadStats.h"
#include "SkyrimDiagHelper/ProcessAttach.h"

#include "CrashEtwCapture.h"
#include "HangCapture.h"
#include "PendingCrashAnalysis.h"

namespace skydiag::helper::internal {

inline constexpr int kHotkeyId = 0x5344;  // 'SD' (arbitrary)
inline constexpr ULONGLONG kManualCaptureDebounceMs = 250;

struct HelperLoopState
{
  bool crashCaptured = false;
  HangCaptureState hangState{};
  std::wstring pendingHangViewerDumpPath;
  std::wstring pendingCrashViewerDumpPath;
  std::wstring capturedCrashDumpPath;
  PendingCrashAnalysis pendingCrashAnalysis{};
  PendingCrashEtwCapture pendingCrashEtw{};
  std::uint64_t nextCrashEventRetryTick64 = 0;
  std::uint64_t nextCrashEventWarnTick64 = 0;
};

HANDLE AcquireHelperSingletonMutex(std::uint32_t pid, std::wstring* err);
std::wstring BuildCrashEventUnavailableMessage(const AttachedProcess& proc, std::wstring_view prefix);
std::uint32_t RemoveCrashArtifactsForDump(
  const std::filesystem::path& outBase,
  std::wstring_view dumpPath,
  const std::filesystem::path& extraArtifactPath = {});
void InitializeLoopState(const AttachedProcess& proc, HelperLoopState* state);
void RegisterManualCaptureHotkeyIfEnabled(const HelperConfig& cfg, const std::filesystem::path& outBase);
void UnregisterManualCaptureHotkeyIfEnabled(const HelperConfig& cfg);
void PumpManualCaptureInputs(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase,
  LoadStats* loadStats,
  std::uint32_t adaptiveLoadingThresholdSec);
void DrainCrashEventBeforeExit(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase,
  HelperLoopState* state);
bool HasSharedMemoryStrongCrashEvidence(const AttachedProcess& proc);
void CleanupCrashArtifactsAfterZeroExit(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase,
  bool exitCode0StrongCrash,
  std::uint32_t exceptionCode,
  HelperLoopState* state);
void AppendExitClassificationLog(
  const std::filesystem::path& outBase,
  bool exitCode0StrongCrash,
  std::uint32_t exceptionCode);
void LaunchDeferredViewersAfterExit(
  const HelperConfig& cfg,
  const std::filesystem::path& outBase,
  DWORD exitCode,
  bool exitCode0StrongCrash,
  HelperLoopState* state);
void HandleProcessWaitFailed(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase,
  DWORD waitError,
  HelperLoopState* state);
bool HandleProcessExitTick(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase,
  HelperLoopState* state);
void RunHelperLoop(
  const HelperConfig& cfg,
  AttachedProcess& proc,
  const std::filesystem::path& outBase,
  LoadStats* loadStats,
  const std::filesystem::path& loadStatsPath,
  std::uint32_t* adaptiveLoadingThresholdSec,
  std::uint64_t attachNowQpc,
  HelperLoopState* state);
void ShutdownLoopState(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase,
  HelperLoopState* state);
bool DetectGrassCacheMode(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase);
void RunGrassCacheLoop(const AttachedProcess& proc, const std::filesystem::path& outBase);

}  // namespace skydiag::helper::internal
