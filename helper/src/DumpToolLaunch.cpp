#include "DumpToolLaunch.h"

#include <Windows.h>

#include <filesystem>
#include <iostream>
#include <string>

#include "ProcessUtil.h"
#include "HelperLog.h"
#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/DumpToolResolve.h"

namespace skydiag::helper::internal {
namespace {

std::filesystem::path GetThisExeDir()
{
  wchar_t buf[MAX_PATH]{};
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::filesystem::path p(buf, buf + n);
  return p.parent_path();
}

std::filesystem::path ResolveDumpToolExe(const skydiag::helper::HelperConfig& cfg)
{
  const auto baseDir = GetThisExeDir();

  auto toAbs = [&](const std::filesystem::path& p) {
    if (p.empty()) {
      return std::filesystem::path{};
    }
    return p.is_relative() ? (baseDir / p) : p;
  };

  std::filesystem::path configured =
    cfg.dumpToolExe.empty() ? L"SkyrimDiagWinUI\\SkyrimDiagDumpToolWinUI.exe" : cfg.dumpToolExe;
  std::filesystem::path resolved = toAbs(configured);
  if (!resolved.empty() && std::filesystem::exists(resolved)) {
    return resolved;
  }

  const std::filesystem::path fallbackWinUiSubdir = baseDir / L"SkyrimDiagWinUI" / L"SkyrimDiagDumpToolWinUI.exe";
  if (std::filesystem::exists(fallbackWinUiSubdir)) {
    return fallbackWinUiSubdir;
  }

  const std::filesystem::path fallbackWinUiFlat = baseDir / L"SkyrimDiagDumpToolWinUI.exe";
  if (std::filesystem::exists(fallbackWinUiFlat)) {
    return fallbackWinUiFlat;
  }

  // Last-resort behavior: return configured path even if missing so error logging
  // still reports the intended executable path.
  return resolved;
}

std::filesystem::path ResolveDumpToolHeadlessExeForConfig(const skydiag::helper::HelperConfig& cfg)
{
  const auto baseDir = GetThisExeDir();
  const auto winuiExe = ResolveDumpToolExe(cfg);
  const auto headless = skydiag::helper::ResolveDumpToolHeadlessExe(baseDir, winuiExe, /*overrideExe=*/{});
  return headless.empty() ? winuiExe : headless;
}

bool StartDumpToolProcessWithHandle(
  const std::filesystem::path& exe,
  std::wstring cmd,
  const std::filesystem::path& outBase,
  DWORD createFlags,
  HANDLE* outProcess,
  std::wstring* err,
  DWORD* outLastError)
{
  if (outProcess) {
    *outProcess = nullptr;
  }
  if (outLastError) {
    *outLastError = ERROR_SUCCESS;
  }
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  const std::wstring cwd = outBase.wstring();

  const BOOL ok = CreateProcessW(
    exe.c_str(),
    cmd.data(),
    nullptr,
    nullptr,
    FALSE,
    createFlags,
    nullptr,
    cwd.empty() ? nullptr : cwd.c_str(),
    &si,
    &pi);

  if (!ok) {
    const DWORD le = GetLastError();
    if (outLastError) {
      *outLastError = le;
    }
    if (err) {
      *err = L"CreateProcessW failed: " + std::to_wstring(le);
    }
    return false;
  }

  CloseHandle(pi.hThread);
  if (outProcess) {
    *outProcess = pi.hProcess;
  } else {
    CloseHandle(pi.hProcess);
  }
  if (err) err->clear();
  return true;
}

bool StartDumpToolProcess(
  const std::filesystem::path& exe,
  std::wstring cmd,
  const std::filesystem::path& outBase,
  DWORD createFlags,
  std::wstring* err,
  DWORD* outLastError = nullptr)
{
  return StartDumpToolProcessWithHandle(exe, std::move(cmd), outBase, createFlags, nullptr, err, outLastError);
}

void AppendOnlineSymbolFlag(std::wstring* cmd, bool allowOnlineSymbols)
{
  if (!cmd) {
    return;
  }
  *cmd += allowOnlineSymbols ? L" --allow-online-symbols" : L" --no-online-symbols";
}

}  // namespace

void StartDumpToolHeadlessIfConfigured(
  const skydiag::helper::HelperConfig& cfg,
  const std::wstring& dumpPath,
  const std::filesystem::path& outBase)
{
  if (!cfg.autoAnalyzeDump) {
    return;
  }

  const auto exe = ResolveDumpToolHeadlessExeForConfig(cfg);
  std::wstring cmd =
    QuoteArg(exe.wstring()) + L" " + QuoteArg(dumpPath) + L" --out-dir " + QuoteArg(outBase.wstring()) + L" --headless";
  AppendOnlineSymbolFlag(&cmd, cfg.allowOnlineSymbols);

  std::wstring err;
  if (!StartDumpToolProcess(exe, std::move(cmd), outBase, CREATE_NO_WINDOW, &err)) {
    std::wcerr << L"[SkyrimDiagHelper] Warning: failed to start DumpTool headless: " << err << L"\n";
  }
}

bool StartDumpToolHeadlessAsync(
  const skydiag::helper::HelperConfig& cfg,
  const std::wstring& dumpPath,
  const std::filesystem::path& outBase,
  HANDLE* outProcess,
  std::wstring* err)
{
  if (outProcess) {
    *outProcess = nullptr;
  }
  if (!cfg.autoAnalyzeDump) {
    if (err) {
      *err = L"autoAnalyzeDump disabled";
    }
    return false;
  }

  const auto exe = ResolveDumpToolHeadlessExeForConfig(cfg);
  std::wstring cmd =
    QuoteArg(exe.wstring()) + L" " + QuoteArg(dumpPath) + L" --out-dir " + QuoteArg(outBase.wstring()) + L" --headless";
  AppendOnlineSymbolFlag(&cmd, cfg.allowOnlineSymbols);
  return StartDumpToolProcessWithHandle(exe, std::move(cmd), outBase, CREATE_NO_WINDOW, outProcess, err, nullptr);
}

DumpToolViewerLaunchResult StartDumpToolViewer(
  const skydiag::helper::HelperConfig& cfg,
  const std::wstring& dumpPath,
  const std::filesystem::path& outBase,
  std::wstring_view reason)
{
  const auto exe = ResolveDumpToolExe(cfg);
  std::wstring cmd = QuoteArg(exe.wstring()) + L" " + QuoteArg(dumpPath) + L" --out-dir " + QuoteArg(outBase.wstring());
  AppendOnlineSymbolFlag(&cmd, cfg.allowOnlineSymbols);
  cmd += cfg.autoOpenViewerBeginnerMode ? L" --simple-ui" : L" --advanced-ui";

  std::wstring err;
  DWORD launchError = ERROR_SUCCESS;
  HANDLE process = nullptr;
  if (!StartDumpToolProcessWithHandle(exe, std::move(cmd), outBase, 0, &process, &err, &launchError)) {
    AppendLogLine(
      outBase,
      L"DumpTool viewer launch failed (reason="
        + std::wstring(reason)
        + L", dump="
        + std::filesystem::path(dumpPath).filename().wstring()
        + L", exe="
        + exe.wstring()
        + L", win32_error="
        + std::to_wstring(launchError)
        + L", detail="
        + err
        + L").");
    std::wcerr << L"[SkyrimDiagHelper] Warning: failed to start DumpTool viewer (" << reason << L"): " << err << L"\n";
    return DumpToolViewerLaunchResult::kLaunchFailed;
  }

  AppendLogLine(
    outBase,
    L"DumpTool viewer launch succeeded (reason="
      + std::wstring(reason)
      + L", dump="
      + std::filesystem::path(dumpPath).filename().wstring()
      + L", exe="
      + exe.wstring()
      + L").");

  // Distinguish "launch succeeded but viewer immediately exited" from true success.
  // This is commonly caused by missing WinUI/.NET runtimes or a startup crash.
  DumpToolViewerLaunchResult result = DumpToolViewerLaunchResult::kLaunched;
  if (process) {
    const DWORD w = WaitForSingleObject(process, 500);
    if (w == WAIT_OBJECT_0) {
      DWORD exitCode = 0;
      if (!GetExitCodeProcess(process, &exitCode)) {
        exitCode = 0xFFFFFFFFu;
      }
      AppendLogLine(
        outBase,
        L"DumpTool viewer exited immediately (reason="
          + std::wstring(reason)
          + L", exit_code="
          + std::to_wstring(exitCode)
          + L", dump="
          + std::filesystem::path(dumpPath).filename().wstring()
          + L", exe="
          + exe.wstring()
          + L"). Possible missing runtime (.NET Desktop Runtime 8 x64 / Windows App Runtime 1.8 x64) or startup crash "
            L"(check SkyrimDiagDumpToolWinUI_startup_error.log next to the viewer).");
      result = DumpToolViewerLaunchResult::kExitedImmediately;
    }
    CloseHandle(process);
  }

  return result;
}

}  // namespace skydiag::helper::internal
