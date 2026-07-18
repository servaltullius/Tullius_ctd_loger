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

inline constexpr std::uint32_t kStatusInvalidHandle = 0xC0000008u;
inline constexpr std::uint32_t kStatusCppException = 0xE06D7363u;
inline constexpr std::uint32_t kStatusClrException = 0xE0434F4Du;
inline constexpr std::uint32_t kStatusBreakpoint = 0x80000003u;
inline constexpr std::uint32_t kStatusControlCExit = 0xC000013Au;
inline constexpr std::uint32_t kStateInMenu = 1u << 2;

inline bool IsStrongCrashExceptionCode(std::uint32_t code) noexcept
{
  if (code == 0) {
    return false;
  }
  if (code == kStatusInvalidHandle ||
      code == kStatusCppException ||
      code == kStatusClrException ||
      code == kStatusBreakpoint ||
      code == kStatusControlCExit) {
    return false;
  }
  return true;
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
