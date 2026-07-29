#include <Windows.h>
#include <DbgHelp.h>

#include <iostream>

int wmain(int argc, wchar_t** argv)
{
  if (argc != 2 || argv == nullptr || argv[1] == nullptr) {
    std::wcerr << L"Usage: skydiag_minidump_fixture_writer.exe <output.dmp>\n";
    return 2;
  }

  const HANDLE output = CreateFileW(
    argv[1],
    GENERIC_WRITE,
    0,
    nullptr,
    CREATE_ALWAYS,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  if (output == INVALID_HANDLE_VALUE) {
    std::wcerr << L"CreateFileW failed: " << GetLastError() << L"\n";
    return 3;
  }

  const auto dumpType = static_cast<MINIDUMP_TYPE>(
    MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
  const BOOL wrote = MiniDumpWriteDump(
    GetCurrentProcess(),
    GetCurrentProcessId(),
    output,
    dumpType,
    nullptr,
    nullptr,
    nullptr);
  const DWORD error = wrote ? ERROR_SUCCESS : GetLastError();
  CloseHandle(output);

  if (!wrote) {
    std::wcerr << L"MiniDumpWriteDump failed: " << error << L"\n";
    return 4;
  }
  return 0;
}
