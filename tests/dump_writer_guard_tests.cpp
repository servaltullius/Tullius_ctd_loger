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
  const auto hangCaptureGuardsPath = repoRoot / "helper" / "src" / "HangCapture.Guards.cpp";
  const auto hangCaptureExecutePath = repoRoot / "helper" / "src" / "HangCapture.Execute.cpp";
  const auto manualCapturePath = repoRoot / "helper" / "src" / "ManualCapture.cpp";
  const auto pendingAnalysisPath = repoRoot / "helper" / "src" / "PendingCrashAnalysis.cpp";
  const auto pendingAnalysisDecisionPath = repoRoot / "helper" / "src" / "PendingCrashAnalysis.Decision.cpp";
  const auto pendingAnalysisExecutePath = repoRoot / "helper" / "src" / "PendingCrashAnalysis.Execute.cpp";

  assert(std::filesystem::exists(headerPath) && "helper/include/SkyrimDiagHelper/DumpWriter.h not found");
  assert(std::filesystem::exists(implPath) && "helper/src/DumpWriter.cpp not found");

  const auto header = ReadAllText(headerPath);
  const auto impl = ReadAllText(implPath);
  const auto crashCapture = ReadAllText(crashCapturePath);
  const auto hangCapture =
    ReadAllText(hangCapturePath) +
    ReadAllText(hangCaptureGuardsPath) +
    ReadAllText(hangCaptureExecutePath);
  const auto manualCapture = ReadAllText(manualCapturePath);
  const auto pendingAnalysis =
    ReadAllText(pendingAnalysisPath) +
    ReadAllText(pendingAnalysisDecisionPath) +
    ReadAllText(pendingAnalysisExecutePath);

  AssertContains(header, "DumpProfile", "DumpWriter must accept an effective DumpProfile.");
  AssertContains(header, "const DumpProfile& dumpProfile", "DumpWriter signature must take DumpProfile by const reference.");
  AssertContains(header, "bool isProcessSnapshot", "DumpWriter must accept a process-snapshot marker for PSS exports.");

  AssertContains(impl, "ResolveDumpProfile", "DumpWriter implementation must resolve or consume dump profiles.");
  AssertContains(impl, "MINIDUMP_CALLBACK_INFORMATION", "DumpWriter must wire MiniDump callback information.");
  AssertContains(impl, "MiniDumpWriteDump(", "DumpWriter must still use MiniDumpWriteDump.");
  AssertContains(impl, "&callbackInfo", "MiniDumpWriteDump must receive callback information.");
  AssertContains(impl, "ApplyProfileToDumpType", "DumpWriter must derive dump flags from profile state.");
  AssertContains(impl, "MiniDumpWithCodeSegs", "DumpWriter must request code segments when the dump profile enables machine-code capture.");
  AssertContains(impl, "MiniDumpWithProcessThreadData", "DumpWriter must request process/thread data when profile enables it.");
  AssertContains(impl, "MiniDumpWithFullMemoryInfo", "DumpWriter must request full memory info when profile enables it.");
  AssertContains(impl, "MiniDumpWithModuleHeaders", "DumpWriter must request module headers when profile enables it.");
  AssertContains(impl, "MiniDumpWithIndirectlyReferencedMemory", "DumpWriter must request indirectly referenced memory for richer crash recapture.");
  AssertContains(impl, "MiniDumpIgnoreInaccessibleMemory", "DumpWriter must tolerate inaccessible memory when requested.");
  AssertContains(impl, "MiniDumpCallback", "DumpWriter must define a callback routine.");
  AssertContains(impl, "IsProcessSnapshotCallback", "DumpWriter callback must handle process snapshot exports.");
  AssertContains(impl, "IncludeThreadCallback", "DumpWriter callback must start shaping preferred threads.");
  AssertContains(impl, "ThreadWriteFlags", "DumpWriter callback must control thread write flags for preferred CTD threads.");
  AssertContains(impl, "preferredThreadId", "DumpWriter callback must keep honoring the preferred crash thread.");
  AssertContains(impl, "ExtractPreferredWctThreadIds", "DumpWriter must derive preferred WCT threads from capture metadata.");
  AssertContains(impl, "\"isCycle\"", "DumpWriter must look for WCT cycle-thread markers when shaping hang/manual dumps.");
  AssertContains(impl, "preferredThreadIds", "DumpWriter callback context must track multiple preferred threads.");
  AssertContains(impl, "preferWctThreads", "DumpWriter shaping must honor WCT-thread preference from the dump profile.");

  AssertContains(crashCapture, "CaptureKind::Crash", "Crash capture must request the crash dump profile.");
  AssertContains(hangCapture, "CaptureKind::Hang", "Hang capture must request the hang dump profile.");
  AssertContains(manualCapture, "CaptureKind::Manual", "Manual capture must request the manual capture profile.");
  AssertContains(pendingAnalysis, "CaptureKind::CrashRecapture", "Crash recapture must request the recapture profile.");

  return 0;
}
