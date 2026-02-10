#include "ProcessUtil.h"

namespace skydiag::helper::internal {

std::wstring QuoteArg(std::wstring_view s)
{
  std::wstring out;
  out.reserve(s.size() + 2);
  out.push_back(L'"');
  for (const wchar_t c : s) {
    if (c == L'"') {
      out.push_back(L'\\');
    }
    out.push_back(c);
  }
  out.push_back(L'"');
  return out;
}

bool RunHiddenProcessAndWait(std::wstring cmdLine, const std::filesystem::path& cwd, DWORD timeoutMs, std::wstring* err)
{
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};

  std::wstring cwdW = cwd.wstring();
  const BOOL ok = CreateProcessW(
    /*lpApplicationName=*/nullptr,
    cmdLine.data(),
    nullptr,
    nullptr,
    FALSE,
    CREATE_NO_WINDOW,
    nullptr,
    cwdW.empty() ? nullptr : cwdW.c_str(),
    &si,
    &pi);

  if (!ok) {
    if (err) *err = L"CreateProcessW failed: " + std::to_wstring(GetLastError());
    return false;
  }

  CloseHandle(pi.hThread);

  const DWORD w = WaitForSingleObject(pi.hProcess, timeoutMs);
  if (w == WAIT_TIMEOUT) {
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    if (err) *err = L"Process timeout";
    return false;
  }
  if (w == WAIT_FAILED) {
    const DWORD le = GetLastError();
    CloseHandle(pi.hProcess);
    if (err) *err = L"WaitForSingleObject failed: " + std::to_wstring(le);
    return false;
  }

  DWORD exitCode = 0;
  if (!GetExitCodeProcess(pi.hProcess, &exitCode)) {
    const DWORD le = GetLastError();
    CloseHandle(pi.hProcess);
    if (err) *err = L"GetExitCodeProcess failed: " + std::to_wstring(le);
    return false;
  }
  CloseHandle(pi.hProcess);

  if (exitCode != 0) {
    if (err) *err = L"Process exited with code: " + std::to_wstring(exitCode);
    return false;
  }

  if (err) err->clear();
  return true;
}

}  // namespace skydiag::helper::internal

