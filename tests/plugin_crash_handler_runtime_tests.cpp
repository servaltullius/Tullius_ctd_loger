// Runs the plugin's real vectored handler against a stand-in CrashLogger.dll.
//
// Field case (v0.2.59-rc2): a mod's assert raised a breakpoint, which the
// fatal-only filter does not record. CrashLogger reported it and faulted in its
// own memory probe, and that probe was recorded as the crash, so the dump and
// report pointed at CrashLogger.dll instead of the mod.

#include <Windows.h>
#include <Psapi.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>

#include "SkyrimDiag/CrashHandler.h"
#include "SkyrimDiag/SharedMemory.h"
#include "SkyrimDiagShared.h"

namespace {

constexpr std::uint32_t kBreakpoint = 0x80000003u;

using ProbeFn = int (*)();
ProbeFn g_probe = nullptr;

void Require(bool condition, const char* message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

struct ModuleRange
{
  std::uint64_t begin = 0;
  std::uint64_t end = 0;

  bool Contains(std::uint64_t address) const { return address >= begin && address < end; }
};

ModuleRange RangeOf(HMODULE module)
{
  MODULEINFO info{};
  Require(GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)) != 0, "GetModuleInformation failed");
  const auto begin = reinterpret_cast<std::uint64_t>(info.lpBaseOfDll);
  return { begin, begin + info.SizeOfImage };
}

int ProbeFromFilter()
{
  g_probe();
  return EXCEPTION_EXECUTE_HANDLER;
}

// CrashLogger runs while the breakpoint is still being dispatched, like the
// filter expression here, and faults in its probe on the same thread.
void BreakpointReportedByCrashLogger()
{
  __try {
    __debugbreak();
  } __except (ProbeFromFilter()) {
  }
}

void HandledBreakpoint()
{
  __try {
    __debugbreak();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

void RearmIncident(skydiag::SharedLayout* shm)
{
  InterlockedAnd(
    reinterpret_cast<volatile LONG*>(&shm->header.state_flags),
    ~static_cast<LONG>(skydiag::kState_Frozen));
}

std::uint32_t CrashSeq(const skydiag::SharedLayout* shm)
{
  return shm->header.crash_seq;
}

void TestBreakpointReportedByCrashLoggerIsRecordedInsteadOfTheProbe(
  skydiag::SharedLayout* shm,
  const ModuleRange& testExe)
{
  BreakpointReportedByCrashLogger();

  const auto& crash = shm->header.crash;
  Require(crash.exception_code == kBreakpoint, "the reported breakpoint must be the recorded crash");
  Require(crash.faulting_tid == GetCurrentThreadId(), "the recorded crash must be on the breakpoint thread");
  Require(testExe.Contains(crash.exception_addr), "the recorded address must be the breakpoint, not the probe");
  Require(crash.context.Rip == crash.exception_addr, "the recorded context must be the breakpoint's context");

  const auto seq = CrashSeq(shm);
  g_probe();
  Require(CrashSeq(shm) == seq, "later CrashLogger probes must not replace the recorded crash");
}

// Field case (v0.2.59-rc4): CrashLogger 1.25 writes a thread dump on
// Ctrl+Shift+F12, the same keys as the manual capture, and its probes there
// were recorded as crashes; each press wrote and discarded full crash dumps.
void TestCrashLoggerProbeWithNothingToReportIsIgnored(skydiag::SharedLayout* shm)
{
  RearmIncident(shm);

  // A breakpoint handled on another thread says nothing about this probe.
  std::thread other(HandledBreakpoint);
  other.join();

  const auto seq = CrashSeq(shm);
  g_probe();
  Require(CrashSeq(shm) == seq, "a CrashLogger probe with no kept exception on its thread must not be recorded");
  Require(
    (shm->header.state_flags & skydiag::kState_Frozen) == 0u,
    "an ignored CrashLogger probe must leave capture armed for a real crash");
}

void TestRealCrashAfterIgnoredProbeIsStillRecorded(skydiag::SharedLayout* shm, const ModuleRange& testExe)
{
  const auto seq = CrashSeq(shm);
  BreakpointReportedByCrashLogger();
  Require(CrashSeq(shm) != seq, "a crash after ignored probes must still be recorded");
  Require(shm->header.crash.exception_code == kBreakpoint, "the reported breakpoint must be recorded");
  Require(testExe.Contains(shm->header.crash.exception_addr), "the recorded address must be the breakpoint");
}

}  // namespace

int main(int argc, char** argv)
{
  try {
    Require(argc == 2, "usage: plugin_crash_handler_runtime_tests <path to fake CrashLogger.dll>");
    const HMODULE crashLogger = LoadLibraryA(argv[1]);
    Require(crashLogger != nullptr, "failed to load the fake CrashLogger.dll");
    g_probe = reinterpret_cast<ProbeFn>(GetProcAddress(crashLogger, "SkydiagFakeCrashLoggerProbe"));
    Require(g_probe != nullptr, "fake CrashLogger.dll has no probe export");

    Require(skydiag::plugin::InitSharedMemory(), "InitSharedMemory failed");
    Require(skydiag::plugin::InstallCrashHandler(/*crashHookMode=*/1), "InstallCrashHandler failed");
    skydiag::plugin::RefreshCrashLoggerModuleRanges();

    auto* shm = skydiag::plugin::GetShared();
    TestBreakpointReportedByCrashLoggerIsRecordedInsteadOfTheProbe(shm, RangeOf(GetModuleHandleW(nullptr)));
    TestCrashLoggerProbeWithNothingToReportIsIgnored(shm);
    TestRealCrashAfterIgnoredProbeIsStillRecorded(shm, RangeOf(GetModuleHandleW(nullptr)));
    std::puts("plugin crash handler runtime tests passed");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
