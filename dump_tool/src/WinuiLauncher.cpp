#include <Windows.h>
#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::filesystem::path GetCurrentExeDir()
{
  std::vector<wchar_t> buffer(32768, L'\0');
  const DWORD copied = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (copied == 0 || copied >= buffer.size()) {
    return {};
  }

  return std::filesystem::path(std::wstring_view(buffer.data(), copied)).parent_path();
}

std::wstring QuoteArg(std::wstring_view arg)
{
  if (arg.empty()) {
    return L"\"\"";
  }

  const bool needsQuotes = arg.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
  if (!needsQuotes) {
    return std::wstring(arg);
  }

  std::wstring quoted;
  quoted.push_back(L'"');

  int backslashes = 0;
  for (const wchar_t ch : arg) {
    if (ch == L'\\') {
      ++backslashes;
      continue;
    }

    if (ch == L'"') {
      quoted.append(static_cast<std::size_t>(backslashes * 2 + 1), L'\\');
      quoted.push_back(ch);
      backslashes = 0;
      continue;
    }

    quoted.append(static_cast<std::size_t>(backslashes), L'\\');
    backslashes = 0;
    quoted.push_back(ch);
  }

  quoted.append(static_cast<std::size_t>(backslashes * 2), L'\\');
  quoted.push_back(L'"');
  return quoted;
}

bool HasArg(int argc, wchar_t** argv, std::wstring_view expected)
{
  for (int i = 1; i < argc; ++i) {
    if (argv[i] && _wcsicmp(argv[i], std::wstring(expected).c_str()) == 0) {
      return true;
    }
  }
  return false;
}

void WriteLauncherError(const std::filesystem::path& launcherDir, std::wstring_view message)
{
  const auto logPath = launcherDir / L"SkyrimDiagDumpToolWinUI_launcher_error.log";
  std::wofstream log(logPath, std::ios::app);
  if (!log) {
    return;
  }

  SYSTEMTIME now{};
  GetLocalTime(&now);
  log << L"["
      << now.wYear << L"-"
      << now.wMonth << L"-"
      << now.wDay << L" "
      << now.wHour << L":"
      << now.wMinute << L":"
      << now.wSecond << L"] "
      << message << L"\n";
}

int Fail(const std::filesystem::path& launcherDir, std::wstring message, bool headless)
{
  WriteLauncherError(launcherDir, message);
  if (!headless) {
    MessageBoxW(nullptr, message.c_str(), L"SkyrimDiag WinUI launcher", MB_ICONERROR | MB_OK);
  }
  return 2;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  const bool headless = argv && HasArg(argc, argv, L"--headless");

  const auto launcherDir = GetCurrentExeDir();
  if (launcherDir.empty()) {
    if (argv) {
      LocalFree(argv);
    }
    return 2;
  }

  const auto appExe = launcherDir / L"app" / L"SkyrimDiagDumpToolWinUI.exe";
  if (!std::filesystem::is_regular_file(appExe)) {
    if (argv) {
      LocalFree(argv);
    }
    return Fail(launcherDir, L"Missing WinUI app executable: " + appExe.wstring(), headless);
  }

  std::wstring commandLine = QuoteArg(appExe.wstring());
  if (argv) {
    for (int i = 1; i < argc; ++i) {
      commandLine.push_back(L' ');
      commandLine += QuoteArg(argv[i] ? std::wstring_view(argv[i]) : std::wstring_view{});
    }
  }

  if (argv) {
    LocalFree(argv);
  }

  STARTUPINFOW startupInfo{};
  startupInfo.cb = sizeof(startupInfo);
  PROCESS_INFORMATION processInfo{};

  const DWORD createFlags = headless ? CREATE_NO_WINDOW : 0;
  std::wstring mutableCommandLine = commandLine;
  const BOOL started = CreateProcessW(
    appExe.c_str(),
    mutableCommandLine.data(),
    nullptr,
    nullptr,
    FALSE,
    createFlags,
    nullptr,
    nullptr,
    &startupInfo,
    &processInfo);

  if (!started) {
    const DWORD error = GetLastError();
    return Fail(
      launcherDir,
      L"Failed to launch WinUI app: "
        + appExe.wstring()
        + L" (win32_error="
        + std::to_wstring(error)
        + L")",
      headless);
  }

  CloseHandle(processInfo.hThread);
  WaitForSingleObject(processInfo.hProcess, INFINITE);

  DWORD exitCode = 0;
  if (!GetExitCodeProcess(processInfo.hProcess, &exitCode)) {
    exitCode = 1;
  }
  CloseHandle(processInfo.hProcess);
  return static_cast<int>(exitCode);
}
