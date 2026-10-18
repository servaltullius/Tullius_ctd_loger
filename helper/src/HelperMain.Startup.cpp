#include "HelperMainInternal.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "SkyrimDiagHelper/ManualCaptureHotkey.h"
#include "SkyrimDiagHelper/PluginScanner.h"
#include "SkyrimDiagProtocol.h"

#include "CrashCapture.h"
#include "HelperLog.h"
#include "ManualCaptureKeyWatcher.h"

namespace {

using skydiag::protocol::MakeKernelName;

}  // namespace

namespace skydiag::helper::internal {

HANDLE AcquireHelperSingletonMutex(std::uint32_t pid, std::wstring* err)
{
  const auto mutexName = MakeKernelName(pid, skydiag::protocol::kKernelObjectSuffix_HelperMutex);
  HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
  if (!mutex) {
    if (err) {
      *err = L"CreateMutexW failed: " + std::to_wstring(GetLastError());
    }
    return nullptr;
  }

  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (err) {
      *err = L"Helper singleton mutex already exists.";
    }
    CloseHandle(mutex);
    return INVALID_HANDLE_VALUE;
  }

  if (err) {
    err->clear();
  }
  return mutex;
}

std::wstring BuildCrashEventUnavailableMessage(const AttachedProcess& proc, std::wstring_view prefix)
{
  std::wstring message(prefix);
  if (proc.crashEventOpenError != ERROR_SUCCESS) {
    message += L" (err=" + std::to_wstring(proc.crashEventOpenError) + L")";
  }
  return message;
}

CrashArtifactRemovalResult RemoveCrashArtifactsForDump(
  const std::filesystem::path& outBase,
  std::wstring_view dumpPath,
  const std::filesystem::path& extraArtifactPath,
  bool preserveDumpFile)
{
  CrashArtifactRemovalResult result{};
  if (dumpPath.empty()) {
    return result;
  }

  const std::filesystem::path dumpFs(dumpPath);
  const std::wstring stem = dumpFs.stem().wstring();
  if (stem.empty()) {
    return result;
  }

  std::vector<std::filesystem::path> artifacts;
  artifacts.reserve(8);
  if (!preserveDumpFile) {
    artifacts.push_back(dumpFs);
  }
  artifacts.push_back(outBase / (stem + L"_SkyrimDiagBlackbox.jsonl"));
  artifacts.push_back(outBase / (stem + L"_SkyrimDiagReport.txt"));
  artifacts.push_back(outBase / (stem + L"_SkyrimDiagSummary.json"));
  artifacts.push_back(outBase / (stem + L"_SkyrimDiagWct.json"));
  artifacts.push_back(dumpFs.parent_path() / (stem + L"_PluginScan.json"));
  artifacts.push_back(outBase / (stem + L".etl"));

  const std::wstring kCrashStemPrefix = L"SkyrimDiag_Crash_";
  if (stem.rfind(kCrashStemPrefix, 0) == 0) {
    std::wstring ts = stem.substr(kCrashStemPrefix.size());
    const std::wstring kFullSuffix = L"_Full";
    if (ts.size() > kFullSuffix.size() && ts.compare(ts.size() - kFullSuffix.size(), kFullSuffix.size(), kFullSuffix) == 0) {
      ts.resize(ts.size() - kFullSuffix.size());
    }
    if (!ts.empty()) {
      artifacts.push_back(outBase / (L"SkyrimDiag_Incident_Crash_" + ts + L".json"));
    }
  }
  if (!extraArtifactPath.empty()) {
    artifacts.push_back(extraArtifactPath);
  }

  for (const auto& path : artifacts) {
    if (path.empty()) {
      continue;
    }
    std::error_code ec;
    const bool isDump = path == dumpFs;
    if (isDump) {
      result.dumpExistedBefore = std::filesystem::exists(path, ec);
      if (ec) {
        result.dumpError = ec;
        AppendLogLine(
          outBase,
          L"Failed to inspect crash dump before removal: " + path.wstring()
            + L" (err=" + std::to_wstring(ec.value()) + L")");
        ec.clear();
      }
    }
    const bool removed = std::filesystem::remove(path, ec);
    if (ec) {
      if (isDump) {
        result.dumpError = ec;
      }
      AppendLogLine(
        outBase,
        L"Failed to remove crash artifact: " + path.wstring()
          + L" (err=" + std::to_wstring(ec.value()) + L")");
      continue;
    }
    if (!ec && removed) {
      ++result.removedCount;
    }
  }

  std::error_code existsEc;
  result.dumpExistsAfter = std::filesystem::exists(dumpFs, existsEc);
  if (existsEc) {
    result.dumpError = existsEc;
    // Fail closed: an unobservable final state must not be reported as deleted.
    result.dumpExistsAfter = true;
    AppendLogLine(
      outBase,
      L"Failed to inspect crash dump after artifact removal: " + dumpFs.wstring()
        + L" (err=" + std::to_wstring(existsEc.value()) + L")");
  }
  return result;
}

void InitializeLoopState(const AttachedProcess& proc, HelperLoopState* state)
{
  if (!state) {
    return;
  }
  state->hangState.wasLoading = (proc.shm->header.state_flags & skydiag::kState_Loading) != 0u;
  // Loading at attach is the game starting up (the plugin sets the flag until
  // its data has loaded), not a load screen: timing it from start_qpc put a
  // 2-minute sample next to every 25 s save load and roughly doubled the
  // adaptive loading threshold. Only load screens seen start to end count.
  state->hangState.loadStartQpc = 0;
  state->nextCrashEventRetryTick64 = GetTickCount64();
  state->nextCrashEventWarnTick64 = 0;
}

void RegisterManualCaptureHotkeyIfEnabled(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase)
{
  // Crash Logger's thread dump runs on its own hotkey whatever the manual
  // capture hotkey is, so its keys are watched either way
  // (IsLikelyCrashLoggerThreadDumpFault).
  std::optional<CrashLoggerThreadDumpHotkey> crashLoggerThreadDump;
  std::filesystem::path gameExeDir;
  if (skydiag::helper::TryResolveGameExeDir(proc.process, gameExeDir)) {
    const auto check = CheckCrashLoggerThreadDumpHotkey(
      gameExeDir / L"Data" / L"SKSE" / L"Plugins", cfg.manualCaptureHotkey);
    if (check.crashLoggerFound && check.threadDump.enabled) {
      crashLoggerThreadDump = check.threadDump;
      AppendLogLine(outBase, L"Watching Crash Logger's thread dump hotkey (" +
        FormatVirtualKeyList(check.threadDump.keys) +
        L") so its thread dump's probes do not get crash dumps while the game keeps running.");
    }
  }

  // Ctrl+Shift+F12, the manual capture hotkey up to v0.2.68-rc3, is watched
  // while the hotkey is set to another chord, so a press of it from habit is
  // logged as taking no snapshot, with or without Crash Logger.
  const std::optional<ManualCaptureHotkey> legacyManualCapture =
    LegacyManualCaptureChordToWatch(cfg.enableManualCaptureHotkey, cfg.manualCaptureHotkey);

  if (!cfg.enableManualCaptureHotkey) {
    ManualCaptureKeys().Start(std::nullopt, crashLoggerThreadDump, legacyManualCapture);
    return;
  }

  static_assert(kHotkeyModAlt == MOD_ALT && kHotkeyModControl == MOD_CONTROL && kHotkeyModShift == MOD_SHIFT);
  static_assert(kVkShift == VK_SHIFT && kVkControl == VK_CONTROL && kVkAlt == VK_MENU && kVkBack == VK_BACK);

  // Key-state polling also catches presses WM_HOTKEY never delivers (see
  // ManualCaptureKeyWatcher), so a failed registration only loses one path.
  const ManualCaptureHotkey& hotkey = cfg.manualCaptureHotkey;
  const std::wstring chord = FormatManualCaptureHotkey(hotkey);
  ManualCaptureKeys().Start(hotkey, crashLoggerThreadDump, legacyManualCapture);
  if (!RegisterHotKey(nullptr, kHotkeyId, hotkey.modifiers, hotkey.virtualKey)) {
    const DWORD le = GetLastError();
    std::wcerr << L"[SkyrimDiagHelper] Warning: RegisterHotKey(" << chord << L") failed: " << le << L"\n";
    AppendLogLine(outBase, L"Warning: RegisterHotKey(" + chord + L") failed: " + std::to_wstring(le) +
      L" (key-state polling still watches " + chord + L")");
  } else {
    std::wcout << L"[SkyrimDiagHelper] Manual capture hotkey: " << chord << L"\n";
    AppendLogLine(outBase, L"Manual capture hotkey registered: " + chord + L" (also watched by key-state polling)");
  }
}

void UnregisterManualCaptureHotkeyIfEnabled(const HelperConfig& cfg)
{
  if (cfg.enableManualCaptureHotkey) {
    UnregisterHotKey(nullptr, kHotkeyId);
  }
  ManualCaptureKeys().Stop();
}

bool DetectGrassCacheMode(
  const HelperConfig& cfg,
  const AttachedProcess& proc,
  const std::filesystem::path& outBase)
{
  if (!cfg.suppressDuringGrassCaching) {
    return false;
  }

  std::filesystem::path gameExeDir;
  if (!skydiag::helper::TryResolveGameExeDir(proc.process, gameExeDir)) {
    AppendLogLine(outBase, L"Grass cache detection skipped: failed to resolve game exe directory.");
    return false;
  }

  const auto marker = gameExeDir / L"PrecacheGrass.txt";
  std::error_code ec;
  if (!std::filesystem::exists(marker, ec)) {
    return false;
  }

  AppendLogLine(outBase, L"Grass cache mode detected (PrecacheGrass.txt found in " + gameExeDir.wstring() + L").");
  std::wcout << L"[SkyrimDiagHelper] Grass cache mode detected; crash/hang handling suppressed.\n";
  return true;
}

void RunGrassCacheLoop(const AttachedProcess& proc, const std::filesystem::path& outBase)
{
  for (;;) {
    const DWORD w = WaitForSingleObject(proc.process, 1000);
    if (w == WAIT_OBJECT_0) {
      DWORD exitCode = STILL_ACTIVE;
      GetExitCodeProcess(proc.process, &exitCode);
      AppendLogLine(
        outBase,
        L"Grass cache mode: target process exited (exit_code=" + std::to_wstring(exitCode) + L").");
      std::wcout << L"[SkyrimDiagHelper] Grass cache mode: target exited (exit_code=" << exitCode << L").\n";
      return;
    }
    if (w == WAIT_FAILED) {
      const DWORD le = GetLastError();
      AppendLogLine(outBase, L"Grass cache mode: process wait failed (err=" + std::to_wstring(le) + L").");
      return;
    }
  }
}

}  // namespace skydiag::helper::internal
