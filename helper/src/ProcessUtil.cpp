#include "ProcessUtil.h"

#include "SkyrimDiagHandle.h"
#include "SkyrimDiagLongPath.h"

namespace skydiag::helper::internal {

std::wstring QuoteArg(std::wstring_view s)
{
  // CommandLineToArgvW and the CRT read backslashes literally except before a
  // quote, where each pair is one backslash. So backslashes before a quote in
  // the value, and before the closing quote (OutputDir=D:\ ends in one), are
  // doubled; "D:\" alone would swallow the closing quote and the next arguments.
  std::wstring out;
  out.reserve(s.size() + 2);
  out.push_back(L'"');
  std::size_t backslashes = 0;
  for (const wchar_t c : s) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    out.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
    backslashes = 0;
    out.push_back(c);
  }
  out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

std::wstring WorkingDirectoryFor(const std::filesystem::path& dir)
{
  std::wstring plain = skydiag::PlainPath(dir).wstring();
  return plain.size() <= skydiag::kMaxPlainPathChars ? plain : std::wstring{};
}

bool RunHiddenProcessAndWait(std::wstring cmdLine, const std::filesystem::path& cwd, DWORD timeoutMs, std::wstring* err)
{
  return RunHiddenProcessAndWait(std::wstring{}, std::move(cmdLine), cwd, timeoutMs, err);
}

bool RunHiddenProcessAndWait(
  const std::wstring& applicationPath,
  std::wstring cmdLine,
  const std::filesystem::path& cwd,
  DWORD timeoutMs,
  std::wstring* err)
{
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};

  std::wstring cwdW = WorkingDirectoryFor(cwd);
  const BOOL ok = CreateProcessW(
    /*lpApplicationName=*/applicationPath.empty() ? nullptr : applicationPath.c_str(),
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

  skydiag::UniqueHandle thread(pi.hThread);
  skydiag::UniqueHandle process(pi.hProcess);
  thread.reset();  // not needed

  const DWORD w = WaitForSingleObject(process.get(), timeoutMs);
  if (w == WAIT_TIMEOUT) {
    TerminateProcess(process.get(), 1);
    if (err) *err = L"Process timeout";
    return false;
  }
  if (w == WAIT_FAILED) {
    if (err) *err = L"WaitForSingleObject failed: " + std::to_wstring(GetLastError());
    return false;
  }

  DWORD exitCode = 0;
  if (!GetExitCodeProcess(process.get(), &exitCode)) {
    if (err) *err = L"GetExitCodeProcess failed: " + std::to_wstring(GetLastError());
    return false;
  }

  if (exitCode != 0) {
    if (err) *err = L"Process exited with code: " + std::to_wstring(exitCode);
    return false;
  }

  if (err) err->clear();
  return true;
}

}  // namespace skydiag::helper::internal
