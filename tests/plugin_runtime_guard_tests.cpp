#include <cassert>
#include <filesystem>
#include <string>

#include "SkyrimDiag/CrashHandler.h"
#include "SourceGuardTestUtils.h"

using skydiag::tests::source_guard::AssertContains;
using skydiag::tests::source_guard::AssertOrdered;
using skydiag::tests::source_guard::ExtractFunctionBody;
using skydiag::tests::source_guard::ReadAllText;

int main()
{
  using skydiag::plugin::CrashHandlerModuleRange;
  using skydiag::plugin::ShouldSuppressNestedCrashLoggerException;

  constexpr CrashHandlerModuleRange crashLoggerRange{0x1000u, 0x2000u};
  constexpr CrashHandlerModuleRange crashLoggerSseRange{0x3000u, 0x4000u};
  static_assert(!ShouldSuppressNestedCrashLoggerException(
    false, 0x1800u, crashLoggerRange, crashLoggerSseRange));
  static_assert(ShouldSuppressNestedCrashLoggerException(
    true, 0x1800u, crashLoggerRange, crashLoggerSseRange));
  static_assert(ShouldSuppressNestedCrashLoggerException(
    true, 0x3800u, crashLoggerRange, crashLoggerSseRange));
  static_assert(!ShouldSuppressNestedCrashLoggerException(
    true, 0x2000u, crashLoggerRange, crashLoggerSseRange));
  static_assert(!ShouldSuppressNestedCrashLoggerException(
    true, 0x5000u, crashLoggerRange, crashLoggerSseRange));

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

  AssertContains(
    heartbeat,
    "kLifecyclePollIntervalMs = 1000",
    "Lifecycle module/thread enumeration must use a slower cadence than the heartbeat.");
  const std::string schedulerLoopBody = ExtractFunctionBody(heartbeat, "void SchedulerLoop(");
  AssertContains(
    schedulerLoopBody,
    "catch (...)",
    "Optional lifecycle polling failures must not terminate the heartbeat scheduler.");

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

  AssertContains(
    pluginMain,
    "static std::jthread g_testHotkeysThread;",
    "Test hotkey worker must keep a managed jthread so DLL unload can request shutdown.");

  AssertContains(
    pluginMain,
    "stop_requested()",
    "Test hotkey worker must observe stop requests instead of spinning forever.");

  assert(
    pluginMain.find("}).detach();") == std::string::npos &&
    "Test hotkey worker must not detach because detached DLL threads outlive unload.");

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
    "ShouldSuppressNestedCrashLoggerException(",
    "CrashLogger introspection exceptions must not replace an already frozen CTD.");

  AssertContains(
    vectoredHandlerBody,
    "TryPublishCrashRecord(shm, ep, code)",
    "Crash handler must publish a stable fixed-size crash record before signaling the helper.");

  AssertContains(
    vectoredHandlerBody,
    "SetEvent(ev)",
    "Crash handler must signal crash event after recording crash snapshot.");

  AssertOrdered(
    vectoredHandlerBody,
    "ShouldSuppressNestedCrashLoggerException(",
    "TryPublishCrashRecord(shm, ep, code)",
    "Nested CrashLogger suppression must run before publishing a replacement crash record.");

  AssertOrdered(
    vectoredHandlerBody,
    "TryPublishCrashRecord(shm, ep, code)",
    "SetEvent(ev)",
    "Crash record publication must precede helper signaling.");

  AssertOrdered(
    vectoredHandlerBody,
    "SetEvent(ev)",
    "ShouldEmitFirstChanceTelemetry(ep->ExceptionRecord)",
    "Fatal capture must return after signaling before dynamic first-chance telemetry can execute.");

  const std::string installCrashHandlerBody = ExtractFunctionBody(
    crashHandler,
    "bool InstallCrashHandler(");
  AssertContains(
    installCrashHandlerBody,
    "QueryLoadedModuleRange(L\"CrashLogger.dll\")",
    "Crash handler install must cache the current CrashLogger module image range.");
  AssertContains(
    installCrashHandlerBody,
    "QueryLoadedModuleRange(L\"CrashLoggerSSE.dll\")",
    "Crash handler install must also cache the legacy CrashLoggerSSE module image range.");
  AssertOrdered(
    installCrashHandlerBody,
    "QueryLoadedModuleRange(L\"CrashLogger.dll\")",
    "AddVectoredExceptionHandler(",
    "CrashLogger module ranges must be cached before the vectored handler can run.");

  AssertOrdered(
    vectoredHandlerBody,
    "ConsumeFirstChanceTelemetryBudget(signature, qpcNow)",
    "ResolveExceptionModuleBasenameUtf8(",
    "First-chance rate limiting must run before module/path resolution.");

  const std::string publishCrashBody = ExtractFunctionBody(crashHandler, "bool TryPublishCrashRecord(");
  AssertContains(
    publishCrashBody,
    "InterlockedCompareExchange(sequence, writing, observed)",
    "Crash record writer must acquire the odd seqlock state atomically.");
  AssertContains(
    publishCrashBody,
    "InterlockedExchange(sequence, committed)",
    "Crash record writer must publish an even committed sequence.");
  assert(
    publishCrashBody.find("std::string") == std::string::npos &&
    publishCrashBody.find("std::filesystem") == std::string::npos &&
    publishCrashBody.find("ResolveExceptionModuleBasenameUtf8") == std::string::npos &&
    "Fatal crash record publication must not allocate or resolve filesystem paths.");

  AssertContains(
    pluginMain,
    "GetModuleFileNameW(mod, buf.data(), static_cast<DWORD>(buf.size()))",
    "Plugin path resolution must use a dynamic buffer for long install paths.");

  AssertContains(
    pluginMain,
    "GetPrivateProfileStringW(",
    "Plugin config loader must still read helper path from ini.");

  AssertContains(
    pluginMain,
    "ReadIniUint32Clamped(",
    "Plugin config loader must clamp numeric INI values before casting to uint32_t.");

  AssertContains(
    pluginMain,
    "HeartbeatIntervalMs",
    "Plugin config clamp helper must cover heartbeat interval.");

  assert(
    pluginMain.find("GetModuleFileNameW(mod, buf, MAX_PATH)") == std::string::npos &&
    "Plugin path resolution must not use MAX_PATH-sized fixed buffers.");

  return 0;
}
