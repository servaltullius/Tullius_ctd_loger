#pragma once

#include <Windows.h>

#include <filesystem>
#include <string>

namespace skydiag::helper {
struct AttachedProcess;
struct HelperConfig;
}

namespace skydiag::helper::internal {

// A full recapture of a crash dump (EnableAutoRecaptureOnUnknownCrash) and its
// incident manifest. If that crash is later filtered (zero exit, late
// recovery) they are cleaned up with the original dump.
struct CrashRecaptureArtifacts
{
  std::filesystem::path dump;
  std::filesystem::path manifest;
};

struct PendingCrashAnalysis
{
  bool active = false;
  std::wstring dumpPath;
  HANDLE process = nullptr;
  ULONGLONG startedAtTick64 = 0;
  DWORD timeoutMs = 0;
};

void ClearPendingCrashAnalysis(PendingCrashAnalysis* task);
// Stops tracking the analyzer without stopping it: it finishes and writes
// its report on its own (outputs are written atomically).
void DetachPendingCrashAnalysis(PendingCrashAnalysis* task);

bool StartPendingCrashAnalysisTask(
  const skydiag::helper::HelperConfig& cfg,
  const std::wstring& dumpPath,
  const std::filesystem::path& outBase,
  PendingCrashAnalysis* task,
  std::wstring* err);

void FinalizePendingCrashAnalysisIfReady(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  PendingCrashAnalysis* task,
  CrashRecaptureArtifacts* recaptureOut = nullptr);

}  // namespace skydiag::helper::internal

