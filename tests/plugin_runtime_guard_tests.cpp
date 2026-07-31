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
  const auto sharedProtocolPath = repoRoot / "shared" / "SkyrimDiagShared.h";

  assert(std::filesystem::exists(heartbeatPath) && "plugin/src/Heartbeat.cpp not found");
  assert(std::filesystem::exists(resourceHooksPath) && "plugin/src/ResourceHooks.cpp not found");
  assert(std::filesystem::exists(pluginMainPath) && "plugin/src/PluginMain.cpp not found");
  assert(std::filesystem::exists(sharedMemoryPath) && "plugin/src/SharedMemory.cpp not found");
  assert(std::filesystem::exists(crashHandlerPath) && "plugin/src/CrashHandler.cpp not found");
  assert(std::filesystem::exists(sharedProtocolPath) && "shared/SkyrimDiagShared.h not found");

  const std::string heartbeat = ReadAllText(heartbeatPath);
  const std::string resourceHooks = ReadAllText(resourceHooksPath);
  const std::string pluginMain = ReadAllText(pluginMainPath);
  const std::string sharedMemory = ReadAllText(sharedMemoryPath);
  const std::string crashHandler = ReadAllText(crashHandlerPath);
  const std::string sharedProtocol = ReadAllText(sharedProtocolPath);

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

  assert(
    pluginMain.find("static std::jthread g_testHotkeysThread;") == std::string::npos &&
    pluginMain.find("static std::jthread g_watchdogThread;") == std::string::npos &&
    heartbeat.find("std::jthread g_scheduler;") == std::string::npos &&
    "Joining jthread value destructors must never run during DLL detach under loader lock.");

  AssertContains(
    pluginMain,
    "GET_MODULE_HANDLE_EX_FLAG_PIN",
    "Plugin module must be pinned before background workers can outlive a FreeLibrary request.");
  const std::string shutdownWorkersBody = ExtractFunctionBody(
    pluginMain,
    "SkyrimDiagShutdownWorkers() noexcept");
  AssertContains(
    shutdownWorkersBody,
    "StopPluginBackgroundWorkers()",
    "Explicit shutdown must join plugin-owned workers outside DllMain.");
  AssertContains(
    shutdownWorkersBody,
    "StopHeartbeatScheduler()",
    "Explicit shutdown must join the heartbeat worker outside DllMain.");
  const std::string stopHeartbeatBody = ExtractFunctionBody(
    heartbeat,
    "void StopHeartbeatScheduler() noexcept");
  AssertContains(
    stopHeartbeatBody,
    "request_stop()",
    "Heartbeat shutdown must request cooperative worker termination.");
  AssertContains(
    stopHeartbeatBody,
    "join()",
    "Heartbeat shutdown must join before deleting its heap-owned jthread.");

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

  const std::string refreshCrashLoggerRangesBody = ExtractFunctionBody(
    crashHandler,
    "void RefreshCrashLoggerModuleRanges() noexcept");
  AssertContains(
    refreshCrashLoggerRangesBody,
    "QueryLoadedModuleRange(L\"CrashLogger.dll\")",
    "Crash handler must cache the current CrashLogger module image range.");
  AssertContains(
    refreshCrashLoggerRangesBody,
    "QueryLoadedModuleRange(L\"CrashLoggerSSE.dll\")",
    "Crash handler must also cache the legacy CrashLoggerSSE module image range.");
  AssertContains(
    refreshCrashLoggerRangesBody,
    "PublishOnce(",
    "CrashLogger ranges must publish through the write-once helper so the crash handler "
    "never observes a torn range.");

  const std::string installCrashHandlerBody = ExtractFunctionBody(
    crashHandler,
    "bool InstallCrashHandler(");
  AssertContains(
    installCrashHandlerBody,
    "RefreshCrashLoggerModuleRanges()",
    "Crash handler install must cache the CrashLogger module image ranges.");
  AssertOrdered(
    installCrashHandlerBody,
    "RefreshCrashLoggerModuleRanges()",
    "AddVectoredExceptionHandler(",
    "CrashLogger module ranges must be cached before the vectored handler can run.");

  // CrashLogger is an SKSE plugin and can load after us, leaving the install-time
  // lookup empty. Without a lifecycle refresh, nested-fault suppression would be
  // permanently disabled for those load orders.
  const std::string onSkseMessageBody = ExtractFunctionBody(
    pluginMain,
    "void OnSkseMessage(SKSE::MessagingInterface::Message* message)");
  AssertContains(
    onSkseMessageBody,
    "RefreshCrashLoggerModuleRanges()",
    "SKSE lifecycle must refresh CrashLogger module ranges so late-loaded builds are covered.");
  AssertContains(
    onSkseMessageBody,
    "kPostLoad",
    "CrashLogger range refresh must start at the earliest post-load lifecycle stage.");

  AssertContains(
    vectoredHandlerBody,
    "PushFirstChanceExceptionEvent(code, addressBucket, {});",
    "VEH first-chance telemetry must record only the numeric address bucket.");
  assert(
    crashHandler.find("ResolveExceptionModuleBasenameUtf8") == std::string::npos &&
    vectoredHandlerBody.find("GetModuleHandleExW") == std::string::npos &&
    vectoredHandlerBody.find("GetModuleFileNameW") == std::string::npos &&
    vectoredHandlerBody.find("WideCharToMultiByte") == std::string::npos &&
    "VEH must not perform loader, module, path, or text conversion lookups.");

  const std::string publishCrashBody =
    ExtractFunctionBody(crashHandler, "bool TryPublishCrashRecord(");
  const std::string claimCrashBody = ExtractFunctionBody(
    sharedProtocol,
    "inline bool TryClaimCrashIncidentOwnership(");
  AssertContains(
    claimCrashBody,
    "InterlockedCompareExchange(flags, desired, observed)",
    "Incident ownership must be claimed with compare-exchange on the shared state word.");
  AssertContains(
    claimCrashBody,
    "kState_Frozen",
    "Incident claim must reject an already-owned Frozen slot.");
  AssertOrdered(
    publishCrashBody,
    "TryClaimCrashIncidentOwnership(&shm->header.state_flags)",
    "InterlockedCompareExchange(sequence, 0, 0)",
    "Incident ownership must be claimed before CrashInfo seqlock publication begins.");
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
  assert(
    vectoredHandlerBody.find("InterlockedOr(") == std::string::npos &&
    "Fatal handler must not set ownership after publication; the CAS claim is the only ownership point.");

  const std::string setupLogBody = ExtractFunctionBody(pluginMain, "bool SetupLog() noexcept");
  AssertContains(
    setupLogBody,
    "catch (...)",
    "File logger setup must catch every exception before it can escape plugin load.");
  AssertContains(
    setupLogBody,
    "InstallFallbackLogger()",
    "Logger setup failure must install a safe fallback logger.");
  AssertContains(
    pluginMain,
    "spdlog::sinks::null_sink_mt",
    "Fallback logging must use a no-op sink that is safe for later log calls.");

  const std::string pluginLoadBody = ExtractFunctionBody(
    pluginMain,
    "SKSEPluginLoad(");
  AssertContains(
    pluginLoadBody,
    "if (!SetupLog())",
    "Plugin load must explicitly tolerate file logger setup failure.");
  AssertContains(
    pluginLoadBody,
    "catch (...)",
    "Plugin initialization must never unwind through the SKSE C ABI.");
  AssertContains(
    pluginLoadBody,
    "return skseInitialized;",
    "A partial plugin initialization must keep the DLL resident after the SKSE boundary is established.");
  AssertOrdered(
    pluginLoadBody,
    "PinThisModuleForWorkerLifetime()",
    "StartHeartbeatScheduler(",
    "Plugin must pin its module before starting a worker that executes DLL code.");

  const std::string watchdogBody = ExtractFunctionBody(
    pluginMain,
    "void StartHelperWatchdogIfConfigured(");
  AssertContains(
    watchdogBody,
    "unconfirmed; retry in {} ms",
    "An unconfirmed helper launch must be logged as a retryable failure.");
  AssertOrdered(
    watchdogBody,
    "if (confirmed)",
    "retryBackoffMs = kRetryMinMs;",
    "A confirmed helper launch must be the branch that resets watchdog backoff.");
  AssertOrdered(
    watchdogBody,
    "if (confirmed)",
    "nextAttemptTick = 0;",
    "A confirmed helper launch must clear its pending retry deadline.");
  AssertOrdered(
    watchdogBody,
    "unconfirmed; retry in {} ms",
    "retryBackoffMs = std::min(retryBackoffMs * 2, kRetryMaxMs);",
    "An unconfirmed launch must increase watchdog backoff after scheduling its retry.");
  AssertContains(
    watchdogBody,
    "nextAttemptTick = GetTickCount64() + retryBackoffMs;",
    "Unconfirmed and failed launches must schedule a bounded retry delay.");

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
