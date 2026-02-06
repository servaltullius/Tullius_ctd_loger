#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/DumpWriter.h"
#include "SkyrimDiagHelper/HangSuppression.h"
#include "SkyrimDiagHelper/HangDetect.h"
#include "SkyrimDiagHelper/LoadStats.h"
#include "SkyrimDiagHelper/ProcessAttach.h"
#include "SkyrimDiagHelper/WctCapture.h"
#include "SkyrimDiagShared.h"

namespace {

std::wstring Timestamp()
{
  SYSTEMTIME st{};
  GetLocalTime(&st);

  wchar_t buf[64]{};
  swprintf_s(
    buf,
    L"%04u%02u%02u_%02u%02u%02u",
    st.wYear,
    st.wMonth,
    st.wDay,
    st.wHour,
    st.wMinute,
    st.wSecond);
  return buf;
}

std::filesystem::path MakeOutputBase(const skydiag::helper::HelperConfig& cfg)
{
  std::filesystem::path out(cfg.outputDir);
  std::error_code ec;
  std::filesystem::create_directories(out, ec);
  return out;
}

void WriteTextFileUtf8(const std::filesystem::path& path, const std::string& s)
{
  std::ofstream f(path, std::ios::binary);
  f.write(s.data(), static_cast<std::streamsize>(s.size()));
}

std::filesystem::path GetThisExeDir()
{
  wchar_t buf[MAX_PATH]{};
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::filesystem::path p(buf, buf + n);
  return p.parent_path();
}

std::string WideToUtf8(std::wstring_view s)
{
  if (s.empty()) {
    return {};
  }
  const int needed = WideCharToMultiByte(
    CP_UTF8,
    0,
    s.data(),
    static_cast<int>(s.size()),
    nullptr,
    0,
    nullptr,
    nullptr);
  if (needed <= 0) {
    return {};
  }
  std::string out(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(
    CP_UTF8,
    0,
    s.data(),
    static_cast<int>(s.size()),
    out.data(),
    needed,
    nullptr,
    nullptr);
  return out;
}

void AppendLogLine(const std::filesystem::path& outBase, std::wstring_view line)
{
  std::error_code ec;
  std::filesystem::create_directories(outBase, ec);

  const auto path = outBase / L"SkyrimDiagHelper.log";
  std::ofstream f(path, std::ios::binary | std::ios::app);
  if (!f) {
    return;
  }

  std::wstring msg(line);
  msg += L"\r\n";
  const auto utf8 = WideToUtf8(msg);
  if (!utf8.empty()) {
    f.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
  }
}

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

DWORD EtwTimeoutMs(const skydiag::helper::HelperConfig& cfg)
{
  std::uint32_t sec = cfg.etwMaxDurationSec;
  if (sec < 10u) {
    sec = 10u;
  }
  if (sec > 120u) {
    sec = 120u;
  }
  return static_cast<DWORD>(sec * 1000u);
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

bool StartEtwCaptureForHang(
  const skydiag::helper::HelperConfig& cfg,
  const std::filesystem::path& outBase,
  std::wstring* err)
{
  const std::wstring wprExe = cfg.etwWprExe.empty() ? L"wpr.exe" : cfg.etwWprExe;
  const std::wstring profile = cfg.etwProfile.empty() ? L"GeneralProfile" : cfg.etwProfile;
  std::wstring cmd = QuoteArg(wprExe) + L" -start " + QuoteArg(profile) + L" -filemode";
  return RunHiddenProcessAndWait(std::move(cmd), outBase, EtwTimeoutMs(cfg), err);
}

bool StopEtwCaptureForHang(
  const skydiag::helper::HelperConfig& cfg,
  const std::filesystem::path& outBase,
  const std::filesystem::path& etlPath,
  std::wstring* err)
{
  const std::wstring wprExe = cfg.etwWprExe.empty() ? L"wpr.exe" : cfg.etwWprExe;
  std::wstring cmd = QuoteArg(wprExe) + L" -stop " + QuoteArg(etlPath.wstring());
  return RunHiddenProcessAndWait(std::move(cmd), outBase, EtwTimeoutMs(cfg), err);
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

bool StartDumpToolProcess(
  const std::filesystem::path& exe,
  std::wstring cmd,
  const std::filesystem::path& outBase,
  DWORD createFlags,
  std::wstring* err)
{
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
    if (err) *err = L"CreateProcessW failed: " + std::to_wstring(GetLastError());
    return false;
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (err) err->clear();
  return true;
}

void StartDumpToolHeadlessIfConfigured(
  const skydiag::helper::HelperConfig& cfg,
  const std::wstring& dumpPath,
  const std::filesystem::path& outBase)
{
  if (!cfg.autoAnalyzeDump) {
    return;
  }

  const auto exe = ResolveDumpToolExe(cfg);
  std::wstring cmd =
    QuoteArg(exe.wstring()) + L" " + QuoteArg(dumpPath) + L" --out-dir " + QuoteArg(outBase.wstring()) + L" --headless";

  std::wstring err;
  if (!StartDumpToolProcess(exe, std::move(cmd), outBase, CREATE_NO_WINDOW, &err)) {
    std::wcerr << L"[SkyrimDiagHelper] Warning: failed to start DumpTool headless: " << err << L"\n";
  }
}

void StartDumpToolViewer(
  const skydiag::helper::HelperConfig& cfg,
  const std::wstring& dumpPath,
  const std::filesystem::path& outBase,
  std::wstring_view reason)
{
  const auto exe = ResolveDumpToolExe(cfg);
  std::wstring cmd = QuoteArg(exe.wstring()) + L" " + QuoteArg(dumpPath) + L" --out-dir " + QuoteArg(outBase.wstring());
  cmd += cfg.autoOpenViewerBeginnerMode ? L" --simple-ui" : L" --advanced-ui";

  std::wstring err;
  if (!StartDumpToolProcess(exe, std::move(cmd), outBase, 0, &err)) {
    std::wcerr << L"[SkyrimDiagHelper] Warning: failed to start DumpTool viewer (" << reason << L"): " << err << L"\n";
  }
}

bool IsPidInForeground(DWORD pid)
{
  const HWND fg = GetForegroundWindow();
  if (!fg) {
    return false;
  }

  HWND root = GetAncestor(fg, GA_ROOTOWNER);
  if (!root) {
    root = fg;
  }

  DWORD fgPid = 0;
  GetWindowThreadProcessId(root, &fgPid);
  return fgPid == pid;
}

struct FindWindowCtx
{
  DWORD pid = 0;
  HWND hwnd = nullptr;
};

BOOL CALLBACK EnumWindows_FindTopLevelForPid(HWND hwnd, LPARAM lParam)
{
  auto* ctx = reinterpret_cast<FindWindowCtx*>(lParam);
  if (!ctx) {
    return TRUE;
  }

  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != ctx->pid) {
    return TRUE;
  }

  // Prefer visible, unowned top-level windows as the "main" window.
  if (!IsWindowVisible(hwnd)) {
    return TRUE;
  }
  if (GetWindow(hwnd, GW_OWNER) != nullptr) {
    return TRUE;
  }

  ctx->hwnd = hwnd;
  return FALSE;  // stop
}

HWND FindMainWindowForPid(DWORD pid)
{
  FindWindowCtx ctx{};
  ctx.pid = pid;
  EnumWindows(EnumWindows_FindTopLevelForPid, reinterpret_cast<LPARAM>(&ctx));
  return ctx.hwnd;
}

bool IsWindowResponsive(HWND hwnd, UINT timeoutMs)
{
  if (!hwnd || !IsWindow(hwnd)) {
    return false;
  }

  DWORD_PTR result = 0;
  SetLastError(ERROR_SUCCESS);
  const LRESULT ok = SendMessageTimeoutW(
    hwnd,
    WM_NULL,
    0,
    0,
    SMTO_ABORTIFHUNG | SMTO_BLOCK,
    timeoutMs,
    &result);
  return ok != 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
  // Keep helper overhead minimal vs. the game.
  SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

  std::wstring err;
  const auto cfg = skydiag::helper::LoadConfig(&err);
  if (!err.empty()) {
    std::wcerr << L"[SkyrimDiagHelper] Config warning: " << err << L"\n";
  }

  skydiag::helper::AttachedProcess proc{};
  if (argc >= 3 && std::wstring_view(argv[1]) == L"--pid") {
    const auto pid = static_cast<std::uint32_t>(std::wcstoul(argv[2], nullptr, 10));
    if (!skydiag::helper::AttachByPid(pid, proc, &err)) {
      std::wcerr << L"[SkyrimDiagHelper] Attach failed: " << err << L"\n";
      return 2;
    }
  } else {
    if (!skydiag::helper::FindAndAttach(proc, &err)) {
      std::wcerr << L"[SkyrimDiagHelper] Attach failed: " << err << L"\n";
      return 2;
    }
  }

  if (!proc.shm || proc.shm->header.magic != skydiag::kMagic) {
    std::wcerr << L"[SkyrimDiagHelper] Shared memory invalid/missing.\n";
    skydiag::helper::Detach(proc);
    return 3;
  }
  if (proc.shm->header.version != skydiag::kVersion) {
    std::wcerr << L"[SkyrimDiagHelper] Shared memory version mismatch (got="
               << proc.shm->header.version << L", expected=" << skydiag::kVersion << L").\n";
    AppendLogLine(MakeOutputBase(cfg), L"Shared memory version mismatch (got=" + std::to_wstring(proc.shm->header.version) +
      L", expected=" + std::to_wstring(skydiag::kVersion) + L")");
    skydiag::helper::Detach(proc);
    return 3;
  }

  const auto outBase = MakeOutputBase(cfg);
  std::wcout << L"[SkyrimDiagHelper] Attached to pid=" << proc.pid << L", output=" << outBase.wstring() << L"\n";
  AppendLogLine(outBase, L"Attached to pid=" + std::to_wstring(proc.pid) + L", output=" + outBase.wstring());
  if (!err.empty()) {
    AppendLogLine(outBase, L"Config warning: " + err);
  }

  skydiag::helper::LoadStats loadStats;
  std::uint32_t adaptiveLoadingThresholdSec = cfg.hangThresholdLoadingSec;
  const auto loadStatsPath = outBase / L"SkyrimDiag_LoadStats.json";
  if (cfg.enableAdaptiveLoadingThreshold) {
    loadStats.LoadFromFile(loadStatsPath);
    adaptiveLoadingThresholdSec = loadStats.SuggestedLoadingThresholdSec(cfg);
    std::wcout << L"[SkyrimDiagHelper] Adaptive loading threshold: "
               << adaptiveLoadingThresholdSec << L"s (fallback=" << cfg.hangThresholdLoadingSec << L"s)\n";
  }

  constexpr int kHotkeyId = 0x5344;  // 'SD' (arbitrary)
  if (cfg.enableManualCaptureHotkey) {
    if (!RegisterHotKey(nullptr, kHotkeyId, MOD_CONTROL | MOD_SHIFT, VK_F12)) {
      const DWORD le = GetLastError();
      std::wcerr << L"[SkyrimDiagHelper] Warning: RegisterHotKey(Ctrl+Shift+F12) failed: " << le << L"\n";
      AppendLogLine(outBase, L"Warning: RegisterHotKey(Ctrl+Shift+F12) failed: " + std::to_wstring(le) +
        L" (falling back to GetAsyncKeyState polling)");
    } else {
      std::wcout << L"[SkyrimDiagHelper] Manual capture hotkey: Ctrl+Shift+F12\n";
      AppendLogLine(outBase, L"Manual capture hotkey registered: Ctrl+Shift+F12");
    }
  }

  LARGE_INTEGER attachNow{};
  QueryPerformanceCounter(&attachNow);
  const std::uint64_t attachNowQpc = static_cast<std::uint64_t>(attachNow.QuadPart);
  const std::uint64_t attachHeartbeatQpc = proc.shm->header.last_heartbeat_qpc;

  bool crashCaptured = false;
  bool hangCapturedThisEpisode = false;
  bool heartbeatEverAdvanced = false;
  bool warnedHeartbeatStale = false;
  bool hangSuppressedNotForegroundThisEpisode = false;
  bool hangSuppressedForegroundGraceThisEpisode = false;
  bool hangSuppressedForegroundResponsiveThisEpisode = false;
  skydiag::helper::HangSuppressionState hangSuppressionState{};
  HWND targetWindow = nullptr;
  std::wstring pendingHangViewerDumpPath;

  bool wasLoading = (proc.shm->header.state_flags & skydiag::kState_Loading) != 0u;
  std::uint64_t loadStartQpc = wasLoading ? proc.shm->header.start_qpc : 0;

  auto doManualCapture = [&](std::wstring_view trigger) {
    const auto ts = Timestamp();
    const auto wctPath = outBase / (L"SkyrimDiag_WCT_Manual_" + ts + L".json");
    const auto dumpPath = (outBase / (L"SkyrimDiag_Manual_" + ts + L".dmp")).wstring();

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const auto stateFlags = proc.shm->header.state_flags;
    const bool inMenu = (stateFlags & skydiag::kState_InMenu) != 0u;
    const std::uint32_t inGameThresholdSec = inMenu
      ? std::max(cfg.hangThresholdInGameSec, cfg.hangThresholdInMenuSec)
      : cfg.hangThresholdInGameSec;
    const std::uint32_t loadingThresholdSec = (cfg.enableAdaptiveLoadingThreshold && loadStats.HasSamples())
      ? adaptiveLoadingThresholdSec
      : cfg.hangThresholdLoadingSec;
    const auto decision = skydiag::helper::EvaluateHang(
      static_cast<std::uint64_t>(now.QuadPart),
      proc.shm->header.last_heartbeat_qpc,
      proc.shm->header.qpc_freq,
      stateFlags,
      inGameThresholdSec,
      loadingThresholdSec);

    AppendLogLine(outBase, L"Manual capture triggered via " + std::wstring(trigger) +
      L" (secondsSinceHeartbeat=" + std::to_wstring(decision.secondsSinceHeartbeat) +
      L", threshold=" + std::to_wstring(decision.thresholdSec) +
      L", loading=" + std::to_wstring(decision.isLoading ? 1 : 0) +
      L", inMenu=" + std::to_wstring(inMenu ? 1 : 0) + L")");

    nlohmann::json wctJson;
    std::wstring wctErr;
    if (!skydiag::helper::CaptureWct(proc.pid, wctJson, &wctErr)) {
      std::wcerr << L"[SkyrimDiagHelper] WCT capture failed: " << wctErr << L"\n";
      AppendLogLine(outBase, L"WCT capture failed: " + wctErr);
      wctJson = nlohmann::json::object();
      wctJson["pid"] = proc.pid;
      wctJson["error"] = "capture_failed";
    }
    if (wctJson.contains("debugPrivilegeEnabled") && wctJson["debugPrivilegeEnabled"].is_boolean() &&
        !wctJson["debugPrivilegeEnabled"].get<bool>()) {
      AppendLogLine(outBase, L"Warning: EnableDebugPrivilege failed; WCT capture may be incomplete.");
    }

    wctJson["capture"] = nlohmann::json::object();
    wctJson["capture"]["kind"] = "manual";
    wctJson["capture"]["secondsSinceHeartbeat"] = decision.secondsSinceHeartbeat;
    wctJson["capture"]["thresholdSec"] = decision.thresholdSec;
    wctJson["capture"]["isLoading"] = decision.isLoading;
    wctJson["capture"]["stateFlags"] = stateFlags;

    const std::string wctUtf8 = wctJson.dump(2);
    WriteTextFileUtf8(wctPath, wctUtf8);

    std::wstring dumpErr;
    if (!skydiag::helper::WriteDumpWithStreams(
          proc.process,
          proc.pid,
          dumpPath,
          proc.shm,
          proc.shmSize,
          wctUtf8,
          /*isCrash=*/false,
          cfg.dumpMode,
          &dumpErr)) {
      std::wcerr << L"[SkyrimDiagHelper] Manual dump failed: " << dumpErr << L"\n";
      AppendLogLine(outBase, L"Manual dump failed: " + dumpErr);
    } else {
      std::wcout << L"[SkyrimDiagHelper] Manual dump written: " << dumpPath << L"\n";
      std::wcout << L"[SkyrimDiagHelper] WCT written: " << wctPath.wstring() << L"\n";
      AppendLogLine(outBase, L"Manual dump written: " + dumpPath);
      AppendLogLine(outBase, L"WCT written: " + wctPath.wstring());
      StartDumpToolHeadlessIfConfigured(cfg, dumpPath, outBase);
      if (cfg.autoOpenViewerOnManualCapture) {
        StartDumpToolViewer(cfg, dumpPath, outBase, L"manual");
      }
    }
  };

  for (;;) {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_HOTKEY && static_cast<int>(msg.wParam) == kHotkeyId) {
        doManualCapture(L"WM_HOTKEY");
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }

    // Fallback manual hotkey detection: some environments can miss WM_HOTKEY even when RegisterHotKey succeeds.
    // Polling is low overhead (once per loop) and makes manual capture more reliable.
    if (cfg.enableManualCaptureHotkey) {
      const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
      const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
      if (ctrl && shift && ((GetAsyncKeyState(VK_F12) & 1) != 0)) {
        doManualCapture(L"GetAsyncKeyState");
      }
    }

    if (proc.process) {
      const DWORD w = WaitForSingleObject(proc.process, 0);
      if (w == WAIT_OBJECT_0) {
        std::wcerr << L"[SkyrimDiagHelper] Target process exited.\n";
        AppendLogLine(outBase, L"Target process exited.");
        if (!pendingHangViewerDumpPath.empty() && cfg.autoOpenViewerOnHang && cfg.autoOpenHangAfterProcessExit) {
          const DWORD delayMs = static_cast<DWORD>(std::min<std::uint32_t>(cfg.autoOpenHangDelayMs, 10000u));
          if (delayMs > 0) {
            Sleep(delayMs);
          }
          StartDumpToolViewer(cfg, pendingHangViewerDumpPath, outBase, L"hang_exit");
          AppendLogLine(outBase, L"Auto-opened DumpTool viewer for latest hang dump after process exit.");
        }
        break;
      }
      if (w == WAIT_FAILED) {
        const DWORD le = GetLastError();
        std::wcerr << L"[SkyrimDiagHelper] Target process wait failed (err=" << le << L").\n";
        AppendLogLine(outBase, L"Target process wait failed: " + std::to_wstring(le));
        break;
      }
    }

    DWORD waitMs = 250;
    if (proc.crashEvent) {
      const DWORD w = WaitForSingleObject(proc.crashEvent, waitMs);
      if (w == WAIT_OBJECT_0) {
        if (crashCaptured) {
          AppendLogLine(outBase, L"Crash event signaled again; ignoring (already captured).");
          continue;
        }

        const auto ts = Timestamp();
        const auto dumpPath = (outBase / (L"SkyrimDiag_Crash_" + ts + L".dmp")).wstring();
        pendingHangViewerDumpPath.clear();

        std::wstring dumpErr;
        if (!skydiag::helper::WriteDumpWithStreams(
              proc.process,
              proc.pid,
              dumpPath,
              proc.shm,
              proc.shmSize,
              /*wctJsonUtf8=*/{},
              /*isCrash=*/true,
              cfg.dumpMode,
              &dumpErr)) {
          std::wcerr << L"[SkyrimDiagHelper] Crash dump failed: " << dumpErr << L"\n";
        } else {
          std::wcout << L"[SkyrimDiagHelper] Crash dump written: " << dumpPath << L"\n";
          StartDumpToolHeadlessIfConfigured(cfg, dumpPath, outBase);
          if (cfg.autoOpenViewerOnCrash) {
            StartDumpToolViewer(cfg, dumpPath, outBase, L"crash");
          }
        }

        crashCaptured = true;
        AppendLogLine(outBase, L"Crash captured; waiting for process exit.");
        continue;
      }
    } else {
      Sleep(waitMs);
    }

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);

    const auto stateFlags = proc.shm->header.state_flags;

    if (cfg.enableAdaptiveLoadingThreshold) {
      const bool isLoading = (stateFlags & skydiag::kState_Loading) != 0u;
      if (!wasLoading && isLoading) {
        loadStartQpc = static_cast<std::uint64_t>(now.QuadPart);
      } else if (wasLoading && !isLoading && loadStartQpc != 0 && proc.shm->header.qpc_freq != 0) {
        const auto deltaQpc = static_cast<std::uint64_t>(now.QuadPart) - loadStartQpc;
        const double seconds = static_cast<double>(deltaQpc) / static_cast<double>(proc.shm->header.qpc_freq);
        const auto secRounded = static_cast<std::uint32_t>(seconds + 0.5);
        if (secRounded > 0) {
          loadStats.AddLoadingSampleSeconds(secRounded);
          loadStats.SaveToFile(loadStatsPath);
          adaptiveLoadingThresholdSec = loadStats.SuggestedLoadingThresholdSec(cfg);
          std::wcout << L"[SkyrimDiagHelper] Observed loading duration=" << secRounded
                     << L"s -> new adaptive threshold=" << adaptiveLoadingThresholdSec << L"s\n";
        }
        loadStartQpc = 0;
      }
      wasLoading = isLoading;
    }

    const std::uint32_t loadingThresholdSec = (cfg.enableAdaptiveLoadingThreshold && loadStats.HasSamples())
      ? adaptiveLoadingThresholdSec
      : cfg.hangThresholdLoadingSec;

    if (!heartbeatEverAdvanced && proc.shm->header.last_heartbeat_qpc > attachHeartbeatQpc) {
      heartbeatEverAdvanced = true;
    }
    if (!heartbeatEverAdvanced && !warnedHeartbeatStale && proc.shm->header.qpc_freq != 0) {
      const std::uint64_t deltaQpc = static_cast<std::uint64_t>(now.QuadPart) - attachNowQpc;
      const double secondsSinceAttach = static_cast<double>(deltaQpc) / static_cast<double>(proc.shm->header.qpc_freq);
      if (secondsSinceAttach >= 5.0) {
        warnedHeartbeatStale = true;
        AppendLogLine(outBase, L"Warning: heartbeat not advancing since attach; auto hang capture disabled (manual capture still works).");
      }
    }

    const auto decision = skydiag::helper::EvaluateHang(
      static_cast<std::uint64_t>(now.QuadPart),
      proc.shm->header.last_heartbeat_qpc,
      proc.shm->header.qpc_freq,
      stateFlags,
      ((stateFlags & skydiag::kState_InMenu) != 0u)
        ? std::max(cfg.hangThresholdInGameSec, cfg.hangThresholdInMenuSec)
        : cfg.hangThresholdInGameSec,
      loadingThresholdSec);

    if (!decision.isHang) {
      hangCapturedThisEpisode = false;
      hangSuppressedNotForegroundThisEpisode = false;
      hangSuppressedForegroundGraceThisEpisode = false;
      hangSuppressedForegroundResponsiveThisEpisode = false;
      hangSuppressionState = {};
      continue;
    }

    // If heartbeat never advanced after attach, treat hang detection as unreliable and avoid auto-captures.
    // Users can still capture a snapshot with the manual hotkey (Ctrl+Shift+F12).
    if (!heartbeatEverAdvanced) {
      continue;
    }

    if (hangCapturedThisEpisode) {
      continue;
    }

    // Common case: users Alt-Tab away while Skyrim is intentionally paused.
    // In that state, the heartbeat can stop, but it is not actionable to create a hang dump.
    // Default: suppress auto hang dumps while Skyrim is not the foreground process.
    // (Optional) If suppression is disabled, we still try to detect "background pause" by checking
    // whether the game window is responsive.
    const bool isForeground = IsPidInForeground(proc.pid);
    if (!targetWindow || !IsWindow(targetWindow)) {
      targetWindow = FindMainWindowForPid(proc.pid);
    }
    const bool isWindowResponsive = targetWindow && IsWindowResponsive(targetWindow, 250);
    const auto hangSup = skydiag::helper::EvaluateHangSuppression(
      hangSuppressionState,
      decision.isHang,
      isForeground,
      decision.isLoading,
      isWindowResponsive,
      cfg.suppressHangWhenNotForeground,
      static_cast<std::uint64_t>(now.QuadPart),
      proc.shm->header.last_heartbeat_qpc,
      proc.shm->header.qpc_freq,
      cfg.foregroundGraceSec);
    if (hangSup.suppress) {
      if (hangSup.reason == skydiag::helper::HangSuppressionReason::kNotForeground) {
        if (!hangSuppressedNotForegroundThisEpisode) {
          hangSuppressedNotForegroundThisEpisode = true;
          const bool inMenu = (stateFlags & skydiag::kState_InMenu) != 0u;
          AppendLogLine(outBase, L"Hang detected but Skyrim is not foreground; suppressing hang dump. "
            L"(secondsSinceHeartbeat=" + std::to_wstring(decision.secondsSinceHeartbeat) +
            L", threshold=" + std::to_wstring(decision.thresholdSec) +
            L", loading=" + std::to_wstring(decision.isLoading ? 1 : 0) +
            L", inMenu=" + std::to_wstring(inMenu ? 1 : 0) + L")");
        }
      } else if (hangSup.reason == skydiag::helper::HangSuppressionReason::kForegroundGrace) {
        if (!hangSuppressedForegroundGraceThisEpisode) {
          hangSuppressedForegroundGraceThisEpisode = true;
          AppendLogLine(outBase, L"Hang detected after returning to foreground, but heartbeat has not advanced yet; waiting for grace period before capturing hang dump.");
        }
      } else if (hangSup.reason == skydiag::helper::HangSuppressionReason::kForegroundResponsive) {
        if (!hangSuppressedForegroundResponsiveThisEpisode) {
          hangSuppressedForegroundResponsiveThisEpisode = true;
          AppendLogLine(outBase, L"Hang detected after returning to foreground, but the window is responsive; assuming Alt-Tab/pause and skipping hang dump.");
        }
      }
      hangCapturedThisEpisode = false;
      continue;
    }

    if (!isForeground) {
      if (!targetWindow || !IsWindow(targetWindow)) {
        targetWindow = FindMainWindowForPid(proc.pid);
      }
      if (targetWindow && IsWindowResponsive(targetWindow, 250)) {
        if (!hangSuppressedNotForegroundThisEpisode) {
          hangSuppressedNotForegroundThisEpisode = true;
          const bool inMenu = (stateFlags & skydiag::kState_InMenu) != 0u;
          AppendLogLine(outBase, L"Hang detected but target window is responsive and not foreground; assuming Alt-Tab/pause and skipping hang dump. "
            L"(secondsSinceHeartbeat=" + std::to_wstring(decision.secondsSinceHeartbeat) +
            L", threshold=" + std::to_wstring(decision.thresholdSec) +
            L", loading=" + std::to_wstring(decision.isLoading ? 1 : 0) +
            L", inMenu=" + std::to_wstring(inMenu ? 1 : 0) + L")");
        }
        hangCapturedThisEpisode = false;
        continue;
      }
    }

    // Avoid generating hang dumps during normal shutdown or transient stalls:
    // Re-check after a short grace period. If the process exits or heartbeats recover, skip capture.
    if (proc.process) {
      const DWORD w = WaitForSingleObject(proc.process, 1500);
      if (w == WAIT_OBJECT_0) {
        AppendLogLine(outBase, L"Hang detected but target process exited during grace period; skipping hang dump.");
        break;
      }
      if (w == WAIT_FAILED) {
        const DWORD le = GetLastError();
        std::wcerr << L"[SkyrimDiagHelper] Target process wait failed (err=" << le << L").\n";
        AppendLogLine(outBase, L"Target process wait failed: " + std::to_wstring(le));
        break;
      }
    }

    LARGE_INTEGER now2{};
    QueryPerformanceCounter(&now2);
    const auto stateFlags2 = proc.shm->header.state_flags;
    const std::uint32_t inGameThresholdSec2 = ((stateFlags2 & skydiag::kState_InMenu) != 0u)
      ? std::max(cfg.hangThresholdInGameSec, cfg.hangThresholdInMenuSec)
      : cfg.hangThresholdInGameSec;
    const std::uint32_t loadingThresholdSec2 = (cfg.enableAdaptiveLoadingThreshold && loadStats.HasSamples())
      ? adaptiveLoadingThresholdSec
      : cfg.hangThresholdLoadingSec;
    const auto decision2 = skydiag::helper::EvaluateHang(
      static_cast<std::uint64_t>(now2.QuadPart),
      proc.shm->header.last_heartbeat_qpc,
      proc.shm->header.qpc_freq,
      stateFlags2,
      inGameThresholdSec2,
      loadingThresholdSec2);
    if (!decision2.isHang) {
      AppendLogLine(outBase, L"Hang detected but recovered during grace period; skipping hang dump.");
      hangCapturedThisEpisode = false;
      hangSuppressedNotForegroundThisEpisode = false;
      hangSuppressedForegroundGraceThisEpisode = false;
      hangSuppressedForegroundResponsiveThisEpisode = false;
      hangSuppressionState = {};
      continue;
    }

    const bool isForeground2 = IsPidInForeground(proc.pid);
    if (!targetWindow || !IsWindow(targetWindow)) {
      targetWindow = FindMainWindowForPid(proc.pid);
    }
    const bool isWindowResponsive2 = targetWindow && IsWindowResponsive(targetWindow, 250);
    const auto hangSup2 = skydiag::helper::EvaluateHangSuppression(
      hangSuppressionState,
      decision2.isHang,
      isForeground2,
      decision2.isLoading,
      isWindowResponsive2,
      cfg.suppressHangWhenNotForeground,
      static_cast<std::uint64_t>(now2.QuadPart),
      proc.shm->header.last_heartbeat_qpc,
      proc.shm->header.qpc_freq,
      cfg.foregroundGraceSec);
    if (hangSup2.suppress) {
      if (hangSup2.reason == skydiag::helper::HangSuppressionReason::kNotForeground) {
        if (!hangSuppressedNotForegroundThisEpisode) {
          hangSuppressedNotForegroundThisEpisode = true;
          const bool inMenu = (stateFlags2 & skydiag::kState_InMenu) != 0u;
          AppendLogLine(outBase, L"Hang confirmed but Skyrim is not foreground; suppressing hang dump. "
            L"(secondsSinceHeartbeat=" + std::to_wstring(decision2.secondsSinceHeartbeat) +
            L", threshold=" + std::to_wstring(decision2.thresholdSec) +
            L", loading=" + std::to_wstring(decision2.isLoading ? 1 : 0) +
            L", inMenu=" + std::to_wstring(inMenu ? 1 : 0) + L")");
        }
      } else if (hangSup2.reason == skydiag::helper::HangSuppressionReason::kForegroundGrace) {
        if (!hangSuppressedForegroundGraceThisEpisode) {
          hangSuppressedForegroundGraceThisEpisode = true;
          AppendLogLine(outBase, L"Hang confirmed after returning to foreground, but heartbeat has not advanced yet; waiting for grace period before capturing hang dump.");
        }
      } else if (hangSup2.reason == skydiag::helper::HangSuppressionReason::kForegroundResponsive) {
        if (!hangSuppressedForegroundResponsiveThisEpisode) {
          hangSuppressedForegroundResponsiveThisEpisode = true;
          AppendLogLine(outBase, L"Hang confirmed after returning to foreground, but the window is responsive; assuming Alt-Tab/pause and skipping hang dump.");
        }
      }
      hangCapturedThisEpisode = false;
      continue;
    }

    if (!isForeground2) {
      if (!targetWindow || !IsWindow(targetWindow)) {
        targetWindow = FindMainWindowForPid(proc.pid);
      }
      if (targetWindow && IsWindowResponsive(targetWindow, 250)) {
        if (!hangSuppressedNotForegroundThisEpisode) {
          hangSuppressedNotForegroundThisEpisode = true;
          const bool inMenu = (stateFlags2 & skydiag::kState_InMenu) != 0u;
          AppendLogLine(outBase, L"Hang confirmed but target window is responsive and not foreground; assuming Alt-Tab/pause and skipping hang dump. "
            L"(secondsSinceHeartbeat=" + std::to_wstring(decision2.secondsSinceHeartbeat) +
            L", threshold=" + std::to_wstring(decision2.thresholdSec) +
            L", loading=" + std::to_wstring(decision2.isLoading ? 1 : 0) +
            L", inMenu=" + std::to_wstring(inMenu ? 1 : 0) + L")");
        }
        hangCapturedThisEpisode = false;
        continue;
      }
    }

    const auto ts = Timestamp();
    const auto wctPath = outBase / (L"SkyrimDiag_WCT_" + ts + L".json");
    const auto dumpPath = (outBase / (L"SkyrimDiag_Hang_" + ts + L".dmp")).wstring();
    const auto etwPath = outBase / (L"SkyrimDiag_Hang_" + ts + L".etl");

    bool etwStarted = false;
    if (cfg.enableEtwCaptureOnHang) {
      std::wstring etwErr;
      if (StartEtwCaptureForHang(cfg, outBase, &etwErr)) {
        etwStarted = true;
        AppendLogLine(outBase, L"ETW hang capture started.");
      } else {
        AppendLogLine(outBase, L"ETW hang capture start failed: " + etwErr);
      }
    }

    nlohmann::json wctJson;
    std::wstring wctErr;
    if (!skydiag::helper::CaptureWct(proc.pid, wctJson, &wctErr)) {
      std::wcerr << L"[SkyrimDiagHelper] WCT capture failed: " << wctErr << L"\n";
      wctJson = nlohmann::json::object();
      wctJson["pid"] = proc.pid;
      wctJson["error"] = "capture_failed";
    }
    if (wctJson.contains("debugPrivilegeEnabled") && wctJson["debugPrivilegeEnabled"].is_boolean() &&
        !wctJson["debugPrivilegeEnabled"].get<bool>()) {
      AppendLogLine(outBase, L"Warning: EnableDebugPrivilege failed; WCT capture may be incomplete.");
    }

    wctJson["capture"] = nlohmann::json::object();
    wctJson["capture"]["kind"] = "hang";
    wctJson["capture"]["secondsSinceHeartbeat"] = decision2.secondsSinceHeartbeat;
    wctJson["capture"]["thresholdSec"] = decision2.thresholdSec;
    wctJson["capture"]["isLoading"] = decision2.isLoading;
    wctJson["capture"]["stateFlags"] = stateFlags2;

    const std::string wctUtf8 = wctJson.dump(2);
    WriteTextFileUtf8(wctPath, wctUtf8);

    std::wstring dumpErr;
    if (!skydiag::helper::WriteDumpWithStreams(
          proc.process,
          proc.pid,
          dumpPath,
          proc.shm,
          proc.shmSize,
          wctUtf8,
          /*isCrash=*/false,
          cfg.dumpMode,
          &dumpErr)) {
      std::wcerr << L"[SkyrimDiagHelper] Hang dump failed: " << dumpErr << L"\n";
      AppendLogLine(outBase, L"Hang dump failed: " + dumpErr);
    } else {
      std::wcout << L"[SkyrimDiagHelper] Hang dump written: " << dumpPath << L"\n";
      std::wcout << L"[SkyrimDiagHelper] WCT written: " << wctPath.wstring() << L"\n";
      std::wcout << L"[SkyrimDiagHelper] Hang decision: secondsSinceHeartbeat=" << decision2.secondsSinceHeartbeat
                 << L" threshold=" << decision2.thresholdSec << L" loading=" << (decision2.isLoading ? L"1" : L"0") << L"\n";
      AppendLogLine(outBase, L"Hang dump written: " + dumpPath);
      AppendLogLine(outBase, L"WCT written: " + wctPath.wstring());
      AppendLogLine(outBase, L"Hang decision: secondsSinceHeartbeat=" + std::to_wstring(decision2.secondsSinceHeartbeat) +
        L" threshold=" + std::to_wstring(decision2.thresholdSec) + L" loading=" + std::to_wstring(decision2.isLoading ? 1 : 0));
      StartDumpToolHeadlessIfConfigured(cfg, dumpPath, outBase);
      if (cfg.autoOpenViewerOnHang) {
        if (cfg.autoOpenHangAfterProcessExit) {
          pendingHangViewerDumpPath = dumpPath;
          AppendLogLine(outBase, L"Queued hang dump for viewer auto-open on process exit.");
        } else {
          StartDumpToolViewer(cfg, dumpPath, outBase, L"hang");
        }
      }
    }

    if (etwStarted) {
      std::wstring etwErr;
      if (StopEtwCaptureForHang(cfg, outBase, etwPath, &etwErr)) {
        AppendLogLine(outBase, L"ETW hang capture written: " + etwPath.wstring());
      } else {
        AppendLogLine(outBase, L"ETW hang capture stop failed: " + etwErr);
      }
    }

    hangCapturedThisEpisode = true;
  }

  if (cfg.enableManualCaptureHotkey) {
    UnregisterHotKey(nullptr, kHotkeyId);
  }

  skydiag::helper::Detach(proc);
  return 0;
}
