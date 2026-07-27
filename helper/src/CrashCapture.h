#pragma once

#ifdef _WIN32
#include <Windows.h>
#else
#include <cstdint>
using DWORD = std::uint32_t;
#endif

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "SkyrimDiagCrashCodes.h"

namespace skydiag {
struct SharedHeader;
struct SharedLayout;
}

namespace skydiag::helper {
struct AttachedProcess;
struct HelperConfig;
}

namespace skydiag::helper::internal {

struct PendingCrashAnalysis;
struct PendingCrashEtwCapture;

struct CrashEventInfo {
  std::uint32_t exceptionCode = 0;
  std::uint64_t exceptionAddr = 0;
  std::uint32_t faultingTid = 0;
  std::uint32_t stateFlags = 0;
  std::uint32_t crashSeq = 0;
  bool isStrong = false;
  bool inMenu = false;
};

struct StableSharedSnapshot {
  struct AlignedByteDeleter {
    std::size_t alignment = alignof(std::max_align_t);
    void operator()(std::byte* ptr) const noexcept;
  };

  using Storage = std::unique_ptr<std::byte, AlignedByteDeleter>;

  Storage storage{nullptr, AlignedByteDeleter{}};
  std::size_t byteSize = 0;

  const skydiag::SharedLayout* layout() const noexcept;
  std::size_t size() const noexcept;
};

enum class FilterVerdict {
  kKeepDump,
  kDeleteBenign,
  kDeleteRecovered,
  kRetryNewerCrash,
};

// Bounded retry budget for crash dump writes.
inline constexpr int kDumpWriteAttempts = 3;

// Decides whether dump attempt `attemptIndex` (0-based) should run.
//
// The first attempt always runs. Retries only make sense while the game process
// is alive: MiniDumpWriteDump reads the target address space, so once the
// process exits there is nothing left to capture and further attempts would
// only delay the exit-path handling.
inline bool ShouldAttemptDumpWrite(int attemptIndex, bool processStillActive) noexcept
{
  if (attemptIndex <= 0) {
    return true;
  }
  if (attemptIndex >= kDumpWriteAttempts) {
    return false;
  }
  return processStillActive;
}

// Re-exported from the shared definition so the plugin and the helper classify
// fault severity identically; the plugin uses it to decide whether an already
// published fault outranks a later one.
inline constexpr std::uint32_t kStatusInvalidHandle = skydiag::kStatusInvalidHandle;
inline constexpr std::uint32_t kStatusCppException = skydiag::kStatusCppException;
inline constexpr std::uint32_t kStatusClrException = skydiag::kStatusClrException;
inline constexpr std::uint32_t kStatusBreakpoint = skydiag::kStatusBreakpoint;
inline constexpr std::uint32_t kStatusControlCExit = skydiag::kStatusControlCExit;
inline constexpr std::uint32_t kStateInMenu = 1u << 2;

inline bool IsStrongCrashExceptionCode(std::uint32_t code) noexcept
{
  return skydiag::IsStrongCrashExceptionCode(code);
}

inline CrashEventInfo BuildCrashEventInfo(
  std::uint32_t exceptionCode,
  std::uint64_t exceptionAddr,
  std::uint32_t faultingTid,
  std::uint32_t stateFlags) noexcept
{
  CrashEventInfo info{};
  info.exceptionCode = exceptionCode;
  info.exceptionAddr = exceptionAddr;
  info.faultingTid = faultingTid;
  info.stateFlags = stateFlags;
  info.isStrong = IsStrongCrashExceptionCode(exceptionCode);
  info.inMenu = (stateFlags & kStateInMenu) != 0u;
  return info;
}

inline FilterVerdict ClassifyExitCodeVerdict(
  std::uint32_t exitCode,
  const CrashEventInfo&,
  const std::filesystem::path&) noexcept
{
  if (exitCode == 0) {
    return FilterVerdict::kDeleteBenign;
  }
  return FilterVerdict::kKeepDump;
}

inline bool QueueDeferredCrashViewer(
  const std::wstring& dumpPath,
  std::wstring* pendingCrashViewerDumpPath) noexcept
{
  if (!pendingCrashViewerDumpPath) {
    return false;
  }
  if (pendingCrashViewerDumpPath->empty()) {
    *pendingCrashViewerDumpPath = dumpPath;
    return true;
  }
  if (*pendingCrashViewerDumpPath == dumpPath) {
    return true;
  }
  return false;
}

inline bool ShouldLatchCrashCapture(FilterVerdict verdict) noexcept
{
  return verdict == FilterVerdict::kKeepDump;
}

inline bool IsCommittedCrashSequence(std::uint32_t crashSeq) noexcept
{
  return crashSeq != 0u && (crashSeq & 1u) == 0u;
}

// Exit code 0 is treated as authoritative proof that an exception was handled,
// which is what keeps handled first-chance exceptions from producing dumps.
// That rule is deliberately absolute, so it also discards the rare genuine CTD
// that exits 0 — a foreign handler calling ExitProcess(0), a launcher
// normalizing the child exit code, or teardown that reaches a normal exit path
// after a fatal fault.
//
// This predicate does not change the delete decision. It only marks the subset
// where the deleted incident carried real fault evidence, so a compact metadata
// record can be preserved for investigation. `kDeleteRecovered` covers the
// heartbeat-recovery case separately, so reaching a benign zero-exit verdict
// with a committed strong fault means recovery was never actually observed.
inline bool ShouldQuarantineCleanExitEvidence(
  std::uint32_t exitCode,
  const CrashEventInfo& info) noexcept
{
  return exitCode == 0u && info.isStrong && IsCommittedCrashSequence(info.crashSeq);
}

CrashEventInfo ExtractCrashInfo(const skydiag::SharedHeader* shm) noexcept;
bool CaptureStableSharedSnapshot(
  const skydiag::SharedLayout* shm,
  std::size_t shmBytes,
  StableSharedSnapshot* out) noexcept;
bool TryClearRecoveredCrashFreeze(
  skydiag::SharedLayout* shm,
  std::uint32_t expectedCrashSeq) noexcept;
void WriteWerFallbackHint(const std::filesystem::path& outBase);

bool HandleCrashEventTick(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  DWORD waitMs,
  bool* crashCaptured,
  PendingCrashEtwCapture* pendingCrashEtw,
  PendingCrashAnalysis* pendingCrashAnalysis,
  std::wstring* lastCrashDumpPath,
  std::wstring* pendingHangViewerDumpPath,
  std::wstring* pendingCrashViewerDumpPath);

}
