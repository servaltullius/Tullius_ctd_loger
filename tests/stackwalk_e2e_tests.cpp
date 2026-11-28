#include <Windows.h>
#include <DbgHelp.h>
#include <Psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "Analyzer.h"
#include "AnalyzerInternals.h"
#include "AnalyzerInternalsStackwalkPriv.h"
#include "AnalyzerPipeline.h"
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

// ADR-0039: a real dispatch of an access violation, with a second exception
// raised inside the vectored handler that received it and dumped from there,
// as a crash handler would. The CONTEXT and EXCEPTION_RECORD are wherever this
// Windows puts them.
constexpr std::uintptr_t kNestedFaultAddress = 0x10;
constexpr DWORD kNestedRaisedCode = 0xE0391234u;
std::filesystem::path g_nestedDumpPath;
bool g_nestedDumpWritten = false;
DWORD g_nestedThreadId = 0;

LONG WriteNestedDump(EXCEPTION_POINTERS* ep, DWORD threadId = GetCurrentThreadId())
{
  const HANDLE file =
    CreateFileW(g_nestedDumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = threadId;
    info.ExceptionPointers = ep;
    info.ClientPointers = FALSE;
    g_nestedDumpWritten = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                            static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules), &info,
                            nullptr, nullptr) != FALSE;
    CloseHandle(file);
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

__declspec(noinline) void RaiseInsideHandler()
{
  __try {
    RaiseException(kNestedRaisedCode, 0, 0, nullptr);
  } __except (WriteNestedDump(GetExceptionInformation())) {
    g_sink.fetch_add(5);
  }
}

LONG CALLBACK FirstSeeingHandler(EXCEPTION_POINTERS* ep)
{
  const auto* record = ep->ExceptionRecord;
  if (GetCurrentThreadId() == g_nestedThreadId && !g_nestedDumpWritten &&
      record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2u &&
      record->ExceptionInformation[1] == kNestedFaultAddress) {
    RaiseInsideHandler();
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

// A crash handler that has the dump of the exception it received written by
// another thread and waits, as the plugin waits for the helper: the faulting
// thread's own context then waits inside the handler, above the dispatcher
// of that same exception.
constexpr std::uintptr_t kOwnFaultAddress = 0x20;

LONG CALLBACK DumpingHandler(EXCEPTION_POINTERS* ep)
{
  const auto* record = ep->ExceptionRecord;
  if (GetCurrentThreadId() == g_nestedThreadId && !g_nestedDumpWritten &&
      record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2u &&
      record->ExceptionInformation[1] == kOwnFaultAddress) {
    const DWORD faultingThread = GetCurrentThreadId();
    std::thread writer([ep, faultingThread] { (void)WriteNestedDump(ep, faultingThread); });
    writer.join();
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

// Removes the exception's own context from a dump (its location reads as
// empty), as when the analyzer cannot read it.
void DropExceptionContext(const std::filesystem::path& dumpPath)
{
  std::vector<char> bytes;
  {
    std::ifstream in(dumpPath, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  Require(bytes.size() >= sizeof(MINIDUMP_HEADER), "dump header");
  MINIDUMP_HEADER header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  bool dropped = false;
  for (ULONG32 i = 0; i < header.NumberOfStreams; ++i) {
    const std::size_t at = header.StreamDirectoryRva + i * sizeof(MINIDUMP_DIRECTORY);
    Require(at + sizeof(MINIDUMP_DIRECTORY) <= bytes.size(), "stream directory");
    MINIDUMP_DIRECTORY entry{};
    std::memcpy(&entry, bytes.data() + at, sizeof(entry));
    if (entry.StreamType != ExceptionStream) {
      continue;
    }
    Require(entry.Location.Rva + sizeof(MINIDUMP_EXCEPTION_STREAM) <= bytes.size(), "exception stream");
    MINIDUMP_EXCEPTION_STREAM stream{};
    std::memcpy(&stream, bytes.data() + entry.Location.Rva, sizeof(stream));
    stream.ThreadContext.DataSize = 0;
    stream.ThreadContext.Rva = 0;
    std::memcpy(bytes.data() + entry.Location.Rva, &stream, sizeof(stream));
    dropped = true;
  }
  Require(dropped, "the dump has an exception stream");
  std::ofstream out(dumpPath, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

__declspec(noinline) int ReadThroughBadPointer(volatile int* p)
{
  const int value = *p;
  g_sink.fetch_add(value);
  return value;
}

__declspec(noinline) bool FaultUnderTheDumpingHandler()
{
  __try {
    ReadThroughBadPointer(reinterpret_cast<volatile int*>(kOwnFaultAddress));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return true;
  }
  return false;
}

__declspec(noinline) bool FaultUnderTheHandler()
{
  __try {
    ReadThroughBadPointer(reinterpret_cast<volatile int*>(kNestedFaultAddress));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return true;
  }
  return false;
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
  sessionStart.seq = 0;  // ring index 0 committed (index * 2)
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
  sessionStart.seq = 0;  // ring index 0 committed (index * 2)
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

// A request the dump covers only in part (its window around a stack value
// ends inside it): the dump's bytes first, the rest from the matching local
// file, as for any address the dump lacks. ReadDump stays the dump's own
// memory (the game's code is encrypted in its local file).
void TestPartialDumpReadIsCompletedFromTheLocalImage()
{
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

  namespace sw = skydiag::dump_tool::internal::stackwalk_internal;
  const sw::LocalImageMemory images(modules);
  const auto code = reinterpret_cast<std::uint64_t>(&DeepFrameB);
  std::uint8_t fromFile[8]{};
  std::size_t fileGot = 0;
  Require(images.Read(code + 8, fromFile, sizeof(fromFile), fileGot) && fileGot == sizeof(fromFile),
    "the local file has the code");

  // The dump holds the first 8 bytes only, marked so their source shows.
  std::uint8_t dumpBytes[8];
  std::memset(dumpBytes, 0xAB, sizeof(dumpBytes));
  sw::MinidumpMemoryView mem;
  mem.ranges.push_back({ code, code + sizeof(dumpBytes), dumpBytes });
  mem.image_fallback = &images;

  std::uint8_t out[16]{};
  std::size_t got = 0;
  Require(mem.Read(code, out, sizeof(out), got) && got == sizeof(out), "the whole request is read");
  Require(std::memcmp(out, dumpBytes, sizeof(dumpBytes)) == 0, "the dump's bytes come first");
  Require(std::memcmp(out + 8, fromFile, sizeof(fromFile)) == 0, "the rest comes from the local file");

  got = 0;
  Require(mem.ReadDump(code, out, sizeof(out), got) && got == sizeof(dumpBytes), "ReadDump reads the dump alone");
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

// A shared dump whose Memory64List claims 2^60 ranges: count * 16 wraps to 0,
// so a multiplied size check passed and reserve() threw out of the analysis.
// The list must be rejected like any other that does not fit its stream.
void TestMemory64ListCountOverflowIsRejected()
{
  // Header, one directory entry, then the list: its count and base RVA (the
  // SDK declares MemoryRanges as a zero-length array) and one descriptor.
  constexpr std::size_t kListRva = sizeof(MINIDUMP_HEADER) + sizeof(MINIDUMP_DIRECTORY);
  constexpr std::size_t kListBytes = 2 * sizeof(ULONG64) + sizeof(MINIDUMP_MEMORY_DESCRIPTOR64);
  std::vector<std::uint8_t> dump(kListRva + kListBytes);
  MINIDUMP_HEADER header{};
  header.Signature = MINIDUMP_SIGNATURE;
  header.Version = MINIDUMP_VERSION;
  header.NumberOfStreams = 1;
  header.StreamDirectoryRva = sizeof(MINIDUMP_HEADER);
  MINIDUMP_DIRECTORY directory{};
  directory.StreamType = Memory64ListStream;
  directory.Location.DataSize = static_cast<ULONG32>(kListBytes);
  directory.Location.Rva = static_cast<RVA>(kListRva);
  const ULONG64 rangeCount = 1ull << 60;
  const ULONG64 baseRva = dump.size();
  MINIDUMP_MEMORY_DESCRIPTOR64 range{};
  range.StartOfMemoryRange = 0x10000;
  std::memcpy(dump.data(), &header, sizeof(header));
  std::memcpy(dump.data() + sizeof(header), &directory, sizeof(directory));
  std::memcpy(dump.data() + kListRva, &rangeCount, sizeof(rangeCount));
  std::memcpy(dump.data() + kListRva + sizeof(rangeCount), &baseRva, sizeof(baseRva));
  std::memcpy(dump.data() + kListRva + 2 * sizeof(ULONG64), &range, sizeof(range));

  namespace sw = skydiag::dump_tool::internal::stackwalk_internal;
  sw::MinidumpMemoryView mem;
  const bool loaded = mem.Init(dump.data(), dump.size(), /*threads=*/nullptr);
  Require(!loaded && mem.ranges.empty(), "a Memory64List with an impossible range count is rejected");
}

// ADR-0034: a crash walk with only the game and Windows on it skips the
// pointer scan; an overlay's hook DLL on the path, a frame outside every
// module, a freeze, or another thread's walk keeps it.
void TestCompleteGameWalkSkipsStackScan()
{
  using skydiag::dump_tool::internal::ChooseStackScan;
  using skydiag::dump_tool::internal::StackScanChoice;
  using skydiag::dump_tool::internal::stackwalk::CompleteWalkWithoutPluginFrames;
  using skydiag::dump_tool::internal::stackwalk::MarkThreadEntryHookPcs;
  using skydiag::dump_tool::minidump::ModuleInfo;

  const auto module = [](std::uint64_t base, const wchar_t* name) {
    ModuleInfo m{};
    m.base = base;
    m.end = base + 0x100000;
    m.filename = name;
    return m;
  };
  auto game = module(0x140000000ull, L"SkyrimSE.exe");
  game.is_game_exe = true;
  auto overlay = module(0x7FF800000000ull, L"RTSSHooks64.dll");
  overlay.external_software = L"RivaTuner Statistics Server";
  overlay.is_systemish = true;
  auto kernel32 = module(0x7FF900000000ull, L"KERNEL32.DLL");
  kernel32.is_systemish = true;
  auto ntdll = module(0x7FFA00000000ull, L"ntdll.dll");
  ntdll.is_systemish = true;
  const std::vector<ModuleInfo> modules = { game, overlay, kernel32, ntdll };  // sorted by base

  const std::vector<std::uint64_t> gameOnly = {
    0x140001000ull, 0x140002000ull, 0x140003000ull, 0x140004000ull, 0x7FF900001000ull, 0x7FFA00001000ull,
  };
  const auto marks = [&](const std::vector<std::uint64_t>& pcs) { return MarkThreadEntryHookPcs(modules, pcs, true); };
  Require(CompleteWalkWithoutPluginFrames(modules, gameOnly, marks(gameOnly), true), "the game and Windows only");
  Require(!CompleteWalkWithoutPluginFrames(modules, gameOnly, marks(gameOnly), false), "a walk cut short");
  auto throughOverlay = gameOnly;
  throughOverlay.insert(throughOverlay.begin() + 2, 0x7FF800001000ull);
  Require(!CompleteWalkWithoutPluginFrames(modules, throughOverlay, marks(throughOverlay), true),
    "an overlay's hook DLL on the path");
  auto outsideModules = gameOnly;
  outsideModules.insert(outsideModules.begin() + 2, 0x500000000ull);
  Require(!CompleteWalkWithoutPluginFrames(modules, outsideModules, marks(outsideModules), true),
    "a frame outside every module");

  // A walk with a thread-entry hook AND an unmapped trampoline frame marks the
  // entry hook (3 game frames directly above it) but is NOT complete-without-plugin-frames.
  const auto plugin = module(0x7FF700000000ull, L"Trails.dll");
  const std::vector<ModuleInfo> modulesWithPlugin = { game, plugin, overlay, kernel32, ntdll };
  auto hookPlusUnmapped = gameOnly;
  hookPlusUnmapped.insert(hookPlusUnmapped.begin() + 3, 0x7FF700001000ull);  // thread entry hook
  hookPlusUnmapped.insert(hookPlusUnmapped.begin(), 0x500000000ull);         // unmapped frame above the 3 game frames
  const auto hookUnmappedMarks = MarkThreadEntryHookPcs(modulesWithPlugin, hookPlusUnmapped, true);
  Require(
    std::find(hookUnmappedMarks.begin(), hookUnmappedMarks.end(), true) != hookUnmappedMarks.end(),
    "entry hook frame is marked");
  Require(
    !CompleteWalkWithoutPluginFrames(modulesWithPlugin, hookPlusUnmapped, hookUnmappedMarks, true),
    "unmapped frame must prevent treating walk as complete without plugin frames");

  skydiag::dump_tool::AnalysisResult crash{};
  crash.exc_tid = 41920u;
  crash.stackwalk_primary_tid = 41920u;
  crash.stackwalk_complete_without_plugin_frames = true;
  Require(ChooseStackScan(crash, /*hangLike=*/false) == StackScanChoice::kSkipCompleteWalk, "skipped for the crash");
  Require(ChooseStackScan(crash, /*hangLike=*/true) == StackScanChoice::kScan, "a freeze keeps its scan");
  auto otherThread = crash;
  otherThread.stackwalk_primary_tid = 47080u;
  Require(ChooseStackScan(otherThread, false) == StackScanChoice::kScan, "another thread's walk");
  auto withPlugin = crash;
  withPlugin.stackwalk_complete_without_plugin_frames = false;
  Require(ChooseStackScan(withPlugin, false) == StackScanChoice::kScan, "a plugin on the path");
  auto entryHooks = withPlugin;
  entryHooks.stackwalk_only_thread_entry_hook_plugins = true;
  Require(ChooseStackScan(entryHooks, true) == StackScanChoice::kSkipThreadEntryHooksOnly,
    "entry hooks only, freezes too (ADR-0015)");
  auto named = crash;
  named.suspects_from_stackwalk = true;
  Require(ChooseStackScan(named, false) == StackScanChoice::kScan, "nothing to skip when the walk named suspects");
}

void TestDetermineHangLikeIncludesWctDeadlockCycles()
{
  skydiag::dump_tool::AnalysisResult manualDeadlock{};
  manualDeadlock.has_wct = true;
  manualDeadlock.wct_json_utf8 =
    R"({"capture":{"kind":"manual","secondsSinceHeartbeat":0.2,"thresholdSec":10},"threads":[{"tid":100,"isCycle":true}]})";
  Require(
    skydiag::dump_tool::DetermineHangLike(/*nameHang=*/false, manualDeadlock),
    "manual capture with WCT deadlock cycle must be treated as hang-like");

  skydiag::dump_tool::AnalysisResult manualHealthy{};
  manualHealthy.has_wct = true;
  manualHealthy.wct_json_utf8 =
    R"({"capture":{"kind":"manual","secondsSinceHeartbeat":0.2,"thresholdSec":10},"threads":[{"tid":100,"isCycle":false}]})";
  Require(
    !skydiag::dump_tool::DetermineHangLike(/*nameHang=*/false, manualHealthy),
    "manual capture without cycle or stall must remain snapshot-like");
}

void TestFindThreadsWithNearStackModulePrefersMatchingPathOverSystemCopy()
{
  // Build a minimal in-memory minidump containing one ThreadListStream entry
  // whose stack holds an address inside the game-folder proxy d3d11.dll.
  struct FakeDump
  {
    MINIDUMP_HEADER header{};
    MINIDUMP_DIRECTORY dir{};
    ULONG32 threadCount = 1;
    MINIDUMP_THREAD thread{};
    CONTEXT ctx{};
    std::uint64_t stackSlots[2]{};
  } dump{};
  dump.header.Signature = MINIDUMP_SIGNATURE;
  dump.header.NumberOfStreams = 1;
  dump.header.StreamDirectoryRva = static_cast<RVA>(offsetof(FakeDump, dir));
  dump.dir.StreamType = ThreadListStream;
  dump.dir.Location.DataSize = sizeof(ULONG32) + sizeof(MINIDUMP_THREAD);
  dump.dir.Location.Rva = static_cast<RVA>(offsetof(FakeDump, threadCount));

  dump.thread.ThreadId = 4242u;
  dump.thread.Stack.StartOfMemoryRange = 0x000000100000ull;
  dump.thread.Stack.Memory.DataSize = sizeof(dump.stackSlots);
  dump.thread.Stack.Memory.Rva = static_cast<RVA>(offsetof(FakeDump, stackSlots));
  dump.thread.ThreadContext.DataSize = sizeof(CONTEXT);
  dump.thread.ThreadContext.Rva = static_cast<RVA>(offsetof(FakeDump, ctx));
  dump.ctx.Rsp = 0x000000100000ull;
  dump.stackSlots[0] = 0x0000000200000100ull;

  skydiag::dump_tool::minidump::ModuleInfo sysCopy{};
  sysCopy.base = 0x0000000100000000ull;
  sysCopy.end = sysCopy.base + 0x10000u;
  sysCopy.filename = L"d3d11.dll";
  sysCopy.path = L"C:\\Windows\\System32\\d3d11.dll";
  sysCopy.is_systemish = true;

  skydiag::dump_tool::minidump::ModuleInfo proxyCopy{};
  proxyCopy.base = 0x0000000200000000ull;
  proxyCopy.end = proxyCopy.base + 0x10000u;
  proxyCopy.filename = L"d3d11.dll";
  proxyCopy.path = L"G:\\Game\\d3d11.dll";
  proxyCopy.is_systemish = false;

  const std::vector<skydiag::dump_tool::minidump::ModuleInfo> modules = { sysCopy, proxyCopy };

  const auto byPath = skydiag::dump_tool::internal::FindThreadsWithNearStackModule(
    &dump, sizeof(dump), modules, L"d3d11.dll", 16, L"G:\\Game\\d3d11.dll");
  Require(byPath.size() == 1u && byPath[0] == 4242u, "exact module_path must select the proxy d3d11.dll");

  const auto preferNonSys = skydiag::dump_tool::internal::FindThreadsWithNearStackModule(
    &dump, sizeof(dump), modules, L"d3d11.dll", 16);
  Require(
    preferNonSys.size() == 1u && preferNonSys[0] == 4242u,
    "without module_path, non-systemish copy must be preferred over System32 copy");

  const auto sysExplicit = skydiag::dump_tool::internal::FindThreadsWithNearStackModule(
    &dump, sizeof(dump), modules, L"d3d11.dll", 16, L"C:\\Windows\\System32\\d3d11.dll");
  Require(sysExplicit.empty(), "explicit System32 path must not match proxy addresses");
}

void TestEarlierExceptionFromARealDispatch()
{
  const auto outBase = MakeTempDir(L"skydiag_earlier_exception_e2e");
  g_nestedDumpPath = outBase / L"SkyrimDiag_Crash_20261128_100235_374.dmp";
  g_nestedDumpWritten = false;
  g_nestedThreadId = GetCurrentThreadId();
  const PVOID handler = AddVectoredExceptionHandler(1, FirstSeeingHandler);
  Require(handler != nullptr, "AddVectoredExceptionHandler failed");
  const bool caught = FaultUnderTheHandler();
  RemoveVectoredExceptionHandler(handler);
  g_nestedThreadId = 0;
  Require(caught, "the access violation reaches its own __except after the handler");
  Require(g_nestedDumpWritten, "the dump was written from inside the handler");

  wchar_t exePath[MAX_PATH]{};
  Require(GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0, "GetModuleFileNameW failed");
  const std::wstring exeName = std::filesystem::path(exePath).filename().wstring();

  skydiag::dump_tool::AnalyzeOptions opt{};
  opt.language = skydiag::dump_tool::i18n::Language::kEnglish;
  opt.output_dir = outBase.wstring();
  opt.use_crash_logger = false;
  skydiag::dump_tool::AnalysisResult result{};
  std::wstring err;
  Require(skydiag::dump_tool::AnalyzeDump(g_nestedDumpPath.wstring(), outBase.wstring(), opt, result, &err),
    "AnalyzeDump failed on the nested exception dump");

  Require(result.exc_code == kNestedRaisedCode, "the reported exception is the one raised in the handler");
  const auto& earlier = result.earlier_exception;
  Require(earlier.detected, "the access violation being handled is found on the stack");
  Require(earlier.code == EXCEPTION_ACCESS_VIOLATION, "an access violation");
  Require(earlier.info.size() == 2u && earlier.info[0] == 0u && earlier.info[1] == kNestedFaultAddress,
    "a read of 0x10");
  Require(_wcsicmp(earlier.module_filename.c_str(), exeName.c_str()) == 0, "in this test's own module");
  Require(_wcsicmp(earlier.handler_module_filename.c_str(), exeName.c_str()) == 0, "and so is the handler");
  Require(earlier.dispatcher_depth >= 2u, "the dispatcher is below the handler's frames");
  Require(earlier.record_address > earlier.context_address, "the record after the CONTEXT");
  Require(!result.suspects.empty() && _wcsicmp(result.suspects[0].module_filename.c_str(), exeName.c_str()) == 0,
    "the faulting module leads");
  // The handler is in the same module here, so only the reason shows the
  // suspects were scored from the earlier exception's call stack.
  Require(result.suspects[0].reason.find(L"exception being handled") != std::wstring::npos,
    "scored from the earlier exception's call stack");
  Require(!earlier.frames.empty() && earlier.frames[0].find(L"ReadThroughBadPointer") != std::wstring::npos,
    "its own call stack starts where it faulted");
  Require(earlier.handler_only_modules.empty(), "the handler's module is also below the dispatcher");

  std::filesystem::remove_all(outBase);
}

// The crash handler's own dispatch is no earlier exception: the walk that
// finds one starts at the exception's context, never at the thread's current
// one, which waits in the handler above the dispatcher of the reported
// exception itself (used when the exception's context cannot be read).
void TestCrashHandlersOwnDispatchIsNoEarlierException()
{
  const auto outBase = MakeTempDir(L"skydiag_own_dispatch_e2e");
  g_nestedDumpPath = outBase / L"SkyrimDiag_Crash_20261128_100236_001.dmp";
  g_nestedDumpWritten = false;
  g_nestedThreadId = GetCurrentThreadId();
  const PVOID handler = AddVectoredExceptionHandler(1, DumpingHandler);
  Require(handler != nullptr, "AddVectoredExceptionHandler failed");
  const bool caught = FaultUnderTheDumpingHandler();
  RemoveVectoredExceptionHandler(handler);
  g_nestedThreadId = 0;
  Require(caught && g_nestedDumpWritten, "the handler wrote the dump of the access violation");

  const auto analyze = [&]() {
    skydiag::dump_tool::AnalyzeOptions opt{};
    opt.language = skydiag::dump_tool::i18n::Language::kEnglish;
    opt.output_dir = outBase.wstring();
    opt.use_crash_logger = false;
    skydiag::dump_tool::AnalysisResult result{};
    std::wstring err;
    Require(skydiag::dump_tool::AnalyzeDump(g_nestedDumpPath.wstring(), outBase.wstring(), opt, result, &err),
      "AnalyzeDump failed on the crash handler's dump");
    Require(result.exc_code == EXCEPTION_ACCESS_VIOLATION, "the reported exception");
    return result;
  };
  const auto withContext = analyze();
  Require(!withContext.earlier_exception.detected, "nothing earlier from the exception's context");

  DropExceptionContext(g_nestedDumpPath);
  const auto withoutContext = analyze();
  const auto& shown = withoutContext.ShownCallstackFrames();
  if (!std::any_of(shown.begin(), shown.end(),
        [](const std::wstring& f) { return f.find(L"DumpingHandler") != std::wstring::npos; })) {
    std::fwprintf(stderr, L"walk without the exception's context (tid %u, %zu frames):\n",
      withoutContext.stackwalk_primary_tid, shown.size());
    for (const auto& f : shown) {
      std::fwprintf(stderr, L"  %ls\n", f.c_str());
    }
    for (const auto& d : withoutContext.diagnostics) {
      std::fwprintf(stderr, L"  diag: %ls\n", d.c_str());
    }
    Require(false, "without it the walk starts in the handler");
  }
  Require(!withoutContext.earlier_exception.detected, "and the reported exception is not taken for an earlier one");

  std::filesystem::remove_all(outBase);
}

int main()
{
  try {
    TestEarlierExceptionFromARealDispatch();
    TestCrashHandlersOwnDispatchIsNoEarlierException();
    TestCompleteGameWalkSkipsStackScan();
    TestDetermineHangLikeIncludesWctDeadlockCycles();
    TestFindThreadsWithNearStackModulePrefersMatchingPathOverSystemCopy();
    TestMemory64ListCountOverflowIsRejected();
    TestFindFunctionStartFollowsChainedEntries();
    TestLocalImageRejectsNonCodeReturnAddresses();
    TestPartialDumpReadIsCompletedFromTheLocalImage();
    TestFormalStackwalkUnwindsRealCallerChain();
    TestThreadDumpPairingNamesNoSuspects();
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
