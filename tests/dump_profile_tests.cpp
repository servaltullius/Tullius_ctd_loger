#include "SkyrimDiagHelper/DumpProfile.h"
#include "SourceGuardTestUtils.h"

#include <cassert>
#include <filesystem>
#include <string_view>

using skydiag::helper::CaptureKind;
using skydiag::helper::CaptureKindToString;
using skydiag::helper::DumpMode;
using skydiag::helper::DumpProfile;
using skydiag::helper::ResolveDumpProfile;
using skydiag::tests::source_guard::AssertContains;
using skydiag::tests::source_guard::ReadAllText;

namespace {

void AssertCrashRicherFlagsDisabled(const DumpProfile& profile)
{
  assert(!profile.includeProcessThreadData);
  assert(!profile.includeFullMemoryInfo);
  assert(!profile.includeModuleHeaders);
}

void AssertCrashRecaptureExtraFlagsDisabled(const DumpProfile& profile)
{
  AssertCrashRicherFlagsDisabled(profile);
  assert(!profile.includeIndirectMemory);
  assert(!profile.ignoreInaccessibleMemory);
}

void AssertDefaultCrashBase(const DumpProfile& profile)
{
  assert(profile.includeThreadInfo);
  assert(profile.includeHandleData);
  assert(profile.includeUnloadedModules);
  assert(profile.includeCodeSegments);
  assert(!profile.includeFullMemory);
  assert(profile.preferMainThread);
  assert(profile.preferCrashContext);
  assert(!profile.preferWctThreads);
}

}  // namespace

int main()
{
  const std::filesystem::path repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  const auto headerPath = repoRoot / "helper" / "include" / "SkyrimDiagHelper" / "DumpProfile.h";
  const auto implPath = repoRoot / "helper" / "src" / "DumpProfile.cpp";
  const auto helperCmakePath = repoRoot / "helper" / "CMakeLists.txt";

  assert(std::filesystem::exists(headerPath) && "helper/include/SkyrimDiagHelper/DumpProfile.h not found");
  assert(std::filesystem::exists(implPath) && "helper/src/DumpProfile.cpp not found");
  assert(std::filesystem::exists(helperCmakePath) && "helper/CMakeLists.txt not found");

  const auto header = ReadAllText(headerPath);
  const auto impl = ReadAllText(implPath);
  const auto helperCmake = ReadAllText(helperCmakePath);

  AssertContains(header, "enum class CaptureKind", "DumpProfile must declare capture kinds.");
  AssertContains(header, "Crash", "CaptureKind must include crash capture.");
  AssertContains(header, "Hang", "CaptureKind must include hang capture.");
  AssertContains(header, "Manual", "CaptureKind must include manual capture.");
  AssertContains(header, "CrashRecapture", "CaptureKind must include crash recapture.");
  AssertContains(header, "struct DumpProfile", "DumpProfile abstraction must exist.");
  AssertContains(header, "DumpMode baseMode", "DumpProfile must preserve the requested base mode.");
  AssertContains(header, "bool includeThreadInfo", "DumpProfile must model thread-info inclusion.");
  AssertContains(header, "bool includeHandleData", "DumpProfile must model handle-data inclusion.");
  AssertContains(header, "bool includeUnloadedModules", "DumpProfile must model unloaded-module inclusion.");
  AssertContains(header, "bool includeCodeSegments", "DumpProfile must model code-segment inclusion for machine-code-aware analysis.");
  AssertContains(header, "bool includeFullMemory", "DumpProfile must model full-memory inclusion separately.");
  AssertContains(header, "bool includeProcessThreadData", "DumpProfile must model process/thread-data inclusion.");
  AssertContains(header, "bool includeFullMemoryInfo", "DumpProfile must model full-memory-info inclusion.");
  AssertContains(header, "bool includeModuleHeaders", "DumpProfile must model module-header inclusion.");
  AssertContains(header, "bool includeIndirectMemory", "DumpProfile must model indirectly referenced memory inclusion.");
  AssertContains(header, "bool ignoreInaccessibleMemory", "DumpProfile must model inaccessible-memory tolerance.");
  AssertContains(header, "CaptureKindToString", "DumpProfile must expose a capture-kind string helper.");
  AssertContains(header, "ResolveDumpProfile", "DumpProfile must expose profile resolution.");

  AssertContains(impl, "ResolveDumpProfile", "DumpProfile.cpp must implement profile resolution.");
  AssertContains(impl, "CaptureKind::Crash", "Crash capture must map to a specific profile.");
  AssertContains(impl, "CaptureKind::Hang", "Hang capture must map to a specific profile.");
  AssertContains(impl, "CaptureKind::Manual", "Manual capture must map to a specific profile.");
  AssertContains(impl, "CaptureKind::CrashRecapture", "Crash recapture must map to a specific profile.");
  AssertContains(impl, "profile.includeCodeSegments = true", "At least one capture profile must request code segments for richer crash analysis.");
  AssertContains(impl, "profile.includeFullMemory = true", "At least one capture profile must be able to request full memory.");
  AssertContains(impl, "profile.includeProcessThreadData = true", "Crash profiles must request process/thread data.");
  AssertContains(impl, "profile.includeFullMemoryInfo = true", "Crash profiles must request full memory info.");
  AssertContains(impl, "profile.includeModuleHeaders = true", "Crash profiles must request module headers.");
  AssertContains(impl, "profile.includeIndirectMemory = true", "Crash recapture profiles must request indirectly referenced memory.");
  AssertContains(impl, "profile.ignoreInaccessibleMemory = true", "Crash recapture profiles must tolerate inaccessible memory.");
  AssertContains(impl, "profile.preferMainThread = true", "Profiles must express thread-priority intent.");
  AssertContains(impl, "profile.preferWctThreads = true", "Hang/manual profiles must express WCT-thread preference.");

  AssertContains(helperCmake, "src/DumpProfile.cpp", "Helper target must compile DumpProfile.cpp.");
  AssertContains(helperCmake, "include/SkyrimDiagHelper/DumpProfile.h", "Helper target must export DumpProfile.h.");

  assert(std::string_view(CaptureKindToString(CaptureKind::Crash)) == "crash");
  assert(std::string_view(CaptureKindToString(CaptureKind::Hang)) == "hang");
  assert(std::string_view(CaptureKindToString(CaptureKind::Manual)) == "manual");
  assert(std::string_view(CaptureKindToString(CaptureKind::CrashRecapture)) == "crash_recapture");

  const DumpProfile crashDefault = ResolveDumpProfile(DumpMode::kDefault, CaptureKind::Crash);
  assert(crashDefault.captureKind == CaptureKind::Crash);
  assert(crashDefault.baseMode == DumpMode::kDefault);
  AssertDefaultCrashBase(crashDefault);
  assert(crashDefault.includeProcessThreadData);
  assert(crashDefault.includeFullMemoryInfo);
  assert(crashDefault.includeModuleHeaders);
  assert(!crashDefault.includeIndirectMemory);
  assert(!crashDefault.ignoreInaccessibleMemory);

  const DumpProfile crashMini = ResolveDumpProfile(DumpMode::kMini, CaptureKind::Crash);
  assert(crashMini.captureKind == CaptureKind::Crash);
  assert(crashMini.baseMode == DumpMode::kMini);
  assert(!crashMini.includeThreadInfo);
  assert(!crashMini.includeHandleData);
  assert(!crashMini.includeUnloadedModules);
  assert(!crashMini.includeCodeSegments);
  assert(!crashMini.includeFullMemory);
  assert(crashMini.preferMainThread);
  assert(crashMini.preferCrashContext);
  assert(!crashMini.preferWctThreads);
  AssertCrashRicherFlagsDisabled(crashMini);

  const DumpProfile crashFull = ResolveDumpProfile(DumpMode::kFull, CaptureKind::Crash);
  assert(crashFull.captureKind == CaptureKind::Crash);
  assert(crashFull.baseMode == DumpMode::kFull);
  assert(crashFull.includeThreadInfo);
  assert(crashFull.includeHandleData);
  assert(crashFull.includeUnloadedModules);
  assert(crashFull.includeCodeSegments);
  assert(crashFull.includeFullMemory);
  assert(crashFull.preferMainThread);
  assert(crashFull.preferCrashContext);
  assert(!crashFull.preferWctThreads);
  AssertCrashRicherFlagsDisabled(crashFull);

  const DumpProfile recaptureDefault = ResolveDumpProfile(DumpMode::kDefault, CaptureKind::CrashRecapture);
  assert(recaptureDefault.captureKind == CaptureKind::CrashRecapture);
  assert(recaptureDefault.baseMode == DumpMode::kDefault);
  AssertDefaultCrashBase(recaptureDefault);
  assert(recaptureDefault.includeProcessThreadData);
  assert(recaptureDefault.includeFullMemoryInfo);
  assert(recaptureDefault.includeModuleHeaders);
  assert(recaptureDefault.includeIndirectMemory);
  assert(recaptureDefault.ignoreInaccessibleMemory);

  const DumpProfile recaptureMini = ResolveDumpProfile(DumpMode::kMini, CaptureKind::CrashRecapture);
  assert(recaptureMini.captureKind == CaptureKind::CrashRecapture);
  assert(recaptureMini.baseMode == DumpMode::kMini);
  assert(recaptureMini.includeThreadInfo);
  assert(recaptureMini.includeHandleData);
  assert(recaptureMini.includeUnloadedModules);
  assert(recaptureMini.includeCodeSegments);
  assert(!recaptureMini.includeFullMemory);
  assert(recaptureMini.preferMainThread);
  assert(recaptureMini.preferCrashContext);
  assert(!recaptureMini.preferWctThreads);
  AssertCrashRecaptureExtraFlagsDisabled(recaptureMini);

  const DumpProfile recaptureFull = ResolveDumpProfile(DumpMode::kFull, CaptureKind::CrashRecapture);
  assert(recaptureFull.captureKind == CaptureKind::CrashRecapture);
  assert(recaptureFull.baseMode == DumpMode::kFull);
  assert(recaptureFull.includeThreadInfo);
  assert(recaptureFull.includeHandleData);
  assert(recaptureFull.includeUnloadedModules);
  assert(recaptureFull.includeCodeSegments);
  assert(recaptureFull.includeFullMemory);
  assert(recaptureFull.preferMainThread);
  assert(recaptureFull.preferCrashContext);
  assert(!recaptureFull.preferWctThreads);
  AssertCrashRecaptureExtraFlagsDisabled(recaptureFull);

  const DumpProfile hangDefault = ResolveDumpProfile(DumpMode::kDefault, CaptureKind::Hang);
  assert(hangDefault.captureKind == CaptureKind::Hang);
  assert(hangDefault.baseMode == DumpMode::kDefault);
  assert(hangDefault.includeThreadInfo);
  assert(hangDefault.includeHandleData);
  assert(hangDefault.includeUnloadedModules);
  assert(hangDefault.includeCodeSegments);
  assert(!hangDefault.includeFullMemory);
  assert(hangDefault.preferMainThread);
  assert(hangDefault.preferWctThreads);
  assert(!hangDefault.preferCrashContext);
  AssertCrashRecaptureExtraFlagsDisabled(hangDefault);

  const DumpProfile hangMini = ResolveDumpProfile(DumpMode::kMini, CaptureKind::Hang);
  assert(hangMini.captureKind == CaptureKind::Hang);
  assert(hangMini.baseMode == DumpMode::kMini);
  assert(hangMini.includeThreadInfo);
  assert(!hangMini.includeHandleData);
  assert(!hangMini.includeUnloadedModules);
  assert(!hangMini.includeCodeSegments);
  assert(!hangMini.includeFullMemory);
  assert(hangMini.preferMainThread);
  assert(hangMini.preferWctThreads);
  assert(!hangMini.preferCrashContext);
  AssertCrashRecaptureExtraFlagsDisabled(hangMini);

  const DumpProfile manualDefault = ResolveDumpProfile(DumpMode::kDefault, CaptureKind::Manual);
  assert(manualDefault.captureKind == CaptureKind::Manual);
  assert(manualDefault.baseMode == DumpMode::kDefault);
  assert(manualDefault.includeThreadInfo);
  assert(manualDefault.includeHandleData);
  assert(manualDefault.includeUnloadedModules);
  assert(manualDefault.includeCodeSegments);
  assert(!manualDefault.includeFullMemory);
  assert(manualDefault.preferMainThread);
  assert(manualDefault.preferWctThreads);
  assert(!manualDefault.preferCrashContext);
  AssertCrashRecaptureExtraFlagsDisabled(manualDefault);

  const DumpProfile manualMini = ResolveDumpProfile(DumpMode::kMini, CaptureKind::Manual);
  assert(manualMini.captureKind == CaptureKind::Manual);
  assert(manualMini.baseMode == DumpMode::kMini);
  assert(manualMini.includeThreadInfo);
  assert(!manualMini.includeHandleData);
  assert(!manualMini.includeUnloadedModules);
  assert(!manualMini.includeCodeSegments);
  assert(!manualMini.includeFullMemory);
  assert(manualMini.preferMainThread);
  assert(manualMini.preferWctThreads);
  assert(!manualMini.preferCrashContext);
  AssertCrashRecaptureExtraFlagsDisabled(manualMini);

  return 0;
}
