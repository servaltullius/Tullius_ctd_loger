#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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

[[noreturn]] static void FailNow(const char* message)
{
  std::cerr << message << "\n";
  std::exit(1);
}

static void Require(bool condition, const char* message)
{
  if (!condition) {
    FailNow(message);
  }
}

int main()
{
  const std::filesystem::path repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto heartbeatPath = repoRoot / "plugin" / "src" / "Heartbeat.cpp";
  const auto resourceHooksPath = repoRoot / "plugin" / "src" / "ResourceHooks.cpp";
  const auto pluginMainPath = repoRoot / "plugin" / "src" / "PluginMain.cpp";
  const auto sharedMemoryPath = repoRoot / "plugin" / "src" / "SharedMemory.cpp";
  const auto crashHandlerPath = repoRoot / "plugin" / "src" / "CrashHandler.cpp";

  assert(std::filesystem::exists(heartbeatPath) && "plugin/src/Heartbeat.cpp not found");
  assert(std::filesystem::exists(resourceHooksPath) && "plugin/src/ResourceHooks.cpp not found");
  assert(std::filesystem::exists(pluginMainPath) && "plugin/src/PluginMain.cpp not found");
  assert(std::filesystem::exists(sharedMemoryPath) && "plugin/src/SharedMemory.cpp not found");
  assert(std::filesystem::exists(crashHandlerPath) && "plugin/src/CrashHandler.cpp not found");

  const std::string heartbeat = ReadAllText(heartbeatPath);
  const std::string resourceHooks = ReadAllText(resourceHooksPath);
  const std::string pluginMain = ReadAllText(pluginMainPath);
  const std::string sharedMemory = ReadAllText(sharedMemoryPath);
  const std::string crashHandler = ReadAllText(crashHandlerPath);

  AssertContains(
    heartbeat,
    "ti->AddUITask([]() { HeartbeatTaskOnMainThread(); });",
    "Heartbeat scheduler must enqueue UI-thread task for stall detection semantics.");

  AssertContains(
    heartbeat,
    "Never leave the scheduler stuck in pending=true if task enqueue throws.",
    "Heartbeat scheduler must explicitly guard against pending-state deadlock when enqueue throws.");

  AssertContains(
    heartbeat,
    "catch (...)",
    "Heartbeat scheduler must catch enqueue failures to avoid scheduler deadlock.");

  AssertContains(
    heartbeat,
    "g_taskPending.store(false);",
    "Heartbeat scheduler must clear pending flag if task enqueue throws.");

  AssertContains(
    resourceHooks,
    "IsInterestingResourceName",
    "Resource hook must pre-filter filename extensions before full path assembly.");

  const auto filterPos = resourceHooks.find("if (!IsInterestingResourceName(fileName))");
  const auto buildPos = resourceHooks.find("char buf[512]{};");
  assert(filterPos != std::string::npos && "Resource hook must skip non-interesting files early.");
  assert(buildPos != std::string::npos && "Resource hook path assembly buffer not found.");
  assert(filterPos < buildPos && "Resource hook must filter before path assembly for hot-path performance.");

  AssertContains(
    pluginMain,
    "failed to enqueue crash hotkey UI task",
    "Test hotkey crash path must log enqueue failures.");

  AssertContains(
    pluginMain,
    "failed to enqueue hang hotkey UI task",
    "Test hotkey hang path must log enqueue failures.");

  AssertContains(
    sharedMemory,
    "if (!g_crashEvent)",
    "Shared memory init must fail fast when crash event creation fails.");

  const auto createEventPos = sharedMemory.find("g_crashEvent = CreateEventW(");
  const auto crashEventFailPos = sharedMemory.find("if (!g_crashEvent)");
  const auto unmapOnFailPos = sharedMemory.find("UnmapViewOfFile(g_shared);", crashEventFailPos);
  const auto closeMappingOnFailPos = sharedMemory.find("CloseHandle(g_mapping);", crashEventFailPos);
  Require(createEventPos != std::string::npos, "CreateEventW call not found in shared memory init.");
  Require(crashEventFailPos != std::string::npos, "Missing crash event failure branch in shared memory init.");
  Require(unmapOnFailPos != std::string::npos, "Crash event failure branch must unmap shared memory view.");
  Require(closeMappingOnFailPos != std::string::npos, "Crash event failure branch must close mapping handle.");
  Require(createEventPos < crashEventFailPos, "Crash event failure branch must appear after event creation.");

  Require(
    crashHandler.find("InterlockedIncrement(") != std::string::npos,
    "Crash handler must bump crash_seq for each captured crash signal so recovery does not permanently disable capture.");
  Require(
    crashHandler.find("InterlockedCompareExchange(") == std::string::npos,
    "Crash handler must not use one-shot crash_seq latch because it blocks future captures after recovery.");

  return 0;
}
