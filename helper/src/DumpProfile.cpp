#include "SkyrimDiagHelper/DumpProfile.h"

namespace skydiag::helper {

const char* CaptureKindToString(CaptureKind captureKind)
{
  switch (captureKind) {
    case CaptureKind::Crash:
      return "crash";
    case CaptureKind::Hang:
      return "hang";
    case CaptureKind::Manual:
      return "manual";
    case CaptureKind::CrashRecapture:
      return "crash_recapture";
  }
  return "crash";
}

DumpProfile ResolveDumpProfile(DumpMode baseMode, CaptureKind captureKind, bool allModuleCode)
{
  DumpProfile profile{};
  profile.captureKind = captureKind;
  profile.baseMode = baseMode;
  profile.allModuleCode = allModuleCode;

  if (baseMode != DumpMode::kMini) {
    profile.includeThreadInfo = true;
    profile.includeHandleData = true;
    profile.includeUnloadedModules = true;
    profile.includeCodeSegments = true;
  }
  if (baseMode == DumpMode::kFull) {
    profile.includeFullMemory = true;
  }

  switch (captureKind) {
    case CaptureKind::Crash:
      profile.preferCrashContext = true;
      profile.preferMainThread = true;
      if (baseMode == DumpMode::kDefault) {
        profile.includeProcessThreadData = true;
        profile.includeFullMemoryInfo = true;
        // ADR-0023: this flag adds every module's resources and relocations
        // as well; the writer adds the headers, exports and unwind data the
        // analysis needs instead.
        profile.includeModuleHeaders = allModuleCode;
      }
      break;
    case CaptureKind::Hang:
      profile.preferMainThread = true;
      profile.preferWctThreads = true;
      if (baseMode == DumpMode::kMini) {
        profile.includeThreadInfo = true;
      }
      break;
    case CaptureKind::Manual:
      profile.preferMainThread = true;
      profile.preferWctThreads = true;
      if (baseMode == DumpMode::kMini) {
        profile.includeThreadInfo = true;
      }
      break;
    case CaptureKind::CrashRecapture:
      profile.preferCrashContext = true;
      profile.preferMainThread = true;
      profile.includeThreadInfo = true;
      profile.includeHandleData = true;
      profile.includeUnloadedModules = true;
      profile.includeCodeSegments = true;
      if (baseMode == DumpMode::kDefault) {
        profile.includeProcessThreadData = true;
        profile.includeFullMemoryInfo = true;
        profile.includeModuleHeaders = true;
        profile.includeIndirectMemory = true;
        profile.ignoreInaccessibleMemory = true;
      }
      profile.includeFullMemory = (baseMode == DumpMode::kFull);
      break;
  }

  // ADR-0023: DumpMode=1 crash, freeze and manual dumps. A crash recapture is
  // asked for to get more, and keeps every module's code.
  profile.gameAndStackCodeOnly = baseMode == DumpMode::kDefault && captureKind != CaptureKind::CrashRecapture &&
    profile.includeCodeSegments && !allModuleCode;

  return profile;
}

const char* CodeSegmentsScopeToString(const DumpProfile& profile)
{
  if (!profile.includeCodeSegments && !profile.includeFullMemory) {
    return "none";
  }
  return profile.gameAndStackCodeOnly ? "game_and_stacks" : "all";
}

}  // namespace skydiag::helper
