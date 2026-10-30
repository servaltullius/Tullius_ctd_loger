#pragma once

#include <cstdint>

#include "SkyrimDiagHelper/Config.h"

namespace skydiag::helper {

enum class CaptureKind {
  Crash,
  Hang,
  Manual,
  CrashRecapture,
};

struct DumpProfile
{
  CaptureKind captureKind = CaptureKind::Crash;
  DumpMode baseMode = DumpMode::kDefault;
  bool includeThreadInfo = false;
  bool includeHandleData = false;
  bool includeUnloadedModules = false;
  bool includeCodeSegments = false;
  bool includeFullMemory = false;
  bool includeProcessThreadData = false;
  bool includeFullMemoryInfo = false;
  bool includeModuleHeaders = false;
  bool includeIndirectMemory = false;
  bool ignoreInaccessibleMemory = false;
  bool preferMainThread = false;
  bool preferWctThreads = false;
  bool preferCrashContext = false;
  // DumpAllModuleCode, as the profile was resolved with.
  bool allModuleCode = false;
  // ADR-0023: of the module code, only the game executable's is written
  // whole; the writer adds what the stacks point at instead.
  bool gameAndStackCodeOnly = false;
};

// MINIDUMP_TYPE flag MiniDumpIgnoreInaccessibleMemory, spelled out so this
// header stays free of <DbgHelp.h> for the host-independent tests.
inline constexpr std::uint32_t kMiniDumpIgnoreInaccessibleMemoryFlag = 0x00020000u;

// MiniDumpWriteDump fails as a whole with ERROR_PARTIAL_COPY (reported as the
// Win32 code or as its HRESULT, 0x8007012B) when a single memory region cannot
// be read, for example because something else changed it while the dump was
// being written. Retrying with the same dump type fails the same way, so the
// writer retries once with unreadable regions skipped: a dump missing one
// region is far more useful than no dump of a CTD at all.
constexpr bool ShouldRetryDumpIgnoringInaccessibleMemory(std::uint32_t dumpType, std::uint32_t lastError) noexcept
{
  constexpr std::uint32_t kErrorPartialCopy = 299u;
  constexpr std::uint32_t kHresultPartialCopy = 0x8007012Bu;
  return (dumpType & kMiniDumpIgnoreInaccessibleMemoryFlag) == 0u &&
         (lastError == kErrorPartialCopy || lastError == kHresultPartialCopy);
}

const char* CaptureKindToString(CaptureKind captureKind);
// allModuleCode (ini DumpAllModuleCode=1): every module's code and, for
// crash dumps, MiniDumpWithModuleHeaders, as through v0.2.71 (ADR-0023).
DumpProfile ResolveDumpProfile(DumpMode baseMode, CaptureKind captureKind, bool allModuleCode = false);
// "none", "all" or "game_and_stacks": the module code the profile writes.
const char* CodeSegmentsScopeToString(const DumpProfile& profile);

}  // namespace skydiag::helper
