#include "Bucket.h"

#include <cassert>
#include <string>
#include <vector>

using skydiag::dump_tool::ComputeCrashBucketKey;
using skydiag::dump_tool::CrashBucketFrame;

static void Test_SameInput_ProducesStableKey()
{
  const std::vector<CrashBucketFrame> frames = {
    { L"hdtSMP64.dll", 0x12 },
    { L"SkyrimSE.exe", 0x123456 },
  };

  const auto a = ComputeCrashBucketKey(
    /*exceptionCode=*/0xC0000005u,
    /*faultModule=*/L"hdtSMP64.dll",
    /*faultModuleOffset=*/0x12,
    frames);
  const auto b = ComputeCrashBucketKey(
    /*exceptionCode=*/0xC0000005u,
    /*faultModule=*/L"hdtSMP64.dll",
    /*faultModuleOffset=*/0x12,
    frames);

  assert(!a.empty());
  assert(a.starts_with(L"CTD3-"));
  assert(a == b);
}

static void Test_DifferentTopFrame_ChangesKey()
{
  const std::vector<CrashBucketFrame> framesA = {
    { L"A.dll", 0x1 },
    { L"SkyrimSE.exe", 0x100 },
  };
  const std::vector<CrashBucketFrame> framesB = {
    { L"B.dll", 0x1 },
    { L"SkyrimSE.exe", 0x100 },
  };

  const auto a = ComputeCrashBucketKey(0xC0000005u, L"A.dll", 0x1, framesA);
  const auto b = ComputeCrashBucketKey(0xC0000005u, L"A.dll", 0x1, framesB);

  assert(a != b);
}

static void Test_DifferentExceptionCode_ChangesKey()
{
  const std::vector<CrashBucketFrame> frames = {
    { L"A.dll", 0x1 },
  };

  const auto av = ComputeCrashBucketKey(0xC0000005u, L"A.dll", 0x1, frames);
  const auto so = ComputeCrashBucketKey(0xC00000FDu, L"A.dll", 0x1, frames);

  assert(av != so);
}

static void Test_DifferentModuleOffset_ChangesKey()
{
  const std::vector<CrashBucketFrame> framesA = { { L"A.dll", 0x10 } };
  const std::vector<CrashBucketFrame> framesB = { { L"A.dll", 0x20 } };
  const auto a = ComputeCrashBucketKey(0xC0000005u, L"A.dll", 0x10, framesA);
  const auto b = ComputeCrashBucketKey(0xC0000005u, L"A.dll", 0x20, framesB);
  assert(a != b);
}

static void Test_CaseDifferences_DoNotChangeCanonicalKey()
{
  const std::vector<CrashBucketFrame> framesA = { { L"Example.dll", 0x123 } };
  const std::vector<CrashBucketFrame> framesB = { { L"EXAMPLE.DLL", 0x123 } };
  const auto a = ComputeCrashBucketKey(0xC0000005u, L"Example.dll", 0x123, framesA);
  const auto b = ComputeCrashBucketKey(0xC0000005u, L"example.DLL", 0x123, framesB);
  assert(a == b);
}

static void Test_HangKey_IsStableAndSeparateFromCrashKeys()
{
  const auto a = skydiag::dump_tool::ComputeHangBucketKey("v=1|kind=engine_wait|detail=gpu_query_poll");
  const auto b = skydiag::dump_tool::ComputeHangBucketKey("v=1|kind=engine_wait|detail=gpu_query_poll");
  const auto c = skydiag::dump_tool::ComputeHangBucketKey("v=1|kind=graphics_driver_wait|detail=");
  assert(a == b);
  assert(a != c);
  assert(a.starts_with(L"HANG1-") && a.size() == 6 + 16);
}

static void Test_LockOwnerKey_GroupsByTheOwnerThread()
{
  using skydiag::dump_tool::ComputeHangBucketKey;
  using skydiag::dump_tool::ComputeLockOwnerBucketKey;
  using skydiag::dump_tool::ThreadNameClass;

  const auto a = ComputeLockOwnerBucketKey("lock_owner_wait", L"nvwgf2umx.dll", L"nv.sl.dlss_g.present");
  assert(a.starts_with(L"HANG1-") && a.size() == 6 + 16);
  assert(a == ComputeLockOwnerBucketKey("lock_owner_wait", L"nvwgf2umx.dll", L"nv.sl.dlss_g.present"));
  // Case, padding and thread numbering change between runs; the owner does not.
  assert(a == ComputeLockOwnerBucketKey("lock_owner_wait", L" NVWGF2UMX.DLL ", L"NV.SL.DLSS_G.Present"));
  const auto w1 = ComputeLockOwnerBucketKey("lock_owner_wait", L"SomePlugin.dll", L"Worker 3");
  assert(w1 == ComputeLockOwnerBucketKey("lock_owner_wait", L"SomePlugin.dll", L"Worker 17"));
  assert(ThreadNameClass(L"Worker 17") == "worker #");
  assert(ThreadNameClass(L"") == "");

  // A different owner module, thread kind or state is a different bucket.
  assert(a != ComputeLockOwnerBucketKey("lock_owner_wait", L"amdxx64.dll", L"nv.sl.dlss_g.present"));
  assert(a != ComputeLockOwnerBucketKey("lock_owner_wait", L"nvwgf2umx.dll", L"BSJobs::JobThread"));
  assert(a != ComputeLockOwnerBucketKey("crash_logger_thread_dump_deadlock", L"nvwgf2umx.dll", L"nv.sl.dlss_g.present"));
  assert(w1 != ComputeLockOwnerBucketKey("lock_owner_wait", L"OtherPlugin.dll", L"Worker 3"));

  // Never the key of a main-thread wait.
  assert(a != ComputeHangBucketKey("v=1|kind=graphics_driver_wait|detail=|module=nvwgf2umx.dll|api="));
}

int main()
{
  Test_SameInput_ProducesStableKey();
  Test_DifferentTopFrame_ChangesKey();
  Test_DifferentExceptionCode_ChangesKey();
  Test_DifferentModuleOffset_ChangesKey();
  Test_CaseDifferences_DoNotChangeCanonicalKey();
  Test_HangKey_IsStableAndSeparateFromCrashKeys();
  Test_LockOwnerKey_GroupsByTheOwnerThread();
  return 0;
}
