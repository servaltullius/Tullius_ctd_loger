#include "HelperMainInternal.h"

#include <ShlObj.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "SkyrimDiagHelper/ManualCaptureHotkey.h"
#include "SkyrimDiagHelper/PluginScanner.h"
#include "SkyrimDiagProtocol.h"
#include "SkyrimDiagStringUtil.h"

#include "CrashCapture.h"
#include "HelperCommon.h"
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

void InitializeLoopState(const AttachedProcess& proc, const std::filesystem::path& outBase, HelperLoopState* state)
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

  // The plugin claims kState_Frozen before every publish, and only a helper
  // clears it. So a committed generation with Frozen clear was already ACKed
  // by an earlier helper that the plugin's watchdog replaced: released as
  // handled, or (indistinguishable here) its dump could not be written. It
  // is not reported again at exit. A generation still Frozen stays reportable,
  // and is handled again while the game runs (RearmFrozenCrashFromEarlierHelper).
  CrashEventInfo committed{};
  if (TryCaptureCommittedCrashInfo(proc.shm ? &proc.shm->header : nullptr, &committed) &&
      (committed.stateFlags & skydiag::kState_Frozen) == 0u) {
    state->crashCaptured.releasedCrashSeq = committed.crashSeq;
    state->crashCaptured.releasedCrashTick64 = GetTickCount64();
    state->crashCaptured.releasedOnBriefHeartbeat = false;
    state->crashCaptured.releasedInThreadDumpWindow = false;
  }
  state->rearmedFrozenCrashSeq = RearmFrozenCrashFromEarlierHelper(proc, outBase);
}

std::wstring FrozenCrashRearmMarkerPrefix(const AttachedProcess& proc)
{
  // The game's creation time keeps a later process that reuses the pid from
  // matching an old marker.
  FILETIME created{};
  FILETIME unusedExit{};
  FILETIME unusedKernel{};
  FILETIME unusedUser{};
  std::uint64_t createdUtc = 0;
  if (proc.process && GetProcessTimes(proc.process, &created, &unusedExit, &unusedKernel, &unusedUser)) {
    createdUtc = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
  }
  return std::wstring(kFrozenCrashRearmMarkerPrefix) + std::to_wstring(proc.pid) + L"_" + std::to_wstring(createdUtc) +
    L"_";
}

void RemoveFrozenCrashRearmMarkers(const AttachedProcess& proc, const std::filesystem::path& outBase)
{
  if (outBase.empty()) {
    return;
  }
  const std::wstring prefix = FrozenCrashRearmMarkerPrefix(proc);
  std::error_code iterEc;
  for (std::filesystem::directory_iterator it(outBase, iterEc), end; !iterEc && it != end; it.increment(iterEc)) {
    const std::wstring name = it->path().filename().wstring();
    if (name.rfind(prefix, 0) == 0) {
      std::error_code ec;
      std::filesystem::remove(it->path(), ec);
    }
  }
}

std::uint32_t RearmFrozenCrashFromEarlierHelper(const AttachedProcess& proc, const std::filesystem::path& outBase)
{
  if (!proc.crashEvent || !proc.process || WaitForSingleObject(proc.process, 0) != WAIT_TIMEOUT) {
    return 0u;  // an exited game's record is drained by the exit path
  }
  CrashEventInfo committed{};
  if (!TryCaptureCommittedCrashInfo(proc.shm ? &proc.shm->header : nullptr, &committed) ||
      (committed.stateFlags & skydiag::kState_Frozen) == 0u) {
    return 0u;
  }

  // A helper that dies on this crash every time (an antivirus killing it
  // during MiniDumpWriteDump, a helper bug) would be restarted by the
  // plugin's watchdog and signaled again forever, the game stuttering at each
  // dump attempt. So a generation is signaled again only once, remembered in
  // a marker file across helper restarts. The next time it is thawed instead,
  // so the plugin records a later crash again. Without a marker there is no
  // bound, so a marker that cannot be written also thaws: the dump would go
  // to the same folder.
  const auto marker = outBase / (FrozenCrashRearmMarkerPrefix(proc) + std::to_wstring(committed.crashSeq) + L".marker");
  std::error_code existsEc;
  const bool alreadyRearmed = std::filesystem::exists(marker, existsEc) || existsEc;
  if (alreadyRearmed || !WriteTextFileUtf8(marker, "rearmed\n")) {
    const bool thawed = proc.shmWritable && TryClearRecoveredCrashFreeze(proc.shmWritable, committed.crashSeq);
    AppendLogLine(
      outBase,
      L"Committed crash_seq=" + std::to_wstring(committed.crashSeq)
        + (alreadyRearmed
             ? L" is still frozen after its crash event was already signaled again once for an earlier helper, "
               L"which did not finish it either; not signaling it again"
             : L" is still frozen with no helper handling it, but its rearm marker could not be written; not "
               L"signaling it again")
        + (thawed ? L". Thawed capture so a later CTD can still be recorded."
                  : L" (capture not thawed: a newer crash record is present)."));
    return 0u;
  }
  return SetEvent(proc.crashEvent) ? committed.crashSeq : 0u;
}

std::filesystem::path DecodeCrashLoggerIniPath(const std::string& bytes)
{
  if (bytes.empty() || bytes.size() > static_cast<std::size_t>(INT_MAX)) {
    return {};
  }
  const int size = static_cast<int>(bytes.size());
  // Crash Logger reads the file as UTF-8; an editor may have saved it in the
  // ANSI code page instead (e.g. CP949). MB_ERR_INVALID_CHARS makes a code
  // page that does not fit fail rather than guess.
  for (const UINT codePage : { static_cast<UINT>(CP_UTF8), static_cast<UINT>(CP_ACP) }) {
    const int needed = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, bytes.data(), size, nullptr, 0);
    if (needed <= 0) {
      continue;
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    if (MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, bytes.data(), size, wide.data(), needed) == needed) {
      return std::filesystem::path(std::move(wide));
    }
  }
  return {};
}

std::vector<std::filesystem::path> ResolveCrashLoggerLogDirs(const HelperConfig& cfg, const AttachedProcess& proc)
{
  std::vector<std::filesystem::path> dirs;
  std::filesystem::path gameExeDir;
  if (!skydiag::helper::TryResolveGameExeDir(proc.process, gameExeDir)) {
    return dirs;
  }
  // Only Crash Logger's thread dump hotkey opens the window this is for.
  const auto crashLogger = CheckCrashLoggerThreadDumpHotkey(
    gameExeDir / L"Data" / L"SKSE" / L"Plugins", cfg.manualCaptureHotkey);
  if (!crashLogger.crashLoggerFound || !crashLogger.threadDump.enabled) {
    return dirs;
  }
  // A Crashlog Directory set in CrashLogger.ini, relative to the game's
  // folder (its working directory) unless absolute; it may not exist yet.
  // A value that decodes neither as UTF-8 nor in the ANSI code page is
  // skipped (a path built from invalid UTF-8 throws).
  const std::filesystem::path custom = DecodeCrashLoggerIniPath(crashLogger.crashlogDirectory);
  if (!custom.empty()) {
    dirs.push_back(custom.is_relative() ? gameExeDir / custom : custom);
  }
  // And the SKSE log folder it uses by default, Documents\My Games\<game
  // folder>\SKSE; the game folder name differs by store ("Skyrim Special
  // Edition", "... GOG").
  wchar_t* documents = nullptr;
  std::filesystem::path myGames;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)) && documents) {
    myGames = std::filesystem::path(documents) / L"My Games";
  }
  CoTaskMemFree(documents);  // required even when the call fails
  if (!myGames.empty()) {
    std::error_code iterEc;
    for (std::filesystem::directory_iterator it(myGames, iterEc), end; !iterEc && it != end; it.increment(iterEc)) {
      std::error_code ec;
      if (it->is_directory(ec) &&
          skydiag::WideLower(it->path().filename().wstring()).rfind(L"skyrim special edition", 0) == 0) {
        dirs.push_back(it->path() / L"SKSE");
      }
    }
  }
  return dirs;
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
  if (!RegisterHotKey(nullptr, kHotkeyId, ManualCaptureHotkeyRegistrationModifiers(hotkey), hotkey.virtualKey)) {
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
