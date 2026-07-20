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
  assert(a.starts_with(L"CTD2-"));
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

int main()
{
  Test_SameInput_ProducesStableKey();
  Test_DifferentTopFrame_ChangesKey();
  Test_DifferentExceptionCode_ChangesKey();
  Test_DifferentModuleOffset_ChangesKey();
  Test_CaseDifferences_DoNotChangeCanonicalKey();
  return 0;
}
