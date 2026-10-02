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

  std::size_t backslashes = 0;
  for (const wchar_t ch : arg) {
    if (ch == L'\\') {
      ++backslashes;
      continue;
    }

    if (ch == L'"') {
      quoted.append(backslashes * 2u + 1u, L'\\');
      quoted.push_back(ch);
      backslashes = 0;
      continue;
    }

    quoted.append(backslashes, L'\\');
    backslashes = 0;
    quoted.push_back(ch);
  }

  quoted.append(backslashes * 2u, L'\\');
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

int Fail(const std::filesystem::path& launcherDir, const std::wstring& message, bool headless)
{
  WriteLauncherError(launcherDir, message);
  if (!headless) {
    MessageBoxW(nullptr, message.c_str(), L"SkyrimDiag WinUI launcher", MB_ICONERROR | MB_OK);
  }
  return 2;
}

bool IsRunningUnderWine()
{
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  return ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

struct DumpArgs
{
  std::filesystem::path dump;
  std::filesystem::path outDir;
};

DumpArgs ParseDumpArgs(int argc, wchar_t** argv)
{
  DumpArgs args{};
  for (int i = 1; argv && i < argc; ++i) {
    if (!argv[i]) {
      continue;
    }
    const std::wstring_view arg(argv[i]);
    if (_wcsicmp(argv[i], L"--out-dir") == 0 && i + 1 < argc && argv[i + 1]) {
      args.outDir = argv[++i];
      continue;
    }
    if (args.dump.empty() && !arg.starts_with(L"--")) {
      const std::filesystem::path candidate(arg);
      if (_wcsicmp(candidate.extension().c_str(), L".dmp") == 0) {
        args.dump = candidate;
      }
    }
  }
  if (args.outDir.empty() && !args.dump.empty()) {
    args.outDir = args.dump.parent_path();
  }
  return args;
}

std::filesystem::path ReportPathFor(const DumpArgs& args)
{
  return args.outDir / (args.dump.stem().wstring() + L"_SkyrimDiagReport.txt");
}

// Produces the text report with the headless CLI that ships next to the
// SkyrimDiagWinUI folder; the CLI does not depend on the Windows App Runtime.
void RunCliAnalysis(const std::filesystem::path& launcherDir, const DumpArgs& args)
{
  const auto cliExe = launcherDir.parent_path() / L"SkyrimDiagDumpToolCli.exe";
  if (!std::filesystem::is_regular_file(cliExe)) {
    return;
  }
  std::wstring commandLine = QuoteArg(cliExe.wstring()) + L" " + QuoteArg(args.dump.wstring()) +
    L" --out-dir " + QuoteArg(args.outDir.wstring());
  STARTUPINFOW startupInfo{};
  startupInfo.cb = sizeof(startupInfo);
  PROCESS_INFORMATION processInfo{};
  if (CreateProcessW(
        cliExe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
        &startupInfo, &processInfo)) {
    CloseHandle(processInfo.hThread);
    WaitForSingleObject(processInfo.hProcess, 5u * 60u * 1000u);
    CloseHandle(processInfo.hProcess);
  }
}

// The WinUI app needs the Windows App Runtime, whose WinRT dependencies Wine
// does not provide, so under Wine/Proton it dies during startup. Explain that
// and point at the text report instead of launching it.
int HandleWine(const std::filesystem::path& launcherDir, int argc, wchar_t** argv, bool headless)
{
  const DumpArgs args = ParseDumpArgs(argc, argv);
  std::wstring message =
    L"The SkyrimDiag WinUI viewer cannot run under Wine/Proton: it needs the Windows App Runtime, "
    L"which Wine does not provide.\n\n"
    L"Use the text report instead: *_SkyrimDiagReport.txt next to each dump. SkyrimDiagHelper writes it "
    L"automatically under Wine, or run SkyrimDiagDumpToolCli.exe \"<dump.dmp>\".";
  WriteLauncherError(launcherDir, L"Wine/Proton detected; WinUI viewer not started.");
  if (headless) {
    return 3;
  }

  if (!args.dump.empty()) {
    const auto report = ReportPathFor(args);
    if (!std::filesystem::is_regular_file(report)) {
      RunCliAnalysis(launcherDir, args);
    }
    if (std::filesystem::is_regular_file(report)) {
      message += L"\n\nOpen the report for this dump now?\n" + report.wstring();
      if (MessageBoxW(nullptr, message.c_str(), L"SkyrimDiag viewer", MB_ICONINFORMATION | MB_YESNO) == IDYES) {
        ShellExecuteW(nullptr, L"open", report.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      }
      return 3;
    }
  }

  MessageBoxW(nullptr, message.c_str(), L"SkyrimDiag viewer", MB_ICONINFORMATION | MB_OK);
  return 3;
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
      LocalFree(reinterpret_cast<HLOCAL>(argv));
    }
    return 2;
  }

  if (IsRunningUnderWine()) {
    const int code = HandleWine(launcherDir, argc, argv, headless);
    if (argv) {
      LocalFree(reinterpret_cast<HLOCAL>(argv));
    }
    return code;
  }

  const auto appExe = launcherDir / L"app" / L"SkyrimDiagDumpToolWinUI.exe";
  if (!std::filesystem::is_regular_file(appExe)) {
    if (argv) {
      LocalFree(reinterpret_cast<HLOCAL>(argv));
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
    LocalFree(reinterpret_cast<HLOCAL>(argv));
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
