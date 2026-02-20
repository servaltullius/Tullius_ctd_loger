#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

static std::string ReadAllText(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::in | std::ios::binary);
  assert(in && "Failed to open file");
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static void AssertContains(const std::string& haystack, const char* needle, const char* message)
{
  assert(haystack.find(needle) != std::string::npos && message);
}

int main()
{
  const std::filesystem::path repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
  const std::filesystem::path pendingPath = repoRoot / "helper" / "src" / "PendingCrashAnalysis.cpp";
  assert(std::filesystem::exists(pendingPath) && "helper/src/PendingCrashAnalysis.cpp not found");

  const std::string pending = ReadAllText(pendingPath);

  AssertContains(
    pending,
    "void ClearPendingCrashAnalysis(PendingCrashAnalysis* task)",
    "Pending crash analysis clear function must exist.");

  AssertContains(
    pending,
    "WaitForSingleObject(task->process, 0)",
    "ClearPendingCrashAnalysis must probe process liveness before cleanup.");

  AssertContains(
    pending,
    "TerminateProcess(task->process, 1)",
    "Pending crash analysis cleanup must terminate stale analyzer processes.");

  AssertContains(
    pending,
    "WaitForSingleObject(task->process, 1000)",
    "Pending crash analysis cleanup must wait briefly after terminate to reduce process leak risk.");

  AssertContains(
    pending,
    "if (task->active)",
    "StartPendingCrashAnalysisTask must guard and clear previous active task state.");

  AssertContains(
    pending,
    "ClearPendingCrashAnalysis(task);",
    "Starting a new pending crash analysis must clear any previous active task state first.");

  return 0;
}
