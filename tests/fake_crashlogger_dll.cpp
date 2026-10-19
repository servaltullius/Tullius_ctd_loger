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

// CrashLogger also calls game code while writing its report (a form's name,
// for example), and that code can fault outside CrashLogger's image. The call
// is not a tail call, so this module's return address stays on the stack
// while `callee` runs, as CrashLogger's frames do.
extern "C" __declspec(dllexport) __declspec(noinline) int SkydiagFakeCrashLoggerCallGameCode(int (*callee)())
{
  volatile int result = callee();
  return result + 1;
}
