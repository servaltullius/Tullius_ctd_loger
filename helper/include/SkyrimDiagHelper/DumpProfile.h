#pragma once

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
};

const char* CaptureKindToString(CaptureKind captureKind);
DumpProfile ResolveDumpProfile(DumpMode baseMode, CaptureKind captureKind);

}  // namespace skydiag::helper
