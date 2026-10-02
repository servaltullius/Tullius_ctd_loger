#include <Windows.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>

#include "HelperRuntimeTestUtils.h"
#include "SkyrimDiagHelper/DumpProfile.h"
#include "SkyrimDiagHelper/DumpWriter.h"

using skydiag::helper::CaptureKind;
using skydiag::helper::DumpMode;
using skydiag::helper::ResolveDumpProfile;
using skydiag::helper::WriteDumpWithStreams;
using skydiag::tests::runtime::LaunchSleepingChildProcess;
using skydiag::tests::runtime::MakeSharedLayout;
using skydiag::tests::runtime::MakeTempDir;
using skydiag::tests::runtime::Require;
using skydiag::tests::runtime::TerminateChildProcess;

namespace {

CONTEXT CaptureChildThreadContext(DWORD tid)
{
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_FULL;
  HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
  Require(thread != nullptr, "OpenThread failed");
  Require(SuspendThread(thread) != static_cast<DWORD>(-1), "SuspendThread failed");
  const BOOL ok = GetThreadContext(thread, &ctx);
  ResumeThread(thread);
  CloseHandle(thread);
  Require(ok != FALSE, "GetThreadContext failed");
  return ctx;
}

bool WriteCrashDump(
  HANDLE process,
  DWORD pid,
  DWORD tid,
  const CONTEXT& ctx,
  DumpMode mode,
  const std::filesystem::path& path,
  std::wstring* err)
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
  const auto profile = ResolveDumpProfile(mode, CaptureKind::Crash);
  return WriteDumpWithStreams(
    process, pid, path.wstring(), shared.get(), sizeof(skydiag::SharedLayout), {}, {}, true, profile, false, err);
}

// A CTD through a dangling function pointer leaves RIP in unmapped memory, and
// a smashed stack leaves RSP there. The crash dump must still be written with
// the production profile; the evidence would otherwise be lost for exactly the
// crashes that need it most.
void TestCrashDumpSurvivesUnmappedRegisterTargets()
{
  const auto outBase = MakeTempDir(L"skydiag_crash_dump_context");
  auto child = LaunchSleepingChildProcess();
  const DWORD pid = child.pi.dwProcessId;
  const DWORD tid = child.pi.dwThreadId;
  const CONTEXT real = CaptureChildThreadContext(tid);

  CONTEXT ripNullPage = real;
  ripNullPage.Rip = 0x10;
  CONTEXT ripUnmapped = real;
  ripUnmapped.Rip = 0x00007000'00000000ull;
  CONTEXT rspNullPage = real;
  rspNullPage.Rsp = 0x10;

  struct Case
  {
    const char* name;
    const CONTEXT* ctx;
  };
  const Case cases[] = {
    { "child thread context", &real },
    { "RIP in the null page", &ripNullPage },
    { "RIP in unmapped memory", &ripUnmapped },
    { "RSP in the null page", &rspNullPage },
  };

  int index = 0;
  for (const auto mode : { DumpMode::kMini, DumpMode::kDefault }) {
    for (const auto& c : cases) {
      std::wstring err;
      const auto path = outBase / (L"case" + std::to_wstring(index++) + L".dmp");
      const bool ok = WriteCrashDump(child.pi.hProcess, pid, tid, *c.ctx, mode, path, &err);
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
    }
  }

  TerminateChildProcess(&child);
  std::filesystem::remove_all(outBase);
}

}  // namespace

int main()
{
  try {
    TestCrashDumpSurvivesUnmappedRegisterTargets();
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
