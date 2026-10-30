#include <Windows.h>

#include <DbgHelp.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "HelperRuntimeTestUtils.h"
#include "ModuleUnwindMemory.h"
#include "SkyrimDiagHelper/DumpProfile.h"
#include "SkyrimDiagHelper/DumpWriter.h"
#include "StackCodeMemory.h"

using skydiag::helper::CaptureKind;
using skydiag::helper::DumpMode;
using skydiag::helper::DumpObjectMemoryStats;
using skydiag::helper::ResolveDumpProfile;
using skydiag::helper::TracedDumpWriteCount;
using skydiag::helper::XStateSizedContextCount;
using skydiag::helper::WriteDumpWithStreams;
using skydiag::tests::runtime::MakeSharedLayout;
using skydiag::tests::runtime::MakeTempDir;
using skydiag::tests::runtime::OpenSelfProcessHandle;
using skydiag::tests::runtime::ParkedThread;
using skydiag::tests::runtime::Require;

namespace {

bool WriteCrashDump(
  HANDLE process,
  DWORD pid,
  DWORD tid,
  const CONTEXT& ctx,
  DumpMode mode,
  const std::filesystem::path& path,
  std::wstring* err,
  bool allModuleCode = false,
  DumpObjectMemoryStats* stats = nullptr)
{
  auto shared = MakeSharedLayout();
  shared->header.crash_seq = 2;
  shared->header.crash.exception_code = 0xC0000005u;
  shared->header.crash.faulting_tid = tid;
  shared->header.crash.exception_addr = ctx.Rip;
  shared->header.crash.exception_record.ExceptionCode = 0xC0000005u;
  shared->header.crash.exception_record.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Rip);
  shared->header.crash.context = ctx;

  // The production crash profile, unmodified.
  const auto profile = ResolveDumpProfile(mode, CaptureKind::Crash, allModuleCode);
  return WriteDumpWithStreams(
    process, pid, path.wstring(), shared.get(), sizeof(skydiag::SharedLayout), {}, {}, true, profile, false, err, stats);
}

// A CTD through a dangling function pointer leaves RIP in unmapped memory, and
// a smashed stack leaves RSP there. The crash dump must still be written with
// the production profile; the evidence would otherwise be lost for exactly the
// crashes that need it most.
void TestCrashDumpSurvivesUnmappedRegisterTargets()
{
  const auto outBase = MakeTempDir(L"skydiag_crash_dump_context");
  // Dump this process with a parked worker as the faulting thread (see
  // ParkedThread for why not an external process).
  ParkedThread faulting;
  const HANDLE process = OpenSelfProcessHandle();
  const DWORD pid = GetCurrentProcessId();
  const DWORD tid = faulting.tid();
  const CONTEXT real = faulting.Context();

  CONTEXT ripNullPage = real;
  ripNullPage.Rip = 0x10;
  CONTEXT ripUnmapped = real;
  ripUnmapped.Rip = 0x00007000'00000000ull;
  CONTEXT rspNullPage = real;
  rspNullPage.Rsp = 0x10;
  // A real exception ContextRecord claims extended state and carries
  // exception status bits; the copy must still go through the sized buffer.
  CONTEXT exceptionFlags = real;
  exceptionFlags.ContextFlags |= 0x00000040u | 0x08000000u;  // XSTATE bit, CONTEXT_EXCEPTION_ACTIVE

  struct Case
  {
    const char* name;
    const CONTEXT* ctx;
  };
  const Case cases[] = {
    { "faulting thread context", &real },
    { "RIP in the null page", &ripNullPage },
    { "RIP in unmapped memory", &ripUnmapped },
    { "RSP in the null page", &rspNullPage },
    { "exception context flags", &exceptionFlags },
  };

  int index = 0;
  for (const auto mode : { DumpMode::kMini, DumpMode::kDefault }) {
    for (const auto& c : cases) {
      std::wstring err;
      const auto path = outBase / (L"case" + std::to_wstring(index++) + L".dmp");
      const auto sizedBefore = XStateSizedContextCount();
      const bool ok = WriteCrashDump(process, pid, tid, *c.ctx, mode, path, &err);
      if (!ok) {
        std::fprintf(
          stderr,
          "crash dump failed: mode=%s case=%s err=%ls\n",
          mode == DumpMode::kMini ? "mini" : "default",
          c.name,
          err.c_str());
      }
      Require(ok, "Crash dump must be written even when the context points at unmapped memory");
      Require(std::filesystem::file_size(path) > 0, "Crash dump file must not be empty");
      Require(XStateSizedContextCount() > sizedBefore, "The exception context must go through the XSTATE-sized copy");
    }
  }

  CloseHandle(process);
  std::filesystem::remove_all(outBase);
}

// With SKYDIAG_DUMP_IO_TRACE=1 the callback performs the file writes. The
// dump must come out the same as dbghelp's own writes: readable and with the
// exception stream that names the faulting thread.
void TestTracedDumpWritesAreComplete()
{
  const auto outBase = MakeTempDir(L"skydiag_crash_dump_io_trace");
  ParkedThread faulting;
  const HANDLE process = OpenSelfProcessHandle();
  const CONTEXT real = faulting.Context();

  wchar_t previous[8]{};
  const bool hadPrevious = GetEnvironmentVariableW(L"SKYDIAG_DUMP_IO_TRACE", previous, 8) > 0;
  SetEnvironmentVariableW(L"SKYDIAG_DUMP_IO_TRACE", L"1");
  for (const auto mode : { DumpMode::kMini, DumpMode::kDefault }) {
    const auto writesBefore = TracedDumpWriteCount();
    std::wstring err;
    const auto path = outBase / (mode == DumpMode::kMini ? L"traced_mini.dmp" : L"traced_default.dmp");
    const bool ok = WriteCrashDump(process, GetCurrentProcessId(), faulting.tid(), real, mode, path, &err);
    if (!ok) {
      std::fprintf(stderr, "traced crash dump failed: err=%ls\n", err.c_str());
    }
    Require(ok, "A traced crash dump must be written");
    Require(TracedDumpWriteCount() > writesBefore, "dbghelp must hand the dump's file writes to the callback");

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Require(file != INVALID_HANDLE_VALUE, "Traced dump must open");
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    void* view = mapping ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : nullptr;
    PMINIDUMP_DIRECTORY dir = nullptr;
    void* stream = nullptr;
    ULONG streamSize = 0;
    const auto* exception = static_cast<const MINIDUMP_EXCEPTION_STREAM*>(stream);
    const bool hasException =
      view && MiniDumpReadDumpStream(view, ExceptionStream, &dir, &stream, &streamSize) && stream &&
      (exception = static_cast<const MINIDUMP_EXCEPTION_STREAM*>(stream))->ThreadId == faulting.tid();
    // The exception context is written from an XSTATE-sized copy; the
    // registers must still be the faulting thread's.
    const bool contextMatches = hasException && exception->ThreadContext.DataSize >= sizeof(CONTEXT) &&
      reinterpret_cast<const CONTEXT*>(static_cast<const BYTE*>(view) + exception->ThreadContext.Rva)->Rip == real.Rip &&
      reinterpret_cast<const CONTEXT*>(static_cast<const BYTE*>(view) + exception->ThreadContext.Rva)->Rsp == real.Rsp;
    if (view) UnmapViewOfFile(view);
    if (mapping) CloseHandle(mapping);
    CloseHandle(file);
    Require(hasException, "A traced dump must be a readable minidump with the faulting thread's exception");
    Require(contextMatches, "The dumped exception context must carry the faulting thread's registers");
  }
  SetEnvironmentVariableW(L"SKYDIAG_DUMP_IO_TRACE", hadPrevious ? previous : nullptr);

  CloseHandle(process);
  std::filesystem::remove_all(outBase);
}

// A write that fails (here: the process has already exited, as when the game
// ends before its dump) leaves no file that the viewer or retention would
// take for a dump.
void TestFailedDumpLeavesNoFile()
{
  const auto outBase = MakeTempDir(L"skydiag_failed_dump");
  ParkedThread faulting;
  auto child = skydiag::tests::runtime::LaunchSleepingChildProcess();
  Require(TerminateProcess(child.pi.hProcess, 1) != FALSE, "TerminateProcess failed");
  Require(WaitForSingleObject(child.pi.hProcess, 10000) == WAIT_OBJECT_0, "the child did not exit");
  const auto path = outBase / L"failed.dmp";
  std::wstring err;
  const bool ok = WriteCrashDump(
    child.pi.hProcess, child.pi.dwProcessId, faulting.tid(), faulting.Context(), DumpMode::kDefault, path, &err);
  skydiag::tests::runtime::TerminateChildProcess(&child);
  Require(!ok, "A dump without read access fails");
  Require(!std::filesystem::exists(path), "A failed dump leaves no file");
  std::filesystem::remove_all(outBase);
}

// The memory ranges a dump holds, [begin, end).
std::vector<std::pair<std::uint64_t, std::uint64_t>> ReadDumpMemoryRanges(const std::filesystem::path& path)
{
  std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  Require(file != INVALID_HANDLE_VALUE, "The dump must open");
  HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  void* view = mapping ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : nullptr;
  PMINIDUMP_DIRECTORY dir = nullptr;
  void* stream = nullptr;
  ULONG streamSize = 0;
  if (view && MiniDumpReadDumpStream(view, MemoryListStream, &dir, &stream, &streamSize) && stream) {
    const auto* list = static_cast<const MINIDUMP_MEMORY_LIST*>(stream);
    for (ULONG32 i = 0; i < list->NumberOfMemoryRanges; ++i) {
      const auto& range = list->MemoryRanges[i];
      ranges.emplace_back(range.StartOfMemoryRange, range.StartOfMemoryRange + range.Memory.DataSize);
    }
  }
  if (view) UnmapViewOfFile(view);
  if (mapping) CloseHandle(mapping);
  CloseHandle(file);
  Require(!ranges.empty(), "The dump must have a memory list");
  return ranges;
}

// Bytes of [begin, end) the ranges hold, each counted once (the writer's own
// ranges and the added ones may overlap).
std::uint64_t CoveredBytes(
  std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges,
  std::uint64_t begin,
  std::uint64_t end)
{
  std::sort(ranges.begin(), ranges.end());
  std::uint64_t covered = 0;
  std::uint64_t at = begin;
  for (const auto& [a, b] : ranges) {
    const std::uint64_t from = std::max(a, at);
    const std::uint64_t to = std::min(b, end);
    if (to > from) {
      covered += to - from;
      at = to;
    }
  }
  return covered;
}

// [begin, end) of a module's section, from its image in this process.
std::pair<std::uint64_t, std::uint64_t> SectionRange(const wchar_t* moduleName, const char* sectionName)
{
  const auto* base = reinterpret_cast<const BYTE*>(GetModuleHandleW(moduleName));
  Require(base != nullptr, "The module must be loaded");
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  const auto* section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    if (std::strncmp(reinterpret_cast<const char*>(section[i].Name), sectionName, IMAGE_SIZEOF_SHORT_NAME) == 0) {
      const auto begin = reinterpret_cast<std::uint64_t>(base) + section[i].VirtualAddress;
      return { begin, begin + section[i].Misc.VirtualSize };
    }
  }
  Require(false, "The section must exist");
  return {};
}

// ADR-0023: a DumpMode=1 crash dump holds the main image's code whole and,
// of other modules, only the code around the thread RIPs and stack values,
// with no module resources. DumpAllModuleCode=1 restores every module's code
// and the module headers.
void TestDumpKeepsGameCodeAndStackCode()
{
  const auto outBase = MakeTempDir(L"skydiag_stack_code");
  ParkedThread faulting;
  const HANDLE process = OpenSelfProcessHandle();
  const CONTEXT real = faulting.Context();
  const auto gameText = SectionRange(nullptr, ".text");
  const auto ntdllText = SectionRange(L"ntdll.dll", ".text");
  const auto ntdllTextSize = ntdllText.second - ntdllText.first;
  const auto* kernel32 = reinterpret_cast<const BYTE*>(GetModuleHandleW(L"kernel32.dll"));
  const auto* kernel32Nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
    kernel32 + reinterpret_cast<const IMAGE_DOS_HEADER*>(kernel32)->e_lfanew);
  const auto& resources = kernel32Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
  Require(resources.Size != 0, "kernel32.dll must have resources");
  const auto resourceBegin = reinterpret_cast<std::uint64_t>(kernel32) + resources.VirtualAddress;
  const auto resourceEnd = resourceBegin + resources.Size;
  const auto gameBase = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));

  DumpObjectMemoryStats stats{};
  std::wstring err;
  const auto stackPath = outBase / L"game_and_stacks.dmp";
  const bool written = WriteCrashDump(
    process, GetCurrentProcessId(), faulting.tid(), real, DumpMode::kDefault, stackPath, &err, false, &stats);
  if (!written) {
    std::fprintf(stderr, "game_and_stacks dump failed: %ls\n", err.c_str());
  }
  Require(written, "The DumpMode=1 crash dump must be written");
  Require(stats.code.collected && !stats.code.all_modules_fallback, "The stack code must be collected");
  Require(stats.code.threads > 0u && stats.code.values > 0u, "Thread RIPs and stack values must be examined");
  Require(stats.code.modules > 0u, "The main image gets its headers and unwind data");
  Require(!stats.code.budget_reached, "A test process stays within the budgets");
  const auto stackDump = ReadDumpMemoryRanges(stackPath);
  const auto ntdllCovered = CoveredBytes(stackDump, ntdllText.first, ntdllText.second);
  std::fprintf(stderr,
    "stack code: threads=%u values=%u ranges=%u bytes=%llu modules=%u module_bytes=%llu us=%u; ntdll .text %llu of %llu\n",
    stats.code.threads, stats.code.values, stats.code.code_ranges, static_cast<unsigned long long>(stats.code.code_bytes),
    stats.code.modules, static_cast<unsigned long long>(stats.code.module_bytes), stats.code.elapsed_us,
    static_cast<unsigned long long>(ntdllCovered), static_cast<unsigned long long>(ntdllTextSize));
  Require(CoveredBytes(stackDump, gameText.first, gameText.second) == gameText.second - gameText.first,
    "The main image's code must be in the dump whole");
  Require(CoveredBytes(stackDump, gameBase, gameBase + 0x200) == 0x200u, "The main image's header must be in the dump");
  Require(ntdllCovered > 0u && ntdllCovered < ntdllTextSize / 2u, "Of ntdll's code only what the stacks point at");
  Require(CoveredBytes(stackDump, real.Rip - 0x100u, real.Rip + 0x100u) == 0x200u,
    "The bytes around the crash RIP must be in the dump");
  Require(CoveredBytes(stackDump, resourceBegin, resourceEnd) == 0u, "Module resources must stay out of the dump");

  // A freeze dump of the same process, with no exception: the same scope.
  DumpObjectMemoryStats hangStats{};
  auto shared = MakeSharedLayout();
  const auto hangPath = outBase / L"hang.dmp";
  const auto hangProfile = ResolveDumpProfile(DumpMode::kDefault, CaptureKind::Hang);
  Require(
    WriteDumpWithStreams(process, GetCurrentProcessId(), hangPath.wstring(), shared.get(), sizeof(skydiag::SharedLayout),
      {}, {}, false, hangProfile, false, &err, &hangStats),
    "The DumpMode=1 freeze dump must be written");
  Require(hangStats.code.collected && hangStats.code.threads > 0u, "Freeze dumps get the stack code too");
  const auto hangDump = ReadDumpMemoryRanges(hangPath);
  Require(CoveredBytes(hangDump, ntdllText.first, ntdllText.second) < ntdllTextSize / 2u,
    "A freeze dump holds only the stack code of ntdll");
  Require(CoveredBytes(hangDump, real.Rip - 0x100u, real.Rip + 0x100u) == 0x200u,
    "The bytes around the parked thread's RIP must be in the freeze dump");

  DumpObjectMemoryStats allStats{};
  const auto allPath = outBase / L"all_code.dmp";
  Require(
    WriteCrashDump(process, GetCurrentProcessId(), faulting.tid(), real, DumpMode::kDefault, allPath, &err, true, &allStats),
    "The DumpAllModuleCode=1 crash dump must be written");
  Require(!allStats.code.collected, "Every module's code needs no stack code");
  const auto allDump = ReadDumpMemoryRanges(allPath);
  Require(CoveredBytes(allDump, ntdllText.first, ntdllText.second) == ntdllTextSize,
    "DumpAllModuleCode=1 writes every module's code");
  Require(CoveredBytes(allDump, resourceBegin, resourceEnd) > 0u,
    "DumpAllModuleCode=1 crash dumps keep the module headers' resources, as through v0.2.71");

  CloseHandle(process);
  std::filesystem::remove_all(outBase);
}

// ADR-0023: the collector on its own, with no game image, so the main image
// is an ordinary module (not under the Windows folder). A return address into
// its code brings its header and .pdata along, a RIP in its data still keeps
// the bytes around it, and the scan goes past the stack's guard page, which
// keeps its guard.
void TestStackCodeCollector()
{
  using skydiag::helper::internal::ExtraDumpMemory;
  using skydiag::helper::internal::StackCodeMemoryCollector;
  const HANDLE process = OpenSelfProcessHandle();
  const auto* image = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
    image + reinterpret_cast<const IMAGE_DOS_HEADER*>(image)->e_lfanew);
  const auto imageBase = reinterpret_cast<std::uint64_t>(image);
  const auto& pdata = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
  Require(pdata.Size != 0, "The test executable must have .pdata");
  wchar_t path[MAX_PATH]{};
  Require(GetModuleFileNameW(nullptr, path, MAX_PATH) != 0, "GetModuleFileNameW failed");
  const auto text = SectionRange(nullptr, ".text");
  const auto rdata = SectionRange(nullptr, ".rdata");

  // A stack of three pages with a guard page in the middle, and a return
  // address into the main image's code above it.
  SYSTEM_INFO system{};
  GetSystemInfo(&system);
  const std::size_t page = system.dwPageSize;
  auto* stack = static_cast<BYTE*>(VirtualAlloc(nullptr, 3 * page, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  Require(stack != nullptr, "VirtualAlloc failed");
  const std::uint64_t returnAddress = text.first + 0x40;
  std::memcpy(stack + 2 * page, &returnAddress, sizeof(returnAddress));
  DWORD oldProtect = 0;
  Require(VirtualProtect(stack + page, page, PAGE_READWRITE | PAGE_GUARD, &oldProtect) != FALSE, "VirtualProtect failed");

  StackCodeMemoryCollector collector(process, /*gameImageBase=*/0, {}, /*exceptionRip=*/0);
  collector.OnModule(imageBase, nt->OptionalHeader.SizeOfImage, path);
  CONTEXT context{};
  context.Rip = rdata.first + 0x10;  // in data, not code
  context.Rsp = 0;                    // outside the stack: scanned from its low end
  const auto stackLow = reinterpret_cast<std::uint64_t>(stack);
  collector.OnThread(1, context, stackLow + 3 * page, stackLow);
  std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
  for (const ExtraDumpMemory& range : collector.Collect()) {
    ranges.emplace_back(range.base, range.base + range.size);
  }
  const auto& stats = collector.Stats();

  MEMORY_BASIC_INFORMATION guard{};
  Require(VirtualQuery(stack + page, &guard, sizeof(guard)) == sizeof(guard), "VirtualQuery failed");
  const bool guardKept = (guard.Protect & PAGE_GUARD) != 0;
  VirtualFree(stack, 0, MEM_RELEASE);
  CloseHandle(process);

  Require(guardKept, "The stack's guard page must keep its guard");
  Require(stats.values == 1u && stats.modules == 1u, "The return address and its module");
  Require(CoveredBytes(ranges, returnAddress - 0x100u, returnAddress + 0x40u) == 0x140u,
    "The bytes around the return address");
  Require(CoveredBytes(ranges, imageBase, imageBase + 0x200u) == 0x200u, "The module's header");
  Require(CoveredBytes(ranges, imageBase + pdata.VirtualAddress, imageBase + pdata.VirtualAddress + pdata.Size) ==
      pdata.Size,
    "The module's .pdata");
  Require(CoveredBytes(ranges, context.Rip - 0x100u, context.Rip + 0x100u) == 0x200u,
    "The bytes around a RIP outside module code");

  // Out of time before the first thread: the crash RIP's module still gets
  // its unwind data, which is taken right after the RIP, not after every stack.
  const HANDLE again = OpenSelfProcessHandle();
  StackCodeMemoryCollector late(again, /*gameImageBase=*/0, {}, /*exceptionRip=*/returnAddress, std::chrono::milliseconds{ 0 });
  late.OnModule(imageBase, nt->OptionalHeader.SizeOfImage, path);
  late.OnThread(1, context, 0, 0);
  ranges.clear();
  for (const ExtraDumpMemory& range : late.Collect()) {
    ranges.emplace_back(range.base, range.base + range.size);
  }
  CloseHandle(again);
  Require(late.Stats().budget_reached && late.Stats().threads == 0u, "No thread is examined after the deadline");
  Require(late.Stats().modules == 1u &&
      CoveredBytes(ranges, imageBase + pdata.VirtualAddress, imageBase + pdata.VirtualAddress + pdata.Size) == pdata.Size,
    "The crash RIP's module keeps its .pdata past the deadline");
}

}  // namespace

// ADR-0023: the code kept around one value: the window, and the .pdata
// function (a return address one byte back, a RIP as is), at most 64 KB.
void TestStackCodeRanges()
{
  using skydiag::helper::internal::CodeImage;
  using skydiag::helper::internal::ExtraDumpMemory;
  using skydiag::helper::internal::StackCodeRanges;
  constexpr std::uint64_t kBase = 0x7FF000000000ull;
  CodeImage image{};
  image.base = kBase;
  image.size = 0x10000;
  image.executable = { { 0x1000, 0x8000 }, { 0xF000, 0x10000 } };
  const auto function = [](std::uint32_t begin, std::uint32_t end) {
    RUNTIME_FUNCTION f{};
    f.BeginAddress = begin;
    f.EndAddress = end;
    return f;
  };
  image.functions = { function(0x1000, 0x1200), function(0x1200, 0x1400),
    function(0x3000, 0x3000 + skydiag::helper::internal::kMaxStackCodeFunctionBytes + 0x10) };

  std::vector<ExtraDumpMemory> out;
  // A return address right after a call that ends the first function.
  Require(StackCodeRanges(image, kBase + 0x1200, false, out), "a return address in code");
  Require(out.size() == 2u && out[0].base == kBase + 0x1100 && out[0].size == 0x140u, "-0x100/+0x40 around it");
  Require(out[1].base == kBase + 0x1000 && out[1].size == 0x200u, "the function of the call before it");
  out.clear();
  Require(StackCodeRanges(image, kBase + 0x1200, true, out), "a RIP in code");
  Require(out.size() == 2u && out[0].base == kBase + 0x1100 && out[0].size == 0x200u, "+-0x100 around a RIP");
  Require(out[1].base == kBase + 0x1200 && out[1].size == 0x200u, "the function the RIP is in");
  out.clear();
  Require(StackCodeRanges(image, kBase + 0x3008, false, out) && out.size() == 1u, "a longer function keeps the window only");
  out.clear();
  Require(StackCodeRanges(image, kBase + 0xFFF8, true, out) && out.size() == 1u, "a RIP near the image end");
  Require(out[0].base == kBase + 0xFEF8 && out[0].size == 0x108u, "the window stops at the image end");
  out.clear();
  Require(!StackCodeRanges(image, kBase + 0x9000, false, out) && out.empty(), "data is not code");
  Require(!StackCodeRanges(image, kBase + 0x10000, true, out) && out.empty(), "past the image");
}

// ADR-0011: the UNWIND_INFO sizes the helper adds to dumps, including the
// handler RVA and a chained entry's own unwind info.
void TestUnwindInfoRanges()
{
  constexpr std::uint64_t kBase = 0x7FF000000000ull;
  constexpr std::uint32_t kSpanRva = 0x100;
  std::vector<std::uint8_t> span(0x400, 0);
  const auto put = [&](std::uint32_t rva, std::uint8_t flags, std::uint8_t codes) {
    span[rva - kSpanRva] = static_cast<std::uint8_t>(1u | (flags << 3));  // version 1
    span[rva - kSpanRva + 2] = codes;
  };
  put(0x100, 0, 3);     // 4 + 2*4 (codes padded to even) = 12
  put(0x200, 0x1, 0);   // EHANDLER: 4 + handler 4 = 8
  put(0x300, 0x4, 1);   // CHAININFO: 4 + 2*2 + 12 = 20, chained to 0x380
  RUNTIME_FUNCTION chained{};
  chained.BeginAddress = 0x1000;
  chained.EndAddress = 0x1100;
  chained.UnwindData = 0x380;
  std::memcpy(span.data() + (0x300 - kSpanRva) + 8, &chained, sizeof(chained));
  put(0x380, 0, 0);     // 4

  std::vector<RUNTIME_FUNCTION> functions(4);
  functions[0].UnwindData = 0x100;
  functions[1].UnwindData = 0x200;
  functions[2].UnwindData = 0x300;
  functions[3].UnwindData = 0x41;  // points at another .pdata entry: skipped
  const auto ranges = skydiag::helper::internal::UnwindInfoRanges(kBase, functions, kSpanRva, span);
  Require(ranges.size() == 4u, "four unwind infos");
  Require(ranges[0].base == kBase + 0x100 && ranges[0].size == 12u, "unwind codes are padded to an even count");
  Require(ranges[1].base == kBase + 0x200 && ranges[1].size == 8u, "a handler adds its RVA");
  Require(ranges[2].base == kBase + 0x300 && ranges[2].size == 20u, "a chain adds the chained entry");
  Require(ranges[3].base == kBase + 0x380 && ranges[3].size == 4u, "the chained unwind info is included too");
}

int main()
{
  try {
    TestUnwindInfoRanges();
    TestStackCodeRanges();
    TestStackCodeCollector();
    TestFailedDumpLeavesNoFile();
    TestDumpKeepsGameCodeAndStackCode();
    TestCrashDumpSurvivesUnmappedRegisterTargets();
    TestTracedDumpWritesAreComplete();
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
