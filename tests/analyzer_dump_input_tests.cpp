// What the analyzer takes from a dump, which the capturing machine (or
// whoever crafted the file) controls:
// - review H3: when the exception thread is dumped it is already inside the
//   crash handlers (the plugin's VEH returned after signalling the helper,
//   then Crash Logger or WER ran). The pointer-scan fallback must start at
//   the faulting Rsp from the exception stream, not at the dump-time Rsp, or
//   the handlers' own frames take the heaviest slots;
// - review M16: module paths the analyzer may open or load from;
// - review L8: module names of any length.

#include <Windows.h>

#include <DbgHelp.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "AnalyzerInternals.h"

namespace {

using skydiag::dump_tool::SuspectItem;
using skydiag::dump_tool::internal::ComputeStackScanSuspects;
using skydiag::dump_tool::minidump::IsLocalFixedDrivePath;
using skydiag::dump_tool::minidump::ModuleInfo;
using skydiag::dump_tool::minidump::ModulePlusOffsetText;
using skydiag::dump_tool::minidump::ReadMinidumpStringUtf8;
namespace i18n = skydiag::dump_tool::i18n;

constexpr std::uint32_t kExceptionTid = 4242;
constexpr std::uint64_t kStackStart = 0x10000;
constexpr std::size_t kStackSize = 0x2000;
constexpr std::uint64_t kDumpTimeRsp = kStackStart;             // inside the handlers
constexpr std::uint64_t kFaultRsp = kStackStart + 0x800;         // where the fault happened
constexpr std::uint64_t kHandlerBase = 0x180000000ull;
constexpr std::uint64_t kPluginBase = 0x190000000ull;

void Require(bool condition, const char* message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename T>
void Put(std::vector<std::uint8_t>& bytes, std::size_t offset, const T& value)
{
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

// Header, one ThreadListStream with one thread, its context, its stack.
std::vector<std::uint8_t> BuildDump()
{
  const std::size_t dirOff = sizeof(MINIDUMP_HEADER);
  const std::size_t listOff = dirOff + sizeof(MINIDUMP_DIRECTORY);
  const std::size_t listSize = offsetof(MINIDUMP_THREAD_LIST, Threads) + sizeof(MINIDUMP_THREAD);
  const std::size_t contextOff = listOff + listSize;
  const std::size_t stackOff = contextOff + sizeof(CONTEXT);
  std::vector<std::uint8_t> dump(stackOff + kStackSize);

  MINIDUMP_HEADER header{};
  header.Signature = MINIDUMP_SIGNATURE;
  header.Version = MINIDUMP_VERSION;
  header.NumberOfStreams = 1;
  header.StreamDirectoryRva = static_cast<RVA>(dirOff);
  Put(dump, 0, header);

  MINIDUMP_DIRECTORY dir{};
  dir.StreamType = ThreadListStream;
  dir.Location.DataSize = static_cast<ULONG32>(listSize);
  dir.Location.Rva = static_cast<RVA>(listOff);
  Put(dump, dirOff, dir);

  const ULONG32 count = 1;
  Put(dump, listOff, count);
  MINIDUMP_THREAD thread{};
  thread.ThreadId = kExceptionTid;
  thread.Stack.StartOfMemoryRange = kStackStart;
  thread.Stack.Memory.DataSize = static_cast<ULONG32>(kStackSize);
  thread.Stack.Memory.Rva = static_cast<RVA>(stackOff);
  thread.ThreadContext.DataSize = static_cast<ULONG32>(sizeof(CONTEXT));
  thread.ThreadContext.Rva = static_cast<RVA>(contextOff);
  Put(dump, listOff + offsetof(MINIDUMP_THREAD_LIST, Threads), thread);

  CONTEXT dumpTime{};
  dumpTime.Rsp = kDumpTimeRsp;
  Put(dump, contextOff, dumpTime);

  // The handler frames fill the top of the dump-time stack; the plugin's
  // return addresses sit right at the faulting Rsp.
  for (std::size_t slot = 0; slot < 8; ++slot) {
    Put(dump, stackOff + slot * 8u, static_cast<std::uint64_t>(kHandlerBase + 0x1000 + slot));
  }
  for (std::size_t slot = 0; slot < 4; ++slot) {
    Put(dump, stackOff + (kFaultRsp - kStackStart) + slot * 8u, static_cast<std::uint64_t>(kPluginBase + 0x2000 + slot));
  }
  return dump;
}

std::vector<ModuleInfo> Modules()
{
  ModuleInfo handler{};
  handler.base = kHandlerBase;
  handler.end = kHandlerBase + 0x100000;
  handler.filename = L"HandlerHost.dll";
  handler.path = L"C:\\Game\\Data\\SKSE\\Plugins\\HandlerHost.dll";
  ModuleInfo plugin{};
  plugin.base = kPluginBase;
  plugin.end = kPluginBase + 0x100000;
  plugin.filename = L"BadPlugin.dll";
  plugin.path = L"C:\\Game\\Data\\SKSE\\Plugins\\BadPlugin.dll";
  return { handler, plugin };
}

bool Lists(const std::vector<SuspectItem>& suspects, const wchar_t* module)
{
  for (const auto& s : suspects) {
    if (s.module_filename == module) {
      return true;
    }
  }
  return false;
}

void TestScanStartsAtTheFaultingRsp()
{
  auto dump = BuildDump();
  const auto modules = Modules();
  CONTEXT fault{};
  fault.Rsp = kFaultRsp;
  const auto suspects = ComputeStackScanSuspects(
    dump.data(), dump.size(), modules, { kExceptionTid }, kExceptionTid, i18n::Language::kEnglish, &fault);
  Require(!suspects.empty() && suspects.front().module_filename == L"BadPlugin.dll",
    "the plugin at the faulting Rsp leads");
  Require(suspects.front().score == 4u * 8u, "its slots get the nearest-slot weight");
  Require(!Lists(suspects, L"HandlerHost.dll"), "handler frames below the fault are not scanned");
}

void TestDumpTimeRspWithoutAUsableExceptionContext()
{
  auto dump = BuildDump();
  const auto modules = Modules();
  // No exception context: the old start.
  auto suspects = ComputeStackScanSuspects(
    dump.data(), dump.size(), modules, { kExceptionTid }, kExceptionTid, i18n::Language::kEnglish, nullptr);
  Require(!suspects.empty() && suspects.front().module_filename == L"HandlerHost.dll", "no context: dump-time Rsp");

  // A faulting Rsp outside the captured stack (a stack overflow into the
  // guard page, a corrupt context) is not trusted.
  CONTEXT outside{};
  outside.Rsp = kStackStart + kStackSize + 0x100;
  suspects = ComputeStackScanSuspects(
    dump.data(), dump.size(), modules, { kExceptionTid }, kExceptionTid, i18n::Language::kEnglish, &outside);
  Require(!suspects.empty() && suspects.front().module_filename == L"HandlerHost.dll",
    "an Rsp outside the stack falls back to the dump-time Rsp");

  // The context belongs to the exception thread only.
  CONTEXT fault{};
  fault.Rsp = kFaultRsp;
  suspects = ComputeStackScanSuspects(
    dump.data(), dump.size(), modules, { kExceptionTid }, /*exceptionTid=*/0u, i18n::Language::kEnglish, &fault);
  Require(!suspects.empty() && suspects.front().module_filename == L"HandlerHost.dll",
    "a scan without an exception thread ignores the context");
}

void TestOnlyLocalFixedDrivePathsAreOpened()
{
  Require(!IsLocalFixedDrivePath(L"\\\\attacker\\share\\SkyrimSE.exe"), "UNC share");
  Require(!IsLocalFixedDrivePath(L"//attacker/share/SkyrimSE.exe"), "UNC share, forward slashes");
  Require(!IsLocalFixedDrivePath(L"\\\\?\\UNC\\attacker\\share\\x.dll"), "long UNC path");
  Require(!IsLocalFixedDrivePath(L"\\\\.\\PhysicalDrive0"), "device path");
  Require(!IsLocalFixedDrivePath(L"Data\\SKSE\\Plugins\\x.dll"), "relative path");
  Require(!IsLocalFixedDrivePath(L"C:x.dll"), "drive-relative path");
  Require(!IsLocalFixedDrivePath(L""), "empty");
  wchar_t system[MAX_PATH]{};
  const UINT n = GetSystemDirectoryW(system, MAX_PATH);
  Require(n > 0 && n < MAX_PATH, "system directory");
  Require(IsLocalFixedDrivePath(system), "the system drive is a local fixed disk");
}

void TestLongModuleNames()
{
  const std::wstring name(5000, L'a');
  Require(ModulePlusOffsetText(name, 0x1234) == name + L"+0x1234", "a 5000-character name is formatted, not truncated");

  // A MINIDUMP_STRING longer than any Windows path is refused.
  std::vector<std::uint8_t> dump(16 + 70000, 0);
  const std::uint32_t tooLong = 32768u * 2u;
  std::memcpy(dump.data() + 16, &tooLong, sizeof(tooLong));
  std::string out;
  Require(!ReadMinidumpStringUtf8(dump.data(), dump.size(), 16, out), "an over-long string is refused");
  const std::uint32_t ok = 4u;
  std::memcpy(dump.data() + 16, &ok, sizeof(ok));
  const wchar_t ab[] = L"ab";
  std::memcpy(dump.data() + 20, ab, 4);
  Require(ReadMinidumpStringUtf8(dump.data(), dump.size(), 16, out) && out == "ab", "a normal string reads");
}

// engine_spin_nan from a dump: one thread whose captured memory holds the
// 2026-10-22 loop (addss xmm0,xmm6; comiss xmm0,xmm7; jb -9) around its Rip,
// with xmm0 = NaN in its context. The context's DataSize decides how much of
// the context exists; the XMM read must stay inside it.
constexpr std::uint32_t kSpinTid = 19588;
constexpr std::uint64_t kCodeStart = 0x7ff780297440ull;
constexpr std::size_t kCodeSize = 0x40;
constexpr std::size_t kLoopOffset = 0x25;  // Rip = kCodeStart + 0x25 = ...297465

std::vector<std::uint8_t> BuildSpinDump(std::uint32_t contextSize, std::uint32_t xmm0, bool loop = true)
{
  const std::size_t dirOff = sizeof(MINIDUMP_HEADER);
  const std::size_t listOff = dirOff + sizeof(MINIDUMP_DIRECTORY);
  const std::size_t listSize = offsetof(MINIDUMP_THREAD_LIST, Threads) + sizeof(MINIDUMP_THREAD);
  const std::size_t contextOff = listOff + listSize;
  const std::size_t codeOff = contextOff + sizeof(CONTEXT);
  std::vector<std::uint8_t> dump(codeOff + kCodeSize, 0);

  MINIDUMP_HEADER header{};
  header.Signature = MINIDUMP_SIGNATURE;
  header.Version = MINIDUMP_VERSION;
  header.NumberOfStreams = 1;
  header.StreamDirectoryRva = static_cast<RVA>(dirOff);
  Put(dump, 0, header);
  MINIDUMP_DIRECTORY dir{};
  dir.StreamType = ThreadListStream;
  dir.Location.DataSize = static_cast<ULONG32>(listSize);
  dir.Location.Rva = static_cast<RVA>(listOff);
  Put(dump, dirOff, dir);
  const ULONG32 count = 1;
  Put(dump, listOff, count);
  // The code goes where the thread's stack memory would: without a memory
  // list the analyzer reads thread memory ranges.
  MINIDUMP_THREAD thread{};
  thread.ThreadId = kSpinTid;
  thread.Stack.StartOfMemoryRange = kCodeStart;
  thread.Stack.Memory.DataSize = static_cast<ULONG32>(kCodeSize);
  thread.Stack.Memory.Rva = static_cast<RVA>(codeOff);
  thread.ThreadContext.DataSize = contextSize;
  thread.ThreadContext.Rva = static_cast<RVA>(contextOff);
  Put(dump, listOff + offsetof(MINIDUMP_THREAD_LIST, Threads), thread);

  CONTEXT ctx{};
  ctx.Rip = kCodeStart + kLoopOffset;
  ctx.Rsp = 0x10000;
  ctx.MxCsr = 0x1fbf;
  ctx.FltSave.XmmRegisters[0].Low = xmm0;
  ctx.FltSave.XmmRegisters[6].Low = 0x40C90FDBu;  // 2*pi
  Put(dump, contextOff, ctx);

  std::fill(dump.begin() + static_cast<std::ptrdiff_t>(codeOff), dump.end(), std::uint8_t{ 0xCC });
  const std::uint8_t fieldLoop[] = { 0xF3, 0x0F, 0x58, 0xC6, 0x0F, 0x2F, 0xC7, 0x72, 0xF7 };
  if (loop) {
    std::memcpy(dump.data() + codeOff + kLoopOffset, fieldLoop, sizeof(fieldLoop));
  }
  return dump;
}

void TestEngineSpinProbeReadsTheDump()
{
  using skydiag::dump_tool::internal::ProbeFloatCompareSpin;
  using skydiag::dump_tool::minidump::LoadThreads;
  const std::vector<ModuleInfo> noModules;

  auto dump = BuildSpinDump(static_cast<std::uint32_t>(sizeof(CONTEXT)), 0x7FE500DDu);
  auto threads = LoadThreads(dump.data(), dump.size());
  auto probe = ProbeFloatCompareSpin(dump.data(), dump.size(), noModules, threads, kSpinTid);
  Require(probe.loop_found && probe.nan, "the field loop with NaN");
  Require(probe.rip == kCodeStart + kLoopOffset, "the thread's Rip");
  Require(probe.compare == "comiss" && probe.nan_register == "xmm0", "comiss on xmm0");
  Require(probe.nan_bits == 0x7FE500DDu && probe.mxcsr == 0x1fbfu, "the NaN bits and MxCsr");

  // A real number in xmm0: the loop is there, NaN is not.
  dump = BuildSpinDump(static_cast<std::uint32_t>(sizeof(CONTEXT)), 0xBF800000u);
  threads = LoadThreads(dump.data(), dump.size());
  probe = ProbeFloatCompareSpin(dump.data(), dump.size(), noModules, threads, kSpinTid);
  Require(probe.loop_found && !probe.nan && probe.nan_register.empty(), "a loop without NaN");

  // A context cut off before the XMM registers: no NaN read past it.
  dump = BuildSpinDump(0x1A0u, 0x7FE500DDu);
  threads = LoadThreads(dump.data(), dump.size());
  probe = ProbeFloatCompareSpin(dump.data(), dump.size(), noModules, threads, kSpinTid);
  Require(probe.loop_found && !probe.nan, "XMM registers outside DataSize are not read");

  // A context running past the end of the file is refused.
  dump = BuildSpinDump(static_cast<std::uint32_t>(sizeof(CONTEXT)), 0x7FE500DDu);
  dump.resize(dump.size() - kCodeSize - sizeof(CONTEXT) + 0x100u);
  threads = LoadThreads(dump.data(), dump.size());
  probe = ProbeFloatCompareSpin(dump.data(), dump.size(), noModules, threads, kSpinTid);
  Require(!probe.loop_found && !probe.nan, "a truncated dump yields nothing");

  // Other code at Rip, another thread id.
  dump = BuildSpinDump(static_cast<std::uint32_t>(sizeof(CONTEXT)), 0x7FE500DDu, /*loop=*/false);
  threads = LoadThreads(dump.data(), dump.size());
  Require(!ProbeFloatCompareSpin(dump.data(), dump.size(), noModules, threads, kSpinTid).loop_found, "no loop");
  dump = BuildSpinDump(static_cast<std::uint32_t>(sizeof(CONTEXT)), 0x7FE500DDu);
  threads = LoadThreads(dump.data(), dump.size());
  Require(!ProbeFloatCompareSpin(dump.data(), dump.size(), noModules, threads, 1234).loop_found, "unknown thread");
}

}  // namespace

int main()
{
  try {
    TestScanStartsAtTheFaultingRsp();
    TestDumpTimeRspWithoutAUsableExceptionContext();
    TestOnlyLocalFixedDrivePathsAreOpened();
    TestLongModuleNames();
    TestEngineSpinProbeReadsTheDump();
  } catch (const std::exception& ex) {
    std::cerr << "analyzer_dump_input_tests failed: " << ex.what() << "\n";
    return 1;
  }
  std::cout << "analyzer_dump_input_tests passed\n";
  return 0;
}
