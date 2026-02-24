#include <cassert>
#include <filesystem>
#include <string>

#include "SourceGuardTestUtils.h"

using skydiag::tests::source_guard::AssertContains;
using skydiag::tests::source_guard::AssertOrdered;
using skydiag::tests::source_guard::ExtractFunctionBody;
using skydiag::tests::source_guard::ReadAllText;

int main()
{
  const std::filesystem::path repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
  const std::filesystem::path crashCapturePath = repoRoot / "helper" / "src" / "CrashCapture.cpp";
  const std::filesystem::path crashCaptureHeaderPath = repoRoot / "helper" / "src" / "CrashCapture.h";
  assert(std::filesystem::exists(crashCapturePath) && "helper/src/CrashCapture.cpp not found");
  assert(std::filesystem::exists(crashCaptureHeaderPath) && "helper/src/CrashCapture.h not found");

  const std::string crashCapture = ReadAllText(crashCapturePath);
  const std::string crashCaptureHeader = ReadAllText(crashCaptureHeaderPath);

  const std::string crashTickBody = ExtractFunctionBody(crashCapture, "bool HandleCrashEventTick(");
  AssertContains(
    crashTickBody,
    "ExtractCrashInfo(",
    "HandleCrashEventTick must extract crash information via helper function.");
  AssertContains(
    crashTickBody,
    "FilterShutdownException(",
    "HandleCrashEventTick must call shutdown filter helper.");
  AssertContains(
    crashTickBody,
    "ProcessValidCrashDump(",
    "HandleCrashEventTick must call extracted post-processing helper.");
  AssertOrdered(
    crashTickBody,
    "WriteDumpWithStreams(",
    "FilterShutdownException(",
    "Dump-first strategy must be preserved before filtering.");

  const std::string processValidBody = ExtractFunctionBody(crashCapture, "void ProcessValidCrashDump(");
  AssertContains(
    processValidBody,
    "CollectPluginScanJson(",
    "ProcessValidCrashDump must collect plugin scan sidecar.");
  AssertContains(
    processValidBody,
    "StartEtwCaptureWithProfile(",
    "ProcessValidCrashDump must attempt ETW start when configured.");
  AssertContains(
    processValidBody,
    "MakeIncidentManifestV1(",
    "ProcessValidCrashDump must write incident manifest when enabled.");
  AssertContains(
    processValidBody,
    "ApplyRetentionFromConfig(cfg, outBase)",
    "ProcessValidCrashDump must apply retention policy.");
  AssertOrdered(
    processValidBody,
    "StartEtwCaptureWithProfile(",
    "MakeIncidentManifestV1(",
    "ETW start logic must appear before manifest generation.");

  AssertContains(
    crashCaptureHeader,
    "struct CrashEventInfo",
    "CrashCapture header must expose CrashEventInfo for extracted flow.");
  AssertContains(
    crashCaptureHeader,
    "enum class FilterVerdict",
    "CrashCapture header must expose FilterVerdict for filtering outcomes.");
  AssertContains(
    crashCaptureHeader,
    "inline FilterVerdict ClassifyExitCodeVerdict(",
    "CrashCapture header must provide exit-code classification helper.");

  return 0;
}
