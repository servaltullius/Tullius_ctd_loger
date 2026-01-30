#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/DumpWriter.h"
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

void StartDumpToolIfConfigured(const skydiag::helper::HelperConfig& cfg, const std::wstring& dumpPath, const std::filesystem::path& outBase)
{
  if (!cfg.autoAnalyzeDump) {
    return;
  }

  std::filesystem::path exe(cfg.dumpToolExe);
  if (exe.is_relative()) {
    exe = GetThisExeDir() / exe;
  }

  std::wstring cmd = L"\"";
  cmd += exe.wstring();
  cmd += L"\" \"";
  cmd += dumpPath;
  cmd += L"\" --out-dir \"";
  cmd += outBase.wstring();
  cmd += L"\" --headless";

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};

  const BOOL ok = CreateProcessW(
    exe.c_str(),
    cmd.data(),
    nullptr,
    nullptr,
    FALSE,
    CREATE_NO_WINDOW,
    nullptr,
    outBase.wstring().c_str(),
    &si,
    &pi);

  if (!ok) {
    std::wcerr << L"[SkyrimDiagHelper] Warning: failed to start DumpTool (err=" << GetLastError() << L")\n";
    return;
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
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

  bool wasLoading = (proc.shm->header.state_flags & skydiag::kState_Loading) != 0u;
  std::uint64_t loadStartQpc = wasLoading ? proc.shm->header.start_qpc : 0;

  auto doManualCapture = [&](std::wstring_view trigger) {
    const auto ts = Timestamp();
    const auto wctPath = outBase / (L"SkyrimDiag_WCT_Manual_" + ts + L".json");
    const auto dumpPath = (outBase / (L"SkyrimDiag_Manual_" + ts + L".dmp")).wstring();

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const auto stateFlags = proc.shm->header.state_flags;
    const std::uint32_t loadingThresholdSec = (cfg.enableAdaptiveLoadingThreshold && loadStats.HasSamples())
      ? adaptiveLoadingThresholdSec
      : cfg.hangThresholdLoadingSec;
    const auto decision = skydiag::helper::EvaluateHang(
      static_cast<std::uint64_t>(now.QuadPart),
      proc.shm->header.last_heartbeat_qpc,
      proc.shm->header.qpc_freq,
      stateFlags,
      cfg.hangThresholdInGameSec,
      loadingThresholdSec);

    AppendLogLine(outBase, L"Manual capture triggered via " + std::wstring(trigger) +
      L" (secondsSinceHeartbeat=" + std::to_wstring(decision.secondsSinceHeartbeat) +
      L", threshold=" + std::to_wstring(decision.thresholdSec) +
      L", loading=" + std::to_wstring(decision.isLoading ? 1 : 0) + L")");

    nlohmann::json wctJson;
    std::wstring wctErr;
    if (!skydiag::helper::CaptureWct(proc.pid, wctJson, &wctErr)) {
      std::wcerr << L"[SkyrimDiagHelper] WCT capture failed: " << wctErr << L"\n";
      AppendLogLine(outBase, L"WCT capture failed: " + wctErr);
      wctJson = nlohmann::json::object();
      wctJson["pid"] = proc.pid;
      wctJson["error"] = "capture_failed";
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
      StartDumpToolIfConfigured(cfg, dumpPath, outBase);
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
          StartDumpToolIfConfigured(cfg, dumpPath, outBase);
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
      cfg.hangThresholdInGameSec,
      loadingThresholdSec);

    if (!decision.isHang) {
      hangCapturedThisEpisode = false;
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

    const auto ts = Timestamp();
    const auto wctPath = outBase / (L"SkyrimDiag_WCT_" + ts + L".json");
    const auto dumpPath = (outBase / (L"SkyrimDiag_Hang_" + ts + L".dmp")).wstring();

    nlohmann::json wctJson;
    std::wstring wctErr;
    if (!skydiag::helper::CaptureWct(proc.pid, wctJson, &wctErr)) {
      std::wcerr << L"[SkyrimDiagHelper] WCT capture failed: " << wctErr << L"\n";
      wctJson = nlohmann::json::object();
      wctJson["pid"] = proc.pid;
      wctJson["error"] = "capture_failed";
    }

    wctJson["capture"] = nlohmann::json::object();
    wctJson["capture"]["kind"] = "hang";
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
      std::wcerr << L"[SkyrimDiagHelper] Hang dump failed: " << dumpErr << L"\n";
      AppendLogLine(outBase, L"Hang dump failed: " + dumpErr);
    } else {
      std::wcout << L"[SkyrimDiagHelper] Hang dump written: " << dumpPath << L"\n";
      std::wcout << L"[SkyrimDiagHelper] WCT written: " << wctPath.wstring() << L"\n";
      std::wcout << L"[SkyrimDiagHelper] Hang decision: secondsSinceHeartbeat=" << decision.secondsSinceHeartbeat
                 << L" threshold=" << decision.thresholdSec << L" loading=" << (decision.isLoading ? L"1" : L"0") << L"\n";
      AppendLogLine(outBase, L"Hang dump written: " + dumpPath);
      AppendLogLine(outBase, L"WCT written: " + wctPath.wstring());
      AppendLogLine(outBase, L"Hang decision: secondsSinceHeartbeat=" + std::to_wstring(decision.secondsSinceHeartbeat) +
        L" threshold=" + std::to_wstring(decision.thresholdSec) + L" loading=" + std::to_wstring(decision.isLoading ? 1 : 0));
      StartDumpToolIfConfigured(cfg, dumpPath, outBase);
    }

    hangCapturedThisEpisode = true;
  }

  if (cfg.enableManualCaptureHotkey) {
    UnregisterHotKey(nullptr, kHotkeyId);
  }

  skydiag::helper::Detach(proc);
  return 0;
}
