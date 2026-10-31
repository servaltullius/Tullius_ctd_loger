#include <Windows.h>

#include <shellapi.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <exception>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "CrashCapture.h"
#include "CrashEtwCapture.h"
#include "HelperCommon.h"
#include "HelperLog.h"
#include "HelperMainInternal.h"
#include "HelperRuntimeTestUtils.h"
#include "HookPageRecord.h"
#include "PendingCrashAnalysis.h"
#include "ProcessUtil.h"
#include "RetentionWorker.h"
#include "SkyrimDiagLongPath.h"

using skydiag::helper::HelperConfig;
using skydiag::helper::internal::ClearLog;
using skydiag::helper::internal::CleanupCrashArtifactsAfterZeroExit;
using skydiag::helper::internal::CaptureStableSharedSnapshot;
using skydiag::helper::internal::CrashCaptureState;
using skydiag::helper::internal::ExtractCrashInfo;
using skydiag::helper::internal::HandleCrashEventTick;
using skydiag::helper::internal::MakeOutputBase;
using skydiag::helper::internal::PendingCrashAnalysis;
using skydiag::helper::internal::PendingCrashEtwCapture;
using skydiag::helper::internal::ShutdownRetentionWorker;
using skydiag::helper::internal::StableSharedSnapshot;
using skydiag::helper::internal::TryClearRecoveredCrashFreeze;
using skydiag::helper::internal::NoteCrashLoggerThreadDumpHotkeyPressed;
using skydiag::tests::runtime::AssertContains;
using skydiag::tests::runtime::CloseAttachedProcess;
using skydiag::tests::runtime::FileExists;
using skydiag::tests::runtime::FindSingleFileByPrefix;
using skydiag::tests::runtime::LaunchSleepingChildProcess;
using skydiag::tests::runtime::MakeAttachedProcessForChild;
using skydiag::tests::runtime::MakeSelfAttachedProcess;
using skydiag::tests::runtime::MakeSharedLayout;
using skydiag::tests::runtime::MakeTempDir;
using skydiag::tests::runtime::MakeTestConfig;
using skydiag::tests::runtime::ParkedThread;
using skydiag::tests::runtime::ReadAllTextUtf8;
using skydiag::tests::runtime::Require;
using skydiag::tests::runtime::TerminateChildProcess;
using skydiag::tests::runtime::WriteAllTextUtf8;

namespace {

// Appends a committed Heartbeat event that names `mainTid` as the game's main
// thread (MainThreadIdFromEvent), as the plugin's heartbeat task does.
void AddHeartbeatNamingMainThread(skydiag::SharedLayout* shared, std::uint32_t mainTid)
{
  auto& event = shared->events[shared->header.write_index % skydiag::kEventCapacity];
  event.tid = 40000u;  // a job thread runs the heartbeat task on AE
  event.qpc = shared->header.last_heartbeat_qpc;
  event.type = static_cast<std::uint16_t>(skydiag::EventType::kHeartbeat);
  event.size = static_cast<std::uint16_t>(sizeof(event.payload));
  event.payload.b = mainTid;
  event.seq = shared->header.write_index * 2u;  // committed for its ring index, as the plugin writes it
  shared->header.write_index = shared->header.write_index + 1u;
}

void CheckCrashArtifactsAreWritten(const std::filesystem::path& outBase)
{
  ClearLog(outBase);

  auto shared = MakeSharedLayout();
  shared->header.crash_seq = 2;
  shared->header.crash.exception_code = 0xC0000005u;
  // The faulting thread is a parked worker of this process, and the dump
  // target is this process (see ParkedThread for why not an external child).
  ParkedThread faulting;
  const CONTEXT faultingContext = faulting.Context();
  shared->header.crash.faulting_tid = faulting.tid();
  shared->header.crash.exception_addr = faultingContext.Rip;
  shared->header.crash.exception_record.ExceptionCode = 0xC0000005u;
  shared->header.crash.exception_record.ExceptionAddress = reinterpret_cast<PVOID>(faultingContext.Rip);
  shared->header.crash.context = faultingContext;

  auto proc = MakeSelfAttachedProcess(shared.get());
  proc.crashEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
  Require(proc.crashEvent != nullptr, "CreateEventW failed");

  HelperConfig cfg = MakeTestConfig();
  cfg.autoOpenViewerOnCrash = false;
  cfg.autoAnalyzeDump = false;
  cfg.enableIncidentManifest = true;

  CrashCaptureState crashState{};
  PendingCrashEtwCapture pendingCrashEtw{};
  PendingCrashAnalysis pendingCrashAnalysis{};
  std::wstring lastCrashDumpPath;
  std::wstring pendingHangViewerDumpPath;
  std::wstring pendingCrashViewerDumpPath;

  const bool handled = HandleCrashEventTick(
    cfg,
    proc,
    outBase,
    /*waitMs=*/0,
    &crashState,
    &pendingCrashEtw,
    &pendingCrashAnalysis,
    &lastCrashDumpPath,
    &pendingHangViewerDumpPath,
    &pendingCrashViewerDumpPath);

  Require(handled, "Crash event should be consumed");
  if (!crashState.latched) {
    // Surface the per-attempt dump errors from the helper log.
    std::fprintf(stderr, "%s\n", ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log").c_str());
  }
  Require(crashState.latched, "Crash capture state should flip true");
  Require(
    crashState.capturedInfo.crashSeq == 2u &&
      crashState.capturedInfo.exceptionCode == 0xC0000005u,
    "Crash capture state must retain the immutable crash metadata for exit-time classification");
  Require(!lastCrashDumpPath.empty(), "Crash dump path should be recorded");
  // IsRegularFileForIo: under a long OutputDir these pass MAX_PATH, where MO2's
  // virtual file system answers only through a handle (SkyrimDiagLongPath.h).
  Require(skydiag::IsRegularFileForIo(lastCrashDumpPath), "Crash dump file must exist");
  Require(
    skydiag::IsRegularFileForIo(FindSingleFileByPrefix(outBase, L"SkyrimDiag_Incident_Crash_", L".json")),
    "Crash incident manifest must be written");

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "Crash event signaled", "Crash capture must log crash-event intake");
  AssertContains(log, "Incident manifest written", "Crash capture must log incident manifest creation");
  AssertContains(log, "Crash captured; waiting for process exit.", "Crash capture must log post-capture state");

  ShutdownRetentionWorker();
  CloseHandle(proc.crashEvent);
  proc.crashEvent = nullptr;
  CloseAttachedProcess(&proc);
}

void TestHandleCrashEventTick_WritesCrashArtifacts()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_runtime_smoke");
  CheckCrashArtifactsAreWritten(outBase);
  std::filesystem::remove_all(outBase);
}

// An OutputDir long enough that MakeOutputBase hands out its extended-length
// form: the dump, the incident manifest and the log still land in it.
void TestHandleCrashEventTick_WritesCrashArtifactsUnderALongOutputDir()
{
  const auto root = MakeTempDir(L"skydiag_helper_runtime_long");
  auto longDir = root;
  while (longDir.native().size() < 230u) {
    longDir /= L"a_long_output_folder_segment";
  }
  HelperConfig cfg = MakeTestConfig();
  cfg.outputDir = longDir.wstring();
  const auto outBase = MakeOutputBase(cfg);
  Require(outBase.native().starts_with(LR"(\\?\)"), "a long OutputDir must be used in its extended-length form");
  CheckCrashArtifactsAreWritten(outBase);
  std::error_code ec;
  skydiag::RemoveAllForIo(skydiag::ExtendedLengthPath(root), ec);
}

// The capture files are written with DumpJsonForFile: json::dump()'s default
// throws on invalid UTF-8, which would cost the capture its file (and, with
// nothing above the helper loop catching, the helper).
void TestDumpJsonForFileReplacesInvalidUtf8()
{
  const nlohmann::json j = { { "name", std::string("bad\xFF") } };
  const auto text = skydiag::helper::internal::DumpJsonForFile(j);
  Require(text.find("bad\xEF\xBF\xBD") != std::string::npos, "invalid UTF-8 is replaced, not thrown on");
}

// ADR-0024: a page a later scan no longer finds keeps its last content (a
// freed hook page is what a crash needs), a page over it replaces it, and
// the dump stream holds both.
void TestHookPageRecordKeepsAFreedPage()
{
  using skydiag::helper::internal::RecordedHookPage;
  using skydiag::helper::internal::RecordedHookStub;
  const auto page = [](std::uint64_t base, std::uint64_t size, const wchar_t* module) {
    RecordedHookPage p{};
    p.base = base;
    p.size = size;
    RecordedHookStub stub{};
    stub.length = 14;
    stub.kind = "jmp_abs";
    stub.target = 0x7FFA00001234ull;
    stub.module = module;
    stub.moduleOffset = 0x1234;
    p.stubs.push_back(stub);
    return p;
  };
  skydiag::helper::internal::HookPageRecord record;
  Require(record.ToJson(0).empty(), "no stream before the first scan");
  record.Merge({ page(0x10000, 0x1000, L"Owner.dll"), page(0x20000, 0x1000, L"Other.dll") }, 1000);
  record.Merge({ page(0x20000, 0x1000, L"Other.dll"), RecordedHookPage{ 0x30000, 0x1000 } }, 31000);
  Require(record.Scans() == 2u && record.PageCount() == 2u, "a page without stubs is not recorded");
  auto json = nlohmann::json::parse(record.ToJson(41000));
  Require(json["scans"] == 2 && json["last_scan_ms_ago"] == 10000, "the stream says when it was read");
  Require(json["pages"][0]["base"] == "0x10000" && json["pages"][0]["last_scan"] == 1 &&
            json["pages"][0]["last_seen_ms_ago"] == 40000 && json["pages"][0]["stubs"][0]["module"] == "Owner.dll",
    "the page gone since the first scan keeps its stubs");
  Require(json["pages"][1]["first_scan"] == 1 && json["pages"][1]["last_scan"] == 2, "a page seen again keeps its first scan");

  // Freed and its memory allocated again: a page over it replaces it.
  record.Merge({ page(0xF000, 0x2000, L"Newer.dll") }, 61000);
  json = nlohmann::json::parse(record.ToJson(61000));
  Require(json["pages"].size() == 2u && json["pages"][0]["base"] == "0xF000" &&
            json["pages"][0]["stubs"][0]["module"] == "Newer.dll" && json["pages"][0]["first_scan"] == 3,
    "a page recorded over an old one replaces it");
}

// The scan finds a jump stub in this process's own executable private memory
// and names the module it jumps into.
void TestScanFindsAHookStubInThisProcess()
{
  auto* page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  Require(page != nullptr, "VirtualAlloc failed");
  HMODULE self = GetModuleHandleW(nullptr);
  const auto target = reinterpret_cast<std::uint64_t>(self) + 0x1000u;
  page[100] = 0xFF;
  page[101] = 0x25;
  std::memcpy(page + 106, &target, sizeof(target));

  const auto pages = skydiag::helper::internal::ScanHookPages(GetCurrentProcess());
  VirtualFree(page, 0, MEM_RELEASE);
  wchar_t selfPath[MAX_PATH]{};
  GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
  const auto selfName = std::filesystem::path(selfPath).filename().wstring();
  bool found = false;
  for (const auto& recorded : pages) {
    if (recorded.base != reinterpret_cast<std::uint64_t>(page)) {
      continue;
    }
    found = true;
    Require(recorded.stubs.size() == 1u && recorded.stubs[0].offset == 100u && recorded.stubs[0].target == target,
      "the stub is read where it was written");
    Require(_wcsicmp(recorded.stubs[0].module.c_str(), selfName.c_str()) == 0 && recorded.stubs[0].moduleOffset == 0x1000u,
      "its target is named by module and offset");
  }
  Require(found, "the scan finds the executable page with a stub");
}

void TestWorkingDirectoryForLongFolders()
{
  using skydiag::helper::internal::WorkingDirectoryFor;
  Require(WorkingDirectoryFor(L"C:\\Logs") == L"C:\\Logs", "a usual folder stays the working directory");
  Require(WorkingDirectoryFor(LR"(\\?\C:\Logs)") == L"C:\\Logs", "an extended-length folder is passed plain");
  std::wstring longDir = L"C:\\Logs";
  while (longDir.size() < 260u) {
    longDir += L"\\a_long_output_folder_segment";
  }
  Require(WorkingDirectoryFor(longDir).empty(), "CreateProcessW refuses a working directory this long");
  Require(WorkingDirectoryFor(LR"(\\?\)" + longDir).empty(), "nor in its extended-length form");
}

// Crash Logger's thread dump on its own hotkey (Ctrl+Shift+F12 by default, no
// longer the manual capture hotkey) records a caught C++ exception; while the
// game keeps running no dump is written (rc1/rc2 captures wrote and deleted
// 600 MB each time). No manual capture is involved: the key-state watcher
// notes Crash Logger's hotkey itself.
void TestHandleCrashEventTick_SkipsTheDumpForCrashLoggerThreadDump()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_threaddump_hotkey");
  ClearLog(outBase);

  auto shared = MakeSharedLayout();
  shared->header.qpc_freq = 10'000'000ull;
  shared->header.state_flags = skydiag::kState_Frozen;
  shared->header.crash.exception_code = 0xE06D7363u;
  shared->header.crash.faulting_tid = 12280u;
  shared->header.crash_seq = 2u;
  // The blackbox names another thread as the main thread: a thread dump probe.
  AddHeartbeatNamingMainThread(shared.get(), 25652u);

  auto proc = MakeSelfAttachedProcess(shared.get());
  proc.crashEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
  Require(proc.crashEvent != nullptr, "CreateEventW failed");

  std::atomic_bool stop{ false };
  std::thread game([&] {
    while (!stop.load()) {
      shared->header.last_heartbeat_qpc += 1'000'000ull;
      Sleep(100);
    }
  });

  HelperConfig cfg = MakeTestConfig();
  cfg.autoOpenViewerOnCrash = false;
  cfg.autoAnalyzeDump = false;
  CrashCaptureState crashState{};
  PendingCrashEtwCapture pendingCrashEtw{};
  PendingCrashAnalysis pendingCrashAnalysis{};
  std::wstring lastCrashDumpPath;
  std::wstring pendingHangViewerDumpPath;
  std::wstring pendingCrashViewerDumpPath;
  NoteCrashLoggerThreadDumpHotkeyPressed(GetTickCount64());
  const bool handled = HandleCrashEventTick(
    cfg, proc, outBase, /*waitMs=*/0, &crashState, &pendingCrashEtw, &pendingCrashAnalysis,
    &lastCrashDumpPath, &pendingHangViewerDumpPath, &pendingCrashViewerDumpPath);
  stop.store(true);
  game.join();
  NoteCrashLoggerThreadDumpHotkeyPressed(0);

  Require(handled, "the event is consumed");
  Require(!crashState.latched && lastCrashDumpPath.empty(), "nothing is captured");
  Require(crashState.releasedCrashSeq == 2u, "the exception is released, so the exit path does not report it again");
  // A real worker-thread crash looks the same while Crash Logger writes its
  // log, so an abnormal exit soon after (with the game still live) records it.
  Require(
    crashState.releasedOnBriefHeartbeat && crashState.releasedCrashTick64 != 0u,
    "the release rests on a few seconds of heartbeat");
  bool anyDump = false;
  for (const auto& entry : std::filesystem::directory_iterator(outBase)) {
    anyDump = anyDump || entry.path().extension() == L".dmp";
  }
  Require(!anyDump, "no dump is written");
  Require((shared->header.state_flags & skydiag::kState_Frozen) == 0u, "the capture is thawed for a later CTD");
  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "No dump written", "the reason is logged");
  // rc3 logged "deleting dump" here although no dump had been written.
  Require(log.find("deleting dump") == std::string::npos, "the filter does not claim to delete a dump");

  CloseAttachedProcess(&proc);
  std::filesystem::remove_all(outBase);
}

// A C++ exception on the game's main thread within a minute of Crash Logger's
// thread dump hotkey is not one of its probes (those fault on Crash Logger's
// own threads) but a crash Crash Logger is reporting, which can end the game
// within seconds. Its dump is written at once, not after the heartbeat check.
void TestHandleCrashEventTick_WritesTheDumpAtOnceForAMainThreadFault()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_threaddump_main_thread");
  ClearLog(outBase);

  auto shared = MakeSharedLayout();
  shared->header.state_flags = skydiag::kState_Frozen;
  shared->header.crash_seq = 2u;
  shared->header.crash.exception_code = 0xE06D7363u;
  ParkedThread mainThread;
  const CONTEXT mainContext = mainThread.Context();
  shared->header.crash.faulting_tid = mainThread.tid();
  shared->header.crash.exception_addr = mainContext.Rip;
  shared->header.crash.exception_record.ExceptionCode = 0xE06D7363u;
  shared->header.crash.exception_record.ExceptionAddress = reinterpret_cast<PVOID>(mainContext.Rip);
  shared->header.crash.context = mainContext;
  AddHeartbeatNamingMainThread(shared.get(), mainThread.tid());

  auto proc = MakeSelfAttachedProcess(shared.get());
  proc.crashEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
  Require(proc.crashEvent != nullptr, "CreateEventW failed");

  HelperConfig cfg = MakeTestConfig();
  CrashCaptureState crashState{};
  PendingCrashEtwCapture pendingCrashEtw{};
  PendingCrashAnalysis pendingCrashAnalysis{};
  std::wstring lastCrashDumpPath;
  std::wstring pendingHangViewerDumpPath;
  std::wstring pendingCrashViewerDumpPath;
  NoteCrashLoggerThreadDumpHotkeyPressed(GetTickCount64());
  const bool handled = HandleCrashEventTick(
    cfg, proc, outBase, /*waitMs=*/0, &crashState, &pendingCrashEtw, &pendingCrashAnalysis,
    &lastCrashDumpPath, &pendingHangViewerDumpPath, &pendingCrashViewerDumpPath);
  NoteCrashLoggerThreadDumpHotkeyPressed(0);

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  Require(handled, "the event is consumed");
  if (!crashState.latched) {
    std::fprintf(stderr, "%s\n", log.c_str());
  }
  Require(crashState.latched && FileExists(lastCrashDumpPath), "the crash is captured with its dump");
  Require(crashState.releasedCrashSeq == 0u, "nothing is released");
  AssertContains(log, "on the game's main thread", "skipping the thread dump check is logged");
  // The early check logs one of these whenever it runs.
  Require(log.find("No dump written") == std::string::npos, "the early check did not release it");
  Require(log.find("did not keep running after this exception") == std::string::npos, "the early check did not run");
  Require(log.find("before a crash dump could be written") == std::string::npos, "the early check did not run");

  ShutdownRetentionWorker();
  CloseAttachedProcess(&proc);
  std::filesystem::remove_all(outBase);
}

// A strong fault whose game keeps running is filtered after its dump was
// written: the generation is released so the exit path does not report it
// again, but the dump is held, since an abnormal exit soon after still
// reports the fault (ShouldReportReleasedCrashAtExit) and then needs it.
void TestHandleCrashEventTick_ReleasesARecoveredCrashAfterItsDump()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_recovered_after_dump");
  ClearLog(outBase);

  auto shared = MakeSharedLayout();
  shared->header.state_flags = skydiag::kState_Frozen;
  shared->header.crash_seq = 2u;
  shared->header.crash.exception_code = 0xC0000005u;
  ParkedThread faulting;
  const CONTEXT faultingContext = faulting.Context();
  shared->header.crash.faulting_tid = faulting.tid();
  shared->header.crash.exception_addr = faultingContext.Rip;
  shared->header.crash.exception_record.ExceptionCode = 0xC0000005u;
  shared->header.crash.exception_record.ExceptionAddress = reinterpret_cast<PVOID>(faultingContext.Rip);
  shared->header.crash.context = faultingContext;

  auto proc = MakeSelfAttachedProcess(shared.get());
  proc.crashEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
  Require(proc.crashEvent != nullptr, "CreateEventW failed");

  std::atomic_bool stop{ false };
  std::thread game([&] {
    while (!stop.load()) {
      shared->header.last_heartbeat_qpc += 1'000'000ull;
      Sleep(100);
    }
  });

  HelperConfig cfg = MakeTestConfig();
  CrashCaptureState crashState{};
  PendingCrashEtwCapture pendingCrashEtw{};
  PendingCrashAnalysis pendingCrashAnalysis{};
  std::wstring lastCrashDumpPath;
  std::wstring pendingHangViewerDumpPath;
  std::wstring pendingCrashViewerDumpPath;
  const bool handled = HandleCrashEventTick(
    cfg, proc, outBase, /*waitMs=*/0, &crashState, &pendingCrashEtw, &pendingCrashAnalysis,
    &lastCrashDumpPath, &pendingHangViewerDumpPath, &pendingCrashViewerDumpPath);
  stop.store(true);
  game.join();

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  Require(handled, "the event is consumed");
  if (log.find("dump kept") == std::string::npos) {
    std::fprintf(stderr, "%s\n", log.c_str());
  }
  Require(!crashState.latched && lastCrashDumpPath.empty(), "nothing stays captured");
  Require(crashState.releasedCrashSeq == 2u, "the recovered exception is released");
  // An abnormal exit soon after must still record it.
  Require(
    crashState.releasedOnBriefHeartbeat && crashState.releasedCrashTick64 != 0u,
    "the release rests on a few seconds of heartbeat");
  Require(
    !crashState.releasedDumpPath.empty() && FileExists(crashState.releasedDumpPath),
    "the released dump is held until the release is settled");
  // Held under its own name, so one a stopped helper leaves behind is not
  // listed as a crash.
  Require(
    std::filesystem::path(crashState.releasedDumpPath).filename().wstring().rfind(L"SkyrimDiag_Released_", 0) == 0,
    "the held dump is renamed while it is held");
  Require(
    !FileExists(skydiag::helper::internal::CrashDumpPathForReleased(crashState.releasedDumpPath)),
    "no dump with the crash name is left");
  Require(crashState.releasedDumpRecovery.latchedTick64 != 0u, "its outlived check is running");
  AssertContains(log, "likely a handled first-chance exception", "the filter logs its evidence");
  AssertContains(log, "Keeping the released dump", "the caller logs what happened to the dump");
  AssertContains(log, "Filtered crash event consumed; dump kept", "the caller logs what happened to the dump");
  Require(log.find("deleting dump") == std::string::npos, "the filter itself does not log the dump's fate");

  // A newer crash event supersedes the release: its held dump goes.
  const std::wstring heldDump = crashState.releasedDumpPath;
  skydiag::helper::internal::DiscardReleasedCrashDump(outBase, &crashState, L"test");
  Require(crashState.releasedDumpPath.empty() && !FileExists(heldDump), "a discarded held dump is deleted");

  CloseAttachedProcess(&proc);
  std::filesystem::remove_all(outBase);
}

// A handled fault that keeps coming back (same code and address, released
// twice within the cooldown) gets the minimal dump first, not a full-size one
// each time. Kept as a crash while the game still runs, it is written again
// at the configured size. Not on the game's main thread: unhandled there, it
// ends the game before a second dump could be written.
void TestHandleCrashEventTick_RepeatedHandledFaultGetsTheMinimalDumpFirst()
{
  struct Case
  {
    bool gameKeepsRunning;
    bool onMainThread;
  };
  for (const auto [gameKeepsRunning, onMainThread] : { Case{ true, false }, Case{ false, false }, Case{ true, true } }) {
    const auto outBase = MakeTempDir(L"skydiag_helper_repeated_fault");
    ClearLog(outBase);

    auto shared = MakeSharedLayout();
    shared->header.state_flags = skydiag::kState_Frozen;
    shared->header.crash_seq = 2u;
    shared->header.crash.exception_code = 0xC0000005u;
    ParkedThread faulting;
    const CONTEXT faultingContext = faulting.Context();
    shared->header.crash.faulting_tid = faulting.tid();
    shared->header.crash.exception_addr = faultingContext.Rip;
    shared->header.crash.exception_record.ExceptionCode = 0xC0000005u;
    shared->header.crash.exception_record.ExceptionAddress = reinterpret_cast<PVOID>(faultingContext.Rip);
    shared->header.crash.context = faultingContext;
    if (onMainThread) {
      AddHeartbeatNamingMainThread(shared.get(), faulting.tid());
    }

    auto proc = MakeSelfAttachedProcess(shared.get());
    proc.crashEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    Require(proc.crashEvent != nullptr, "CreateEventW failed");

    std::atomic_bool stop{ false };
    std::thread game([&, keepsRunning = gameKeepsRunning] {
      while (keepsRunning && !stop.load()) {
        shared->header.last_heartbeat_qpc += 1'000'000ull;
        Sleep(100);
      }
    });

    HelperConfig cfg = MakeTestConfig();
    CrashCaptureState crashState{};
    const auto now = GetTickCount64();
    skydiag::helper::internal::NoteRecoveredFault(&crashState.recoveredFaults, 0xC0000005u, faultingContext.Rip, now);
    skydiag::helper::internal::NoteRecoveredFault(&crashState.recoveredFaults, 0xC0000005u, faultingContext.Rip, now);
    PendingCrashEtwCapture pendingCrashEtw{};
    PendingCrashAnalysis pendingCrashAnalysis{};
    std::wstring lastCrashDumpPath;
    std::wstring pendingHangViewerDumpPath;
    std::wstring pendingCrashViewerDumpPath;
    const bool handled = HandleCrashEventTick(
      cfg, proc, outBase, /*waitMs=*/0, &crashState, &pendingCrashEtw, &pendingCrashAnalysis,
      &lastCrashDumpPath, &pendingHangViewerDumpPath, &pendingCrashViewerDumpPath);
    stop.store(true);
    game.join();

    const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
    Require(handled, "the event is consumed");
    if (onMainThread) {
      Require(log.find("writing a minimal dump first") == std::string::npos, "a main-thread fault gets its full dump");
    } else {
      AssertContains(log, "writing a minimal dump first", "the smaller first dump is logged");
    }
    if (gameKeepsRunning) {
      Require(!crashState.latched && FileExists(crashState.releasedDumpPath), "released again, its dump held");
    } else {
      if (log.find("rewrote its dump at the configured size") == std::string::npos) {
        std::fprintf(stderr, "%s\n", log.c_str());
      }
      Require(crashState.latched && FileExists(lastCrashDumpPath), "kept as a crash with its dump");
      AssertContains(log, "rewrote its dump at the configured size", "the kept crash gets the configured dump size");
      Require(!FileExists(lastCrashDumpPath + L".full.tmp"), "no temporary dump is left");
      // ADR-0023: the manifest describes the dump as rewritten, not the minimal first one.
      const auto manifest = ReadAllTextUtf8(FindSingleFileByPrefix(outBase, L"SkyrimDiag_Incident_Crash_", L".json"));
      AssertContains(manifest, "\"base_mode\": \"default\"", "the manifest records the configured dump mode");
      AssertContains(manifest, "\"code_segments_scope\": \"game_and_stacks\"", "and the code it holds");
    }

    ShutdownRetentionWorker();
    CloseAttachedProcess(&proc);
    std::filesystem::remove_all(outBase);
  }
}

void TestRecoveredCrashThaw_AllowsStableFollowupSnapshot()
{
  auto shared = MakeSharedLayout();
  shared->header.crash_seq = 2u;
  shared->header.crash.exception_code = 0xC0000005u;
  shared->header.crash.exception_addr = 0x1111u;
  shared->header.crash.faulting_tid = 101u;
  shared->header.state_flags = skydiag::kState_Frozen;

  StableSharedSnapshot first{};
  Require(
    CaptureStableSharedSnapshot(shared.get(), sizeof(*shared), &first),
    "Committed crash record must produce a stable helper snapshot");
  Require(
    (reinterpret_cast<std::uintptr_t>(first.layout()) % alignof(skydiag::SharedLayout)) == 0u,
    "Stable helper snapshot storage must satisfy SharedLayout alignment");
  const auto firstInfo = ExtractCrashInfo(&first.layout()->header);
  Require(firstInfo.crashSeq == 2u, "Stable snapshot must retain its committed crash sequence");
  Require(firstInfo.exceptionAddr == 0x1111u, "Stable snapshot must retain the first crash record");

  Require(
    TryClearRecoveredCrashFreeze(shared.get(), firstInfo.crashSeq),
    "Recovered first-chance exception must thaw the matching frozen record");
  Require(
    (shared->header.state_flags & skydiag::kState_Frozen) == 0u,
    "Recovered exception must resume blackbox/resource telemetry");

  // Model the next committed crash. The old immutable snapshot must stay
  // unchanged while a new stable snapshot observes only the follow-up record.
  shared->header.crash_seq = 3u;
  shared->header.crash.exception_code = 0xC000001Du;
  shared->header.crash.exception_addr = 0x2222u;
  shared->header.crash.faulting_tid = 202u;
  shared->header.state_flags |= skydiag::kState_Frozen;
  MemoryBarrier();
  shared->header.crash_seq = 4u;

  Require(
    ExtractCrashInfo(&first.layout()->header).exceptionAddr == 0x1111u,
    "Previously captured snapshot must remain immutable after a follow-up crash");

  StableSharedSnapshot second{};
  Require(
    CaptureStableSharedSnapshot(shared.get(), sizeof(*shared), &second),
    "Follow-up crash must produce a new stable snapshot after recovery");
  const auto secondInfo = ExtractCrashInfo(&second.layout()->header);
  Require(secondInfo.crashSeq == 4u, "Follow-up snapshot must expose the new committed sequence");
  Require(secondInfo.exceptionAddr == 0x2222u, "Follow-up snapshot must not mix the prior crash record");
  Require(secondInfo.faultingTid == 202u, "Follow-up snapshot must retain one coherent crash record");
}

void TestStableSnapshot_PerEntrySeqlocksRejectTornRingData()
{
  auto shared = MakeSharedLayout();
  shared->header.crash_seq = 2u;
  shared->header.crash.exception_code = 0xC0000005u;
  shared->events[0].seq = 3u;
  shared->events[0].payload.a = 0xAAAAAAAAAAAAAAAAull;
  shared->resources.entries[0].seq = 5u;
  shared->resources.entries[0].path_hash = 0xBBBBBBBBBBBBBBBBull;

  StableSharedSnapshot unstableSnapshot{};
  Require(
    CaptureStableSharedSnapshot(shared.get(), sizeof(*shared), &unstableSnapshot),
    "An unstable ring entry must not invalidate the coherent crash snapshot");
  // The source entries are odd already, so a plain copy would also be odd:
  // the snapshot must hold the cleared invalid marker (seq 1, no payload).
  const auto& unstableEvent = unstableSnapshot.layout()->events[0];
  Require(
    unstableEvent.seq == 1u && unstableEvent.payload.a == 0u,
    "Overlapping blackbox entry must be marked invalid instead of copied torn");
  const auto& unstableResource = unstableSnapshot.layout()->resources.entries[0];
  Require(
    unstableResource.seq == 1u && unstableResource.path_hash == 0u,
    "Overlapping resource entry must be marked invalid instead of copied torn");

  std::atomic<bool> stop{false};
  std::thread writer([&]() {
    std::uint32_t generation = 1u;
    while (!stop.load(std::memory_order_relaxed)) {
      auto& event = shared->events[1];
      const std::uint32_t eventCommitted = generation * 2u;
      InterlockedExchange(
        reinterpret_cast<volatile LONG*>(&event.seq),
        static_cast<LONG>(eventCommitted | 1u));
      event.payload.a = generation;
      event.payload.b = ~static_cast<std::uint64_t>(generation);
      event.qpc = static_cast<std::uint64_t>(generation) * 3u;
      MemoryBarrier();
      InterlockedExchange(
        reinterpret_cast<volatile LONG*>(&event.seq),
        static_cast<LONG>(eventCommitted));

      auto& resource = shared->resources.entries[1];
      const std::uint32_t resourceCommitted = generation * 2u;
      InterlockedExchange(
        reinterpret_cast<volatile LONG*>(&resource.seq),
        static_cast<LONG>(resourceCommitted | 1u));
      resource.path_hash = generation;
      resource.qpc = static_cast<std::uint64_t>(generation) * 7u;
      MemoryBarrier();
      InterlockedExchange(
        reinterpret_cast<volatile LONG*>(&resource.seq),
        static_cast<LONG>(resourceCommitted));
      ++generation;
    }
  });

  for (int i = 0; i < 64; ++i) {
    StableSharedSnapshot snapshot{};
    Require(
      CaptureStableSharedSnapshot(shared.get(), sizeof(*shared), &snapshot),
      "Concurrent ring writer must not destabilize the committed crash record");

    const auto& event = snapshot.layout()->events[1];
    if ((event.seq & 1u) == 0u && event.seq != 0u) {
      Require(
        event.payload.b == ~event.payload.a &&
          event.qpc == event.payload.a * 3u,
        "Committed blackbox snapshot entry must come from one writer generation");
    }
    const auto& resource = snapshot.layout()->resources.entries[1];
    if ((resource.seq & 1u) == 0u && resource.seq != 0u) {
      Require(
        resource.qpc == resource.path_hash * 7u,
        "Committed resource snapshot entry must come from one writer generation");
    }
  }

  stop.store(true, std::memory_order_relaxed);
  writer.join();
}

void TestHandleCrashEventTick_RejectsUncommittedCrashSequenceBeforeDump()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_runtime_zero_seq");
  ClearLog(outBase);

  auto shared = MakeSharedLayout();
  shared->header.crash_seq = 0u;
  shared->header.crash.exception_code = 0xC0000005u;
  shared->header.crash.exception_addr = 0x1234u;
  shared->header.state_flags = skydiag::kState_Frozen;

  auto child = LaunchSleepingChildProcess();
  auto proc = MakeAttachedProcessForChild(child, shared.get());
  proc.crashEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
  Require(proc.crashEvent != nullptr, "CreateEventW failed");

  HelperConfig cfg = MakeTestConfig();
  CrashCaptureState crashState{};
  PendingCrashEtwCapture pendingCrashEtw{};
  PendingCrashAnalysis pendingCrashAnalysis{};
  std::wstring lastCrashDumpPath;
  std::wstring pendingHangViewerDumpPath;
  std::wstring pendingCrashViewerDumpPath;

  const bool handled = HandleCrashEventTick(
    cfg,
    proc,
    outBase,
    /*waitMs=*/0,
    &crashState,
    &pendingCrashEtw,
    &pendingCrashAnalysis,
    &lastCrashDumpPath,
    &pendingHangViewerDumpPath,
    &pendingCrashViewerDumpPath);

  Require(!handled, "Zero crash sequence must be rejected before dump capture");
  Require(!crashState.latched, "Rejected crash sequence must not set capture latch");
  Require(lastCrashDumpPath.empty(), "Rejected crash sequence must not expose a dump path");

  shared->header.crash_seq = 1u;
  Require(SetEvent(proc.crashEvent) != FALSE, "SetEvent failed for odd-sequence check");
  const bool oddHandled = HandleCrashEventTick(
    cfg,
    proc,
    outBase,
    /*waitMs=*/0,
    &crashState,
    &pendingCrashEtw,
    &pendingCrashAnalysis,
    &lastCrashDumpPath,
    &pendingHangViewerDumpPath,
    &pendingCrashViewerDumpPath);
  Require(!oddHandled, "Odd crash sequence must be rejected before dump capture");
  Require(!crashState.latched, "Odd crash sequence must not set capture latch");
  Require(lastCrashDumpPath.empty(), "Odd crash sequence must not expose a dump path");

  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(outBase, ec)) {
    Require(!ec, "Failed to enumerate zero-sequence output directory");
    Require(entry.path().extension() != L".dmp", "Zero crash sequence must not write a dump file");
  }
  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "crash_seq is not a non-zero committed sequence", "Rejected sequence must be logged");

  CloseHandle(proc.crashEvent);
  proc.crashEvent = nullptr;
  TerminateChildProcess(&child);
  std::filesystem::remove_all(outBase);
}

void TestCleanupCrashArtifactsAfterZeroExit_RemovesHandledStrongCrashArtifacts()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_runtime_preserve");
  ClearLog(outBase);

  const auto dumpPath = outBase / "SkyrimDiag_Crash_20260315_120000_001.dmp";
  WriteAllTextUtf8(dumpPath, "dump");

  HelperConfig cfg = MakeTestConfig();
  skydiag::helper::AttachedProcess proc{};
  skydiag::helper::internal::HelperLoopState state{};
  state.crashCaptured.latched = true;
  state.capturedCrashDumpPath = dumpPath.wstring();
  state.pendingCrashViewerDumpPath = dumpPath.wstring();

  CleanupCrashArtifactsAfterZeroExit(cfg, proc, outBase, &state);

  Require(!FileExists(dumpPath), "Handled strong exception with exit_code=0 must remove artifacts");
  Require(!state.crashCaptured.latched, "Handled strong exception with exit_code=0 must clear capture state");
  Require(state.pendingCrashViewerDumpPath.empty(), "Handled strong exception with exit_code=0 must suppress viewer");

  const auto log = ReadAllTextUtf8(outBase / "SkyrimDiagHelper.log");
  AssertContains(log, "removed", "Handled strong zero-exit exception must be logged as filtered");

  std::filesystem::remove_all(outBase);
}

}  // namespace

// The helper log used to be truncated on every attach, so relaunching the game
// after a crash erased the log users are told to read.
void TestHelperLogSessionKeepsThePreviousLog()
{
  const auto outBase = MakeTempDir(L"skydiag_helper_log_session");
  skydiag::helper::internal::AppendLogLine(outBase, L"crash session line");
  skydiag::helper::internal::StartHelperLogSession(outBase);
  skydiag::helper::internal::AppendLogLine(outBase, L"next session line");

  const auto previous = ReadAllTextUtf8(outBase / L"SkyrimDiagHelper.previous.log");
  const auto current = ReadAllTextUtf8(outBase / L"SkyrimDiagHelper.log");
  std::error_code ec;
  std::filesystem::remove_all(outBase, ec);
  Require(previous.find("crash session line") != std::string::npos, "the previous session's log must be kept");
  Require(current.find("crash session line") == std::string::npos, "the new session starts a fresh log");
  Require(current.find("next session line") != std::string::npos, "the new session writes to the current log");
}

// v0.2.68-rc5 field case (2026-10-21): two relaunches after a session that
// exited abnormally overwrote its log, since only previous.log was kept.
void TestHelperLogSessionKeepsSeveralEarlierSessions()
{
  using skydiag::helper::internal::AppendLogLine;
  using skydiag::helper::internal::HelperSessionLogPath;
  using skydiag::helper::internal::SetHelperSessionLogsToKeep;
  using skydiag::helper::internal::StartHelperLogSession;
  const auto outBase = MakeTempDir(L"skydiag_helper_log_sessions");
  SetHelperSessionLogsToKeep(3);
  for (int session = 1; session <= 5; ++session) {
    StartHelperLogSession(outBase);
    AppendLogLine(outBase, L"session " + std::to_wstring(session));
  }
  const auto current = ReadAllTextUtf8(outBase / L"SkyrimDiagHelper.log");
  const auto age1 = ReadAllTextUtf8(HelperSessionLogPath(outBase, 1));
  const auto age2 = ReadAllTextUtf8(HelperSessionLogPath(outBase, 2));
  const auto age3 = ReadAllTextUtf8(HelperSessionLogPath(outBase, 3));
  std::error_code ec;
  const bool age4Exists = std::filesystem::exists(HelperSessionLogPath(outBase, 4), ec);
  std::filesystem::remove_all(outBase, ec);
  SetHelperSessionLogsToKeep(skydiag::helper::internal::kDefaultHelperSessionLogsToKeep);
  Require(current.find("session 5") != std::string::npos, "the current session writes to the current log");
  Require(age1.find("session 4") != std::string::npos, "the last session is previous.log");
  Require(age2.find("session 3") != std::string::npos, "the one before is previous.2.log");
  Require(age3.find("session 2") != std::string::npos, "the oldest kept session is previous.3.log");
  Require(!age4Exists, "sessions beyond the limit are removed");
  Require(HelperSessionLogPath(outBase, 1).filename() == L"SkyrimDiagHelper.previous.log", "previous.log keeps its name");
}

// v0.2.68-rc4 built the Crashlog Directory path from CrashLogger.ini's bytes
// as UTF-8, which throws on a value saved in the ANSI code page (e.g. CP949
// from Notepad), and the helper died at every start before its capture loop.
void TestCrashLoggerIniPathDecodesWithoutThrowing()
{
  using skydiag::helper::internal::DecodeCrashLoggerIniPath;
  Require(DecodeCrashLoggerIniPath("").empty(), "an unset value is no folder");
  Require(DecodeCrashLoggerIniPath("Logs") == std::filesystem::path(L"Logs"), "ASCII decodes as is");

  // D:\<game>\<log> in Hangul, as UTF-8 and as CP949.
  const std::wstring expected = L"D:\\\uAC8C\uC784\\\uB85C\uADF8";
  const std::string utf8 = "D:\\\xEA\xB2\x8C\xEC\x9E\x84\\\xEB\xA1\x9C\xEA\xB7\xB8";
  const std::string cp949 = "D:\\\xB0\xD4\xC0\xD3\\\xB7\xCE\xB1\xD7";
  Require(DecodeCrashLoggerIniPath(utf8) == std::filesystem::path(expected), "UTF-8 is read as Crash Logger reads it");

  bool threw = false;
  std::filesystem::path decoded;
  try {
    decoded = DecodeCrashLoggerIniPath(cp949);
  } catch (...) {
    threw = true;
  }
  Require(!threw, "bytes that are not UTF-8 must not throw");
  if (GetACP() == 949u) {
    Require(decoded == std::filesystem::path(expected), "the ANSI code page is the fallback");
  } else {
    Require(decoded.empty() || decoded.wstring().rfind(L"D:\\", 0) == 0, "a fallback decode keeps the plain bytes");
  }
}

// QuoteArg values read back as written through Windows' own command-line
// parsing: a trailing backslash (OutputDir=D:\, a share root), embedded
// quotes, and backslashes before them.
void TestQuoteArgRoundTripsThroughCommandLineToArgvW()
{
  const std::wstring values[] = {
    L"D:\\",
    L"\\\\server\\share\\",
    L"C:\\Program Files\\Skyrim\\SkyrimDiagDumpToolCli.exe",
    L"a\"b",
    L"a\\\"b",
    L"trailing\\\\",
    L"",
  };
  for (const auto& value : values) {
    const std::wstring commandLine = L"x.exe " + skydiag::helper::internal::QuoteArg(value) + L" --headless";
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(commandLine.c_str(), &argc);
    Require(argv != nullptr, "CommandLineToArgvW failed");
    const bool ok = argc == 3 && value == argv[1] && std::wstring(argv[2]) == L"--headless";
    LocalFree(argv);
    Require(ok, "a quoted value must read back as one argument, followed by the next");
  }
}

int main()
{
  try {
    TestQuoteArgRoundTripsThroughCommandLineToArgvW();
    TestDumpJsonForFileReplacesInvalidUtf8();
    TestHookPageRecordKeepsAFreedPage();
    TestScanFindsAHookStubInThisProcess();
    TestCrashLoggerIniPathDecodesWithoutThrowing();
    TestWorkingDirectoryForLongFolders();
    TestHandleCrashEventTick_WritesCrashArtifacts();
    TestHandleCrashEventTick_WritesCrashArtifactsUnderALongOutputDir();
    TestHandleCrashEventTick_SkipsTheDumpForCrashLoggerThreadDump();
    TestHandleCrashEventTick_WritesTheDumpAtOnceForAMainThreadFault();
    TestHandleCrashEventTick_ReleasesARecoveredCrashAfterItsDump();
    TestHandleCrashEventTick_RepeatedHandledFaultGetsTheMinimalDumpFirst();
    TestRecoveredCrashThaw_AllowsStableFollowupSnapshot();
    TestStableSnapshot_PerEntrySeqlocksRejectTornRingData();
    TestHandleCrashEventTick_RejectsUncommittedCrashSequenceBeforeDump();
    TestCleanupCrashArtifactsAfterZeroExit_RemovesHandledStrongCrashArtifacts();
    TestHelperLogSessionKeepsThePreviousLog();
    TestHelperLogSessionKeepsSeveralEarlierSessions();
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
