#include "SourceGuardTestUtils.h"

#include <filesystem>

using skydiag::tests::source_guard::AssertContains;
using skydiag::tests::source_guard::ReadAllText;

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
  AssertContains(header, "bool includeFullMemory", "DumpProfile must model full-memory inclusion separately.");
  AssertContains(header, "CaptureKindToString", "DumpProfile must expose a capture-kind string helper.");
  AssertContains(header, "ResolveDumpProfile", "DumpProfile must expose profile resolution.");

  AssertContains(impl, "ResolveDumpProfile", "DumpProfile.cpp must implement profile resolution.");
  AssertContains(impl, "CaptureKind::Crash", "Crash capture must map to a specific profile.");
  AssertContains(impl, "CaptureKind::Hang", "Hang capture must map to a specific profile.");
  AssertContains(impl, "CaptureKind::Manual", "Manual capture must map to a specific profile.");
  AssertContains(impl, "CaptureKind::CrashRecapture", "Crash recapture must map to a specific profile.");
  AssertContains(impl, "profile.includeFullMemory = true", "At least one capture profile must be able to request full memory.");
  AssertContains(impl, "profile.preferMainThread = true", "Profiles must express thread-priority intent.");
  AssertContains(impl, "profile.preferWctThreads = true", "Hang/manual profiles must express WCT-thread preference.");

  AssertContains(helperCmake, "src/DumpProfile.cpp", "Helper target must compile DumpProfile.cpp.");
  AssertContains(helperCmake, "include/SkyrimDiagHelper/DumpProfile.h", "Helper target must export DumpProfile.h.");
  return 0;
}
