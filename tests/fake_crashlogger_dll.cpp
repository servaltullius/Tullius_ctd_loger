// Stand-in for CrashLogger.dll in plugin crash handler runtime tests. The real
// CrashLogger probes memory while writing its report and catches the access
// violations that causes; this probe does the same from inside a module whose
// file name is CrashLogger.dll.

#include <Windows.h>

namespace {

int ReadThroughBadPointer(volatile int* pointer)
{
  __try {
    return *pointer;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

}  // namespace

extern "C" __declspec(dllexport) int SkydiagFakeCrashLoggerProbe()
{
  return ReadThroughBadPointer(reinterpret_cast<volatile int*>(static_cast<ULONG_PTR>(0x10)));
}
