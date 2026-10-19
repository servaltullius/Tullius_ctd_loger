// What the analyzer takes from a dump, which the capturing machine (or
// whoever crafted the file) controls:
// - review H3: when the exception thread is dumped it is already inside the
//   crash handlers (the plugin's VEH returned after signalling the helper,
//   then Crash Logger or WER ran). The pointer-scan fallback must start at
//   the faulting Rsp from the exception stream, not at the dump-time Rsp, or
//   the handlers' own frames take the heaviest slots.

#include <Windows.h>

#include <DbgHelp.h>

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
using skydiag::dump_tool::minidump::ModuleInfo;
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
    dump.data(), dump.size(), modules, { kExceptionTid }, kExceptionTid, i18n::Language::kEnglish);
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

}  // namespace

int main()
{
  try {
    TestScanStartsAtTheFaultingRsp();
    TestDumpTimeRspWithoutAUsableExceptionContext();
  } catch (const std::exception& ex) {
    std::cerr << "analyzer_dump_input_tests failed: " << ex.what() << "\n";
    return 1;
  }
  std::cout << "analyzer_dump_input_tests passed\n";
  return 0;
}
