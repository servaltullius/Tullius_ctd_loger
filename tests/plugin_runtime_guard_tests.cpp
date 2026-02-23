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

  const std::string queueHeartbeatTaskBody = ExtractFunctionBody(heartbeat, "void QueueHeartbeatTask() noexcept");
  AssertContains(
    queueHeartbeatTaskBody,
    "g_taskPending.compare_exchange_strong",
    "Heartbeat scheduler must gate task queueing with pending compare-exchange.");

  AssertContains(
    queueHeartbeatTaskBody,
    "ti->AddUITask",
    "Heartbeat scheduler must enqueue UI-thread heartbeat task.");

  AssertContains(
    queueHeartbeatTaskBody,
    "catch (...)",
    "Heartbeat scheduler must catch enqueue failures to avoid scheduler deadlock.");

  AssertContains(
    queueHeartbeatTaskBody,
    "g_taskPending.store(false);",
    "Heartbeat scheduler must clear pending flag if task enqueue throws.");

  AssertOrdered(
    queueHeartbeatTaskBody,
    "g_taskPending.compare_exchange_strong",
    "ti->AddUITask",
    "Heartbeat scheduler must check/set pending state before queueing UI task.");

  const std::string looseFileOpenHookBody = ExtractFunctionBody(resourceHooks, "ErrorCode LooseFileDoOpen_Hook(");
  AssertContains(
    looseFileOpenHookBody,
    "IsInterestingResourceName",
    "Resource hook must pre-filter filename extensions before full path assembly.");

  AssertContains(
    looseFileOpenHookBody,
    "NoteResourceOpen(",
    "Resource hook must record interesting resource opens.");

  AssertOrdered(
    looseFileOpenHookBody,
    "if (!IsInterestingResourceName(fileName))",
    "char buf[512]{};",
    "Resource hook must filter before path assembly for hot-path performance.");

  const std::string onDataLoadedBody = ExtractFunctionBody(pluginMain, "void OnDataLoaded(");
  AssertContains(
    onDataLoadedBody,
    "RegisterEventSinks(",
    "Plugin data-loaded path must register event sinks.");

  AssertContains(
    onDataLoadedBody,
    "StartHeartbeatScheduler(",
    "Plugin data-loaded path must start heartbeat scheduler.");

  const std::string initSharedMemoryBody = ExtractFunctionBody(sharedMemory, "bool InitSharedMemory()");
  AssertContains(
    initSharedMemoryBody,
    "g_crashEvent = CreateEventW(",
    "Shared memory init must create crash event.");

  AssertContains(
    initSharedMemoryBody,
    "if (!g_crashEvent)",
    "Shared memory init must fail fast when crash event creation fails.");

  AssertContains(
    initSharedMemoryBody,
    "UnmapViewOfFile(g_shared);",
    "Crash event failure branch must unmap shared memory view.");

  AssertContains(
    initSharedMemoryBody,
    "CloseHandle(g_mapping);",
    "Crash event failure branch must close mapping handle.");

  AssertOrdered(
    initSharedMemoryBody,
    "g_crashEvent = CreateEventW(",
    "if (!g_crashEvent)",
    "Crash event failure branch must appear after event creation.");

  const std::string vectoredHandlerBody = ExtractFunctionBody(crashHandler, "LONG CALLBACK VectoredHandler(");
  AssertContains(
    vectoredHandlerBody,
    "ShouldRecordException(code)",
    "Crash handler must filter exceptions according to hook mode.");

  AssertContains(
    vectoredHandlerBody,
    "InterlockedIncrement(",
    "Crash handler must bump crash_seq for each captured crash signal so recovery does not permanently disable capture.");

  AssertContains(
    vectoredHandlerBody,
    "SetEvent(ev)",
    "Crash handler must signal crash event after recording crash snapshot.");

  assert(
    vectoredHandlerBody.find("InterlockedCompareExchange(") == std::string::npos &&
    "Crash handler must not use one-shot crash_seq latch because it blocks future captures after recovery.");

  return 0;
}
