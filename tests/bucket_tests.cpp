#include "Bucket.h"

#include <cassert>
#include <string>
#include <vector>

using skydiag::dump_tool::ComputeCrashBucketKey;

static void Test_SameInput_ProducesStableKey()
{
  const std::vector<std::wstring> frames = {
    L"hdtSMP64.dll!SomeFunction+0x12",
    L"SkyrimSE.exe+0x123456",
  };

  const auto a = ComputeCrashBucketKey(
    /*exceptionCode=*/0xC0000005u,
    /*faultModule=*/L"hdtSMP64.dll",
    frames);
  const auto b = ComputeCrashBucketKey(
    /*exceptionCode=*/0xC0000005u,
    /*faultModule=*/L"hdtSMP64.dll",
    frames);

  assert(!a.empty());
  assert(a == b);
}

static void Test_DifferentTopFrame_ChangesKey()
{
  const std::vector<std::wstring> framesA = {
    L"A.dll!Foo+0x1",
    L"SkyrimSE.exe+0x100",
  };
  const std::vector<std::wstring> framesB = {
    L"B.dll!Bar+0x1",
    L"SkyrimSE.exe+0x100",
  };

  const auto a = ComputeCrashBucketKey(0xC0000005u, L"A.dll", framesA);
  const auto b = ComputeCrashBucketKey(0xC0000005u, L"A.dll", framesB);

  assert(a != b);
}

static void Test_DifferentExceptionCode_ChangesKey()
{
  const std::vector<std::wstring> frames = {
    L"A.dll!Foo+0x1",
  };

  const auto av = ComputeCrashBucketKey(0xC0000005u, L"A.dll", frames);
  const auto so = ComputeCrashBucketKey(0xC00000FDu, L"A.dll", frames);

  assert(av != so);
}

int main()
{
  Test_SameInput_ProducesStableKey();
  Test_DifferentTopFrame_ChangesKey();
  Test_DifferentExceptionCode_ChangesKey();
  return 0;
}

