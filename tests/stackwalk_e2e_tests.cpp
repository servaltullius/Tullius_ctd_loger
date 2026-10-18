#include <Windows.h>
#include <Psapi.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "Analyzer.h"
#include "AnalyzerInternalsStackwalkPriv.h"
#include "HangCaptureInternal.h"
#include "HelperLog.h"
#include "HelperRuntimeTestUtils.h"
#include "RetentionWorker.h"

using skydiag::helper::HangDecision;
using skydiag::helper::internal::ClearLog;
using skydiag::helper::internal::ExecuteConfirmedHangCapture;
using skydiag::helper::internal::HangCaptureState;
using skydiag::helper::internal::ShutdownRetentionWorker;
using skydiag::tests::runtime::CloseAttachedProcess;
using skydiag::tests::runtime::FindSingleFileWithExt;
using skydiag::tests::runtime::MakeSelfAttachedProcess;
using skydiag::tests::runtime::MakeSharedLayout;
using skydiag::tests::runtime::MakeTempDir;
using skydiag::tests::runtime::MakeTestConfig;
using skydiag::tests::runtime::Require;
using skydiag::tests::runtime::WriteAllTextUtf8;

namespace {

HANDLE g_release = nullptr;
HANDLE g_parked = nullptr;
std::atomic<int> g_sink{ 0 };

// noinline plus work after each call keeps every function as a real frame
// (no inlining, no tail call) in the RelWithDebInfo build.
__declspec(noinline) void DeepFrameC()
{
  SetEvent(g_parked);
  WaitForSingleObject(g_release, 30000);
  g_sink.fetch_add(3);
}

__declspec(noinline) void DeepFrameB()
{
  DeepFrameC();
  g_sink.fetch_add(2);
}

__declspec(noinline) void DeepFrameA()
{
  DeepFrameB();
  g_sink.fetch_add(1);
}

std::size_t FindFrame(const std::vector<std::wstring>& frames, const wchar_t* name)
{
  for (std::size_t i = 0; i < frames.size(); ++i) {
    if (frames[i].find(name) != std::wstring::npos) {
      return i;
    }
  }
  return frames.size();
}

// A dump of a thread parked three calls deep must unwind through every caller
// rather than stopping at the wait syscall or guessing stack slots.
void TestFormalStackwalkUnwindsRealCallerChain()
{
  const auto outBase = MakeTempDir(L"skydiag_stackwalk_e2e");
  ClearLog(outBase);

  g_release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_parked = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Require(g_release && g_parked, "CreateEventW failed");

  DWORD parkedTid = 0;
  std::thread parked([&]() {
    parkedTid = GetCurrentThreadId();
    DeepFrameA();
  });
  Require(WaitForSingleObject(g_parked, 5000) == WAIT_OBJECT_0, "Worker never reached the wait");
  Sleep(100);

  // The blackbox names the parked thread as the game main thread.
  auto shared = MakeSharedLayout();
  shared->header.last_heartbeat_qpc = 1;
  auto& sessionStart = shared->events[0];
  sessionStart.tid = parkedTid;
  sessionStart.qpc = 2;
  sessionStart.type = static_cast<std::uint16_t>(skydiag::EventType::kSessionStart);
  sessionStart.seq = 2;
  shared->header.write_index = 1;

  auto proc = MakeSelfAttachedProcess(shared.get());
  HangDecision decision{};
  decision.isHang = true;
  decision.secondsSinceHeartbeat = 30.0;
  decision.thresholdSec = 10;
  std::wstring pendingViewer;
  HangCaptureState state{};
  ExecuteConfirmedHangCapture(MakeTestConfig(), proc, outBase, decision, 0u, &pendingViewer, &state);

  SetEvent(g_release);
  parked.join();
  ShutdownRetentionWorker();
  CloseAttachedProcess(&proc);
  CloseHandle(g_release);
  CloseHandle(g_parked);

  const auto dumpPath = FindSingleFileWithExt(outBase, L".dmp");
  Require(std::filesystem::exists(dumpPath), "Hang capture must write a dump");

  skydiag::dump_tool::AnalyzeOptions opt{};
  opt.language = skydiag::dump_tool::i18n::Language::kEnglish;
  opt.output_dir = outBase.wstring();
  skydiag::dump_tool::AnalysisResult result{};
  std::wstring err;
  Require(
    skydiag::dump_tool::AnalyzeDump(dumpPath.wstring(), outBase.wstring(), opt, result, &err),
    "AnalyzeDump failed on the stackwalk dump");

  Require(result.stackwalk_primary_tid == parkedTid, "The main thread must be the walked thread");
  Require(result.suspects_from_stackwalk, "Suspects must come from the formal stackwalk, not pointer scan");
  Require(result.stackwalk_total_frames >= 5u, "Formal stackwalk must unwind past the wait syscall");

  const auto& frames = result.stackwalk_primary_frames;
  const auto c = FindFrame(frames, L"DeepFrameC");
  const auto b = FindFrame(frames, L"DeepFrameB");
  const auto a = FindFrame(frames, L"DeepFrameA");
  Require(c < frames.size() && b < frames.size() && a < frames.size(), "Every caller in the chain must appear");
  Require(c < b && b < a, "Callers must appear innermost first");
  for (const auto& frame : frames) {
    Require(frame.rfind(L"0x", 0) != 0, "Walked frames must all belong to a loaded module");
  }

  std::filesystem::remove_all(outBase);
}

// A Crash Logger thread dump paired with a hang dump lists every thread's call
// stack. Its module tally must not rank the stack suspects or become evidence
// (2026-10-18 hang: plugins seen on idle threads were listed as the top
// modules); the pairing itself stays.
void TestThreadDumpPairingNamesNoSuspects()
{
  const auto outBase = MakeTempDir(L"skydiag_threaddump_pairing_e2e");
  ClearLog(outBase);

  g_release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_parked = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Require(g_release && g_parked, "CreateEventW failed");

  DWORD parkedTid = 0;
  std::thread parked([&]() {
    parkedTid = GetCurrentThreadId();
    DeepFrameA();
  });
  Require(WaitForSingleObject(g_parked, 5000) == WAIT_OBJECT_0, "Worker never reached the wait");
  Sleep(100);

  auto shared = MakeSharedLayout();
  shared->header.last_heartbeat_qpc = 1;
  auto& sessionStart = shared->events[0];
  sessionStart.tid = parkedTid;
  sessionStart.qpc = 2;
  sessionStart.type = static_cast<std::uint16_t>(skydiag::EventType::kSessionStart);
  sessionStart.seq = 2;
  shared->header.write_index = 1;

  auto proc = MakeSelfAttachedProcess(shared.get());
  HangDecision decision{};
  decision.isHang = true;
  decision.secondsSinceHeartbeat = 30.0;
  decision.thresholdSec = 10;
  std::wstring pendingViewer;
  HangCaptureState state{};
  ExecuteConfirmedHangCapture(MakeTestConfig(), proc, outBase, decision, 0u, &pendingViewer, &state);

  SetEvent(g_release);
  parked.join();
  ShutdownRetentionWorker();
  CloseAttachedProcess(&proc);
  CloseHandle(g_release);
  CloseHandle(g_parked);

  const auto dumpPath = FindSingleFileWithExt(outBase, L".dmp");
  Require(std::filesystem::exists(dumpPath), "Hang capture must write a dump");

  // Crash Logger's thread dump from the same second, beside the dump. The test
  // executable heads it: it is also on the walked thread's stack.
  const std::wstring stem = dumpPath.stem().wstring();  // SkyrimDiag_Hang_YYYYMMDD_HHMMSS_mmm
  const std::wstring prefix = L"SkyrimDiag_Hang_";
  Require(stem.rfind(prefix, 0) == 0 && stem.size() >= prefix.size() + 15u, "Unexpected hang dump name");
  const std::wstring ts = stem.substr(prefix.size(), 15u);
  const std::wstring logName = L"threaddump-" + ts.substr(0, 4) + L"-" + ts.substr(4, 2) + L"-" + ts.substr(6, 2) +
                               L"-" + ts.substr(9, 2) + L"-" + ts.substr(11, 2) + L"-" + ts.substr(13, 2) + L".log";
  wchar_t exePath[MAX_PATH]{};
  Require(GetModuleFileNameW(nullptr, exePath, MAX_PATH) != 0, "GetModuleFileNameW failed");
  const std::string exeName = std::filesystem::path(exePath).filename().string();
  WriteAllTextUtf8(
    outBase / logName,
    "========================================\n"
    "THREAD DUMP (Manual Trigger)\n"
    "========================================\n"
    "TIME: 2097-07-13 12:00:00\n"
    "CrashLoggerSSE v1-25-0-0\n"
    "Total Threads: 2\n"
    "\n"
    "===== THREAD 1 (ID: 1111) =====\n"
    "\tCALLSTACK:\n"
    "\t[ 0] 0x7FF612340001 " + exeName + "+0000001\n"
    "\t[ 1] 0x7FF612340002 " + exeName + "+0000002\n"
    "\t[ 2] 0x7FF612340003 ThreadDumpBystander.dll+0000003\n"
    "\n"
    "===== THREAD 2 (ID: 2222) =====\n"
    "\tCALLSTACK:\n"
    "\t[ 0] 0x7FF612340004 ThreadDumpBystander.dll+0000004\n"
    "\n");

  skydiag::dump_tool::AnalyzeOptions opt{};
  opt.language = skydiag::dump_tool::i18n::Language::kEnglish;
  opt.output_dir = outBase.wstring();
  skydiag::dump_tool::AnalysisResult result{};
  std::wstring err;
  Require(
    skydiag::dump_tool::AnalyzeDump(dumpPath.wstring(), outBase.wstring(), opt, result, &err),
    "AnalyzeDump failed on the thread-dump pairing dump");

  Require(
    std::filesystem::path(result.crash_logger_log_path).filename() == std::filesystem::path(logName),
    "The thread dump must still pair with the hang dump");
  Require(result.crash_logger_pairing_selected_kind == "thread_dump", "Pairing must record the thread-dump kind");
  Require(result.crash_logger_top_modules.empty(), "A thread dump must supply no Crash Logger top modules");
  Require(!result.suspects.empty(), "The walked thread's stack must still give suspects");
  for (const auto& suspect : result.suspects) {
    Require(
      suspect.reason.find(L"Crash Logger") == std::wstring::npos,
      "A thread dump must not promote a stack suspect");
  }
  for (const auto& evidence : result.evidence) {
    Require(
      evidence.title.find(L"top callstack modules") == std::wstring::npos,
      "A thread dump must not appear as top-module evidence");
    Require(
      evidence.details.find(L"ThreadDumpBystander.dll") == std::wstring::npos,
      "A module seen only in the thread dump must not appear in the evidence");
  }
  for (const auto& recommendation : result.recommendations) {
    Require(
      recommendation.find(L"ThreadDumpBystander.dll") == std::wstring::npos,
      "A module seen only in the thread dump must not appear in the advice");
  }

  std::filesystem::remove_all(outBase);
}

}  // namespace

// A recovered return address has to point into code. The local-image check
// must reject a data address (a constant in .rdata) and accept a function.
void TestLocalImageRejectsNonCodeReturnAddresses()
{
  static const char kReadOnlyData[] = "skydiag stackwalk rdata probe";

  const HMODULE self = GetModuleHandleW(nullptr);
  MODULEINFO info{};
  Require(GetModuleInformation(GetCurrentProcess(), self, &info, sizeof(info)) != 0, "GetModuleInformation failed");
  wchar_t path[MAX_PATH]{};
  Require(GetModuleFileNameW(self, path, MAX_PATH) != 0, "GetModuleFileNameW failed");
  const auto* view = reinterpret_cast<const std::uint8_t*>(self);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(view);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(view + dos->e_lfanew);

  std::vector<skydiag::dump_tool::minidump::ModuleInfo> modules(1);
  modules[0].base = reinterpret_cast<std::uint64_t>(info.lpBaseOfDll);
  modules[0].end = modules[0].base + info.SizeOfImage;
  modules[0].time_date_stamp = nt->FileHeader.TimeDateStamp;
  modules[0].path = path;

  const skydiag::dump_tool::internal::stackwalk_internal::LocalImageMemory images(modules);
  Require(
    images.IsKnownNonCodeAddress(reinterpret_cast<std::uint64_t>(&kReadOnlyData[0])),
    "a read-only data address must be rejected as a return address");
  Require(
    !images.IsKnownNonCodeAddress(reinterpret_cast<std::uint64_t>(&DeepFrameB)),
    "a function address must be accepted");
  Require(
    !images.IsKnownNonCodeAddress(modules[0].end + 0x1000u),
    "an address outside every module cannot be judged");
}

// FindFunctionStart names a frame only by the exact start of its function.
// A chained .pdata entry is a fragment of another function (for example a
// cold block moved away from it) and must lead back to that function.
void TestFindFunctionStartFollowsChainedEntries()
{
  constexpr std::uint64_t kBase = 0x140000000ull;
  constexpr std::size_t kSize = 0x4000;
  std::vector<std::uint8_t> image(kSize);
  auto put = [&](std::size_t at, const void* src, std::size_t n) { std::memcpy(image.data() + at, src, n); };

  IMAGE_DOS_HEADER dos{};
  dos.e_magic = IMAGE_DOS_SIGNATURE;
  dos.e_lfanew = 0x80;
  put(0, &dos, sizeof(dos));
  IMAGE_NT_HEADERS64 nt{};
  nt.Signature = IMAGE_NT_SIGNATURE;
  nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
  nt.OptionalHeader.SizeOfImage = static_cast<DWORD>(kSize);
  nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION] = { 0x1000, 3 * sizeof(RUNTIME_FUNCTION) };
  put(0x80, &nt, sizeof(nt));

  // .pdata: a function, a chained fragment of it, and an indirect entry.
  const RUNTIME_FUNCTION entries[3] = {
    { 0x2000, 0x2100, 0x3000 },
    { 0x2200, 0x2280, 0x3010 },
    { 0x2400, 0x2480, 0x1000 | 1u },
  };
  put(0x1000, entries, sizeof(entries));
  const std::uint8_t primary[4] = { 0x01, 0, 0, 0 };                          // version 1, no flags
  const std::uint8_t chained[4] = { 0x01 | (UNW_FLAG_CHAININFO << 3), 0, 0, 0 };  // no unwind codes
  put(0x3000, primary, sizeof(primary));
  put(0x3010, chained, sizeof(chained));
  put(0x3014, &entries[0], sizeof(RUNTIME_FUNCTION));

  std::vector<skydiag::dump_tool::minidump::ModuleInfo> modules(1);
  modules[0].base = kBase;
  modules[0].end = kBase + kSize;
  modules[0].filename = L"SkyrimSE.exe";
  modules[0].is_game_exe = true;

  namespace sw = skydiag::dump_tool::internal::stackwalk_internal;
  sw::MinidumpMemoryView mem;
  mem.ranges.push_back({ kBase, kBase + kSize, image.data() });
  const sw::DumpImageData images(mem, modules);
  mem.dump_images = &images;

  Require(sw::FindFunctionStart(mem, kBase, kBase + 0x2050) == kBase + 0x2000, "a primary entry starts its function");
  Require(sw::FindFunctionStart(mem, kBase, kBase + 0x2250) == kBase + 0x2000, "a chained fragment leads to its function");
  Require(sw::FindFunctionStart(mem, kBase, kBase + 0x2410) == kBase + 0x2000, "an indirect entry leads to its target");
  Require(!sw::FindFunctionStart(mem, kBase, kBase + 0x2150).has_value(), "a leaf function has no start");
}

int main()
{
  try {
    TestFindFunctionStartFollowsChainedEntries();
    TestLocalImageRejectsNonCodeReturnAddresses();
    TestFormalStackwalkUnwindsRealCallerChain();
    TestThreadDumpPairingNamesNoSuspects();
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
