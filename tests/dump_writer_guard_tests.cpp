#include "SourceGuardTestUtils.h"

#include <filesystem>

using skydiag::tests::source_guard::AssertContains;
using skydiag::tests::source_guard::ReadAllText;

int main()
{
  const std::filesystem::path repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  const auto headerPath = repoRoot / "helper" / "include" / "SkyrimDiagHelper" / "DumpWriter.h";
  const auto implPath = repoRoot / "helper" / "src" / "DumpWriter.cpp";
  const auto crashCapturePath = repoRoot / "helper" / "src" / "CrashCapture.cpp";
  const auto hangCapturePath = repoRoot / "helper" / "src" / "HangCapture.cpp";
  const auto manualCapturePath = repoRoot / "helper" / "src" / "ManualCapture.cpp";
  const auto pendingAnalysisPath = repoRoot / "helper" / "src" / "PendingCrashAnalysis.cpp";

  assert(std::filesystem::exists(headerPath) && "helper/include/SkyrimDiagHelper/DumpWriter.h not found");
  assert(std::filesystem::exists(implPath) && "helper/src/DumpWriter.cpp not found");

  const auto header = ReadAllText(headerPath);
  const auto impl = ReadAllText(implPath);
  const auto crashCapture = ReadAllText(crashCapturePath);
  const auto hangCapture = ReadAllText(hangCapturePath);
  const auto manualCapture = ReadAllText(manualCapturePath);
  const auto pendingAnalysis = ReadAllText(pendingAnalysisPath);

  AssertContains(header, "DumpProfile", "DumpWriter must accept an effective DumpProfile.");
  AssertContains(header, "const DumpProfile& dumpProfile", "DumpWriter signature must take DumpProfile by const reference.");

  AssertContains(impl, "ResolveDumpProfile", "DumpWriter implementation must resolve or consume dump profiles.");
  AssertContains(impl, "MINIDUMP_CALLBACK_INFORMATION", "DumpWriter must wire MiniDump callback information.");
  AssertContains(impl, "MiniDumpWriteDump(", "DumpWriter must still use MiniDumpWriteDump.");
  AssertContains(impl, "&callbackInfo", "MiniDumpWriteDump must receive callback information.");
  AssertContains(impl, "ApplyProfileToDumpType", "DumpWriter must derive dump flags from profile state.");
  AssertContains(impl, "MiniDumpCallback", "DumpWriter must define a callback routine.");

  AssertContains(crashCapture, "CaptureKind::Crash", "Crash capture must request the crash dump profile.");
  AssertContains(hangCapture, "CaptureKind::Hang", "Hang capture must request the hang dump profile.");
  AssertContains(manualCapture, "CaptureKind::Manual", "Manual capture must request the manual capture profile.");
  AssertContains(pendingAnalysis, "CaptureKind::CrashRecapture", "Crash recapture must request the recapture profile.");

  return 0;
}
