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

DumpProfile ResolveDumpProfile(DumpMode baseMode, CaptureKind captureKind)
{
  DumpProfile profile{};
  profile.captureKind = captureKind;
  profile.baseMode = baseMode;

  if (baseMode != DumpMode::kMini) {
    profile.includeThreadInfo = true;
    profile.includeHandleData = true;
    profile.includeUnloadedModules = true;
  }
  if (baseMode == DumpMode::kFull) {
    profile.includeFullMemory = true;
  }

  switch (captureKind) {
    case CaptureKind::Crash:
      profile.preferCrashContext = true;
      profile.preferMainThread = true;
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
      profile.includeFullMemory = (baseMode == DumpMode::kFull);
      break;
  }

  return profile;
}

}  // namespace skydiag::helper
