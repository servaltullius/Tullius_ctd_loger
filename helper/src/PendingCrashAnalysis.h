#pragma once

#include <Windows.h>

#include <filesystem>
#include <string>

namespace skydiag::helper {
struct AttachedProcess;
struct HelperConfig;
}

namespace skydiag::helper::internal {

struct PendingCrashAnalysis
{
  bool active = false;
  std::wstring dumpPath;
  HANDLE process = nullptr;
  ULONGLONG startedAtTick64 = 0;
  DWORD timeoutMs = 0;
};

void ClearPendingCrashAnalysis(PendingCrashAnalysis* task);

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
  PendingCrashAnalysis* task);

}  // namespace skydiag::helper::internal

