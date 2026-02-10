#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/CrashRecapturePolicy.h"
#include "SkyrimDiagHelper/HeadlessAnalysisPolicy.h"
#include "SkyrimDiagHelper/DumpToolResolve.h"
#include "SkyrimDiagHelper/DumpWriter.h"
#include "SkyrimDiagHelper/HangSuppression.h"
#include "SkyrimDiagHelper/HangDetect.h"
#include "SkyrimDiagHelper/LoadStats.h"
#include "SkyrimDiagHelper/ProcessAttach.h"
#include "SkyrimDiagHelper/Retention.h"
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

bool ReadTextFileUtf8(const std::filesystem::path& path, std::string* out)
{
  if (out) {
    out->clear();
  }
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  if (out) {
    *out = ss.str();
  }
  return true;
}

std::string TrimAscii(std::string_view s)
{
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
    b++;
  }
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
    e--;
  }
  return std::string(s.substr(b, e - b));
}

std::string LowerAscii(std::string_view s)
{
  std::string out;
  out.reserve(s.size());
  for (const unsigned char c : s) {
    out.push_back(static_cast<char>(std::tolower(c)));
  }
  return out;
}

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b)
{
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); i++) {
    if (std::towlower(a[i]) != std::towlower(b[i])) {
      return false;
    }
  }
  return true;
}

bool IsUnknownModuleField(std::string_view modulePlusOffset)
{
  const std::string lower = LowerAscii(TrimAscii(modulePlusOffset));
  return lower.empty() || lower == "unknown" || lower == "<unknown>" || lower == "n/a" || lower == "none";
}

struct CrashSummaryInfo
{
  std::uint32_t schemaVersion = 1;
  std::string bucketKey;
  bool unknownFaultModule = true;
};

constexpr std::uint32_t kMinSupportedSummarySchemaVersion = 1;
constexpr std::uint32_t kMaxSupportedSummarySchemaVersion = 2;

std::filesystem::path SummaryPathForDump(const std::wstring& dumpPath, const std::filesystem::path& outBase)
{
  const std::filesystem::path dumpFs(dumpPath);
  const std::wstring stem = dumpFs.stem().wstring();
  return outBase / (stem + L"_SkyrimDiagSummary.json");
}

bool TryLoadCrashSummaryInfo(const std::filesystem::path& summaryPath, CrashSummaryInfo* out, std::wstring* err)
{
  if (out) {
    *out = CrashSummaryInfo{};
  }
  std::string txt;
  if (!ReadTextFileUtf8(summaryPath, &txt)) {
    if (err) *err = L"summary not found/readable: " + summaryPath.wstring();
    return false;
  }
  const auto root = nlohmann::json::parse(txt, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    if (err) *err = L"summary json parse failed: " + summaryPath.wstring();
    return false;
  }

  CrashSummaryInfo info{};
  if (root.contains("schema") && root["schema"].is_object()) {
    const auto& schema = root["schema"];
    info.schemaVersion = schema.value("version", info.schemaVersion);
  }
  if (info.schemaVersion < kMinSupportedSummarySchemaVersion || info.schemaVersion > kMaxSupportedSummarySchemaVersion) {
    if (err) {
      *err = L"unsupported summary schema version: " + std::to_wstring(info.schemaVersion);
    }
    return false;
  }

  info.bucketKey = TrimAscii(root.value("crash_bucket_key", std::string{}));

  std::optional<bool> unknownFromField;
  if (root.contains("exception") && root["exception"].is_object()) {
    const auto& exceptionObj = root["exception"];
    if (exceptionObj.contains("fault_module_unknown") && exceptionObj["fault_module_unknown"].is_boolean()) {
      unknownFromField = exceptionObj["fault_module_unknown"].get<bool>();
    }
    if (!unknownFromField.has_value()) {
      const std::string modulePlusOffset = exceptionObj.value("module_plus_offset", std::string{});
      unknownFromField = IsUnknownModuleField(modulePlusOffset);
    }
  }
  info.unknownFaultModule = unknownFromField.value_or(true);

  if (out) {
    *out = std::move(info);
  }
  if (err) {
    err->clear();
  }
  return true;
}

std::filesystem::path CrashBucketStatsPath(const std::filesystem::path& outBase)
{
  return outBase / L"SkyrimDiag_CrashBucketStats.json";
}

bool UpdateCrashBucketStats(
  const std::filesystem::path& outBase,
  const CrashSummaryInfo& info,
  std::uint32_t* outUnknownStreak,
  std::wstring* err)
{
  if (outUnknownStreak) {
    *outUnknownStreak = 0;
  }
  if (info.bucketKey.empty()) {
    if (err) {
      *err = L"missing crash bucket key";
    }
    return false;
  }

  const auto path = CrashBucketStatsPath(outBase);

  nlohmann::json root = nlohmann::json::object();
  if (std::filesystem::exists(path)) {
    std::string txt;
    if (ReadTextFileUtf8(path, &txt)) {
      const auto parsed = nlohmann::json::parse(txt, nullptr, false);
      if (!parsed.is_discarded() && parsed.is_object()) {
        root = parsed;
      }
    }
  }
  if (!root.contains("version")) {
    root["version"] = 1;
  }
  if (!root.contains("buckets") || !root["buckets"].is_object()) {
    root["buckets"] = nlohmann::json::object();
  }

  auto& bucket = root["buckets"][info.bucketKey];
  if (!bucket.is_object()) {
    bucket = nlohmann::json::object();
  }

  std::uint32_t seenTotal = bucket.value("seen_total", 0u);
  std::uint32_t unknownTotal = bucket.value("unknown_total", 0u);
  std::uint32_t unknownStreak = bucket.value("unknown_streak", 0u);

  seenTotal++;
  if (info.unknownFaultModule) {
    unknownTotal++;
    unknownStreak++;
  } else {
    unknownStreak = 0;
  }

  bucket["seen_total"] = seenTotal;
  bucket["unknown_total"] = unknownTotal;
  bucket["unknown_streak"] = unknownStreak;
  bucket["last_unknown_fault_module"] = info.unknownFaultModule;
  bucket["updated_at_epoch"] = static_cast<std::int64_t>(std::time(nullptr));

  if (outUnknownStreak) {
    *outUnknownStreak = unknownStreak;
  }

  WriteTextFileUtf8(path, root.dump(2));
  if (err) {
    err->clear();
  }
  return true;
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

std::string DumpModeToString(skydiag::helper::DumpMode mode)
{
  switch (mode) {
    case skydiag::helper::DumpMode::kMini:
      return "mini";
    case skydiag::helper::DumpMode::kDefault:
      return "default";
    case skydiag::helper::DumpMode::kFull:
      return "full";
  }
  return "default";
}

nlohmann::json MakeIncidentConfigSnapshotSafe(const skydiag::helper::HelperConfig& cfg)
{
  // Keep this snapshot privacy-safe: avoid absolute paths (OutputDir, DumpToolExe, EtwWprExe).
  nlohmann::json j = nlohmann::json::object();
  j["dump_mode"] = DumpModeToString(cfg.dumpMode);

  j["hang_threshold_in_game_sec"] = cfg.hangThresholdInGameSec;
  j["hang_threshold_in_menu_sec"] = cfg.hangThresholdInMenuSec;
  j["hang_threshold_loading_sec"] = cfg.hangThresholdLoadingSec;
  j["suppress_hang_when_not_foreground"] = cfg.suppressHangWhenNotForeground;
  j["foreground_grace_sec"] = cfg.foregroundGraceSec;

  j["enable_manual_capture_hotkey"] = cfg.enableManualCaptureHotkey;
  j["auto_analyze_dump"] = cfg.autoAnalyzeDump;
  j["allow_online_symbols"] = cfg.allowOnlineSymbols;

  j["auto_open_viewer_on_crash"] = cfg.autoOpenViewerOnCrash;
  j["auto_open_crash_only_if_process_exited"] = cfg.autoOpenCrashOnlyIfProcessExited;
  j["auto_open_crash_wait_for_exit_ms"] = cfg.autoOpenCrashWaitForExitMs;
  j["enable_auto_recapture_on_unknown_crash"] = cfg.enableAutoRecaptureOnUnknownCrash;
  j["auto_recapture_unknown_bucket_threshold"] = cfg.autoRecaptureUnknownBucketThreshold;
  j["auto_recapture_analysis_timeout_sec"] = cfg.autoRecaptureAnalysisTimeoutSec;

  j["auto_open_viewer_on_hang"] = cfg.autoOpenViewerOnHang;
  j["auto_open_viewer_on_manual_capture"] = cfg.autoOpenViewerOnManualCapture;
  j["auto_open_hang_after_process_exit"] = cfg.autoOpenHangAfterProcessExit;
  j["auto_open_hang_delay_ms"] = cfg.autoOpenHangDelayMs;
  j["auto_open_viewer_beginner_mode"] = cfg.autoOpenViewerBeginnerMode;

  j["enable_adaptive_loading_threshold"] = cfg.enableAdaptiveLoadingThreshold;
  j["adaptive_loading_min_sec"] = cfg.adaptiveLoadingMinSec;
  j["adaptive_loading_min_extra_sec"] = cfg.adaptiveLoadingMinExtraSec;
  j["adaptive_loading_max_sec"] = cfg.adaptiveLoadingMaxSec;

  j["enable_etw_capture_on_hang"] = cfg.enableEtwCaptureOnHang;
  j["etw_hang_profile"] = WideToUtf8(cfg.etwHangProfile);
  j["etw_hang_fallback_profile"] = WideToUtf8(cfg.etwHangFallbackProfile);
  j["etw_max_duration_sec"] = cfg.etwMaxDurationSec;

  j["enable_etw_capture_on_crash"] = cfg.enableEtwCaptureOnCrash;
  j["etw_crash_profile"] = WideToUtf8(cfg.etwCrashProfile);
  j["etw_crash_capture_seconds"] = cfg.etwCrashCaptureSeconds;

  j["enable_incident_manifest"] = cfg.enableIncidentManifest;
  j["incident_manifest_include_config_snapshot"] = cfg.incidentManifestIncludeConfigSnapshot;

  j["retention"] = {
    { "max_crash_dumps", cfg.maxCrashDumps },
    { "max_hang_dumps", cfg.maxHangDumps },
    { "max_manual_dumps", cfg.maxManualDumps },
    { "max_etw_traces", cfg.maxEtwTraces },
  };

  return j;
}

std::string MakeIncidentId(std::string_view captureKind, std::wstring_view ts, DWORD pid)
{
  return std::string(captureKind) + "_" + WideToUtf8(ts) + "_pid" + std::to_string(static_cast<std::uint32_t>(pid));
}

nlohmann::json MakeIncidentManifestV1(
  std::string_view captureKind,
  std::wstring_view ts,
  DWORD pid,
  const std::filesystem::path& dumpPath,
  const std::optional<std::filesystem::path>& wctPath,
  const std::optional<std::filesystem::path>& etwPath,
  std::string_view etwStatus,
  std::uint32_t stateFlags,
  const nlohmann::json& context,
  const skydiag::helper::HelperConfig& cfg,
  bool includeConfigSnapshot)
{
  nlohmann::json j = nlohmann::json::object();
  j["schema"] = {
    { "name", "SkyrimDiagIncident" },
    { "version", 1 },
  };
  j["incident_id"] = MakeIncidentId(captureKind, ts, pid);
  j["capture_kind"] = std::string(captureKind);
  j["timestamp_local"] = WideToUtf8(ts);
  j["pid"] = static_cast<std::uint32_t>(pid);
  j["state_flags"] = stateFlags;
  j["artifacts"] = nlohmann::json::object();
  j["artifacts"]["dump"] = WideToUtf8(dumpPath.filename().wstring());
  j["artifacts"]["wct"] = wctPath ? WideToUtf8(wctPath->filename().wstring()) : "";
  j["artifacts"]["etw"] = etwPath ? WideToUtf8(etwPath->filename().wstring()) : "";
  j["artifacts"]["etw_status"] = std::string(etwStatus);
  j["artifacts"]["helper_log"] = "SkyrimDiagHelper.log";
  j["privacy"] = {
    { "paths", "filenames_only" },
    { "contains_user_paths", false },
    { "contains_config_paths", false },
  };
  if (context.is_object() && !context.empty()) {
    j["context"] = context;
  }
  if (includeConfigSnapshot) {
    j["config_snapshot"] = MakeIncidentConfigSnapshotSafe(cfg);
  }
  return j;
}

bool TryUpdateIncidentManifestEtw(
  const std::filesystem::path& manifestPath,
  const std::filesystem::path& etwPath,
  std::string_view etwStatus,
  std::wstring* err)
{
  std::string txt;
  if (!ReadTextFileUtf8(manifestPath, &txt)) {
    if (err) {
      *err = L"manifest read failed";
    }
    return false;
  }
  auto j = nlohmann::json::parse(txt, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    if (err) {
      *err = L"manifest parse failed";
    }
    return false;
  }
  if (!j.contains("artifacts") || !j["artifacts"].is_object()) {
    j["artifacts"] = nlohmann::json::object();
  }
  j["artifacts"]["etw"] = WideToUtf8(etwPath.filename().wstring());
  j["artifacts"]["etw_status"] = std::string(etwStatus);
  WriteTextFileUtf8(manifestPath, j.dump(2));
  if (err) {
    err->clear();
  }
  return true;
}

std::uint64_t g_maxHelperLogBytes = 0;
std::uint32_t g_maxHelperLogFiles = 0;

void AppendLogLine(const std::filesystem::path& outBase, std::wstring_view line)
{
  std::error_code ec;
  std::filesystem::create_directories(outBase, ec);

  const auto path = outBase / L"SkyrimDiagHelper.log";
  skydiag::helper::RotateLogFileIfNeeded(path, g_maxHelperLogBytes, g_maxHelperLogFiles);
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

bool StartEtwCaptureWithProfile(
  const skydiag::helper::HelperConfig& cfg,
  const std::filesystem::path& outBase,
  std::wstring_view profile,
  std::wstring* err)
{
  const std::wstring wprExe = cfg.etwWprExe.empty() ? L"wpr.exe" : cfg.etwWprExe;
  const std::wstring effectiveProfile = profile.empty() ? L"GeneralProfile" : std::wstring(profile);
  std::wstring cmd = QuoteArg(wprExe) + L" -start " + QuoteArg(effectiveProfile) + L" -filemode";
  return RunHiddenProcessAndWait(std::move(cmd), outBase, EtwTimeoutMs(cfg), err);
}

bool StartEtwCaptureForHang(
  const skydiag::helper::HelperConfig& cfg,
  const std::filesystem::path& outBase,
  std::wstring* outUsedProfile,
  std::wstring* err)
{
  if (outUsedProfile) {
    outUsedProfile->clear();
  }

  const std::wstring primaryProfile = cfg.etwHangProfile.empty() ? L"GeneralProfile" : cfg.etwHangProfile;
  std::wstring primaryErr;
  if (StartEtwCaptureWithProfile(cfg, outBase, primaryProfile, &primaryErr)) {
    if (outUsedProfile) {
      *outUsedProfile = primaryProfile;
    }
    if (err) {
      err->clear();
    }
    return true;
  }

  const std::wstring fallbackProfile = cfg.etwHangFallbackProfile;
  if (!fallbackProfile.empty() && !EqualsIgnoreCase(primaryProfile, fallbackProfile)) {
    std::wstring fallbackErr;
    if (StartEtwCaptureWithProfile(cfg, outBase, fallbackProfile, &fallbackErr)) {
      if (outUsedProfile) {
        *outUsedProfile = fallbackProfile;
      }
      if (err) {
        err->clear();
      }
      return true;
    }
    if (err) {
      *err = L"primary(" + primaryProfile + L") failed: " + primaryErr + L"; fallback(" + fallbackProfile +
        L") failed: " + fallbackErr;
    }
    return false;
  }

  if (err) {
    *err = L"primary(" + primaryProfile + L") failed: " + primaryErr;
  }
  return false;
}

bool StopEtwCaptureToPath(
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
  std::wstring* err)
{
  if (outProcess) {
    *outProcess = nullptr;
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
    if (err) *err = L"CreateProcessW failed: " + std::to_wstring(GetLastError());
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
  std::wstring* err)
{
  return StartDumpToolProcessWithHandle(exe, std::move(cmd), outBase, createFlags, nullptr, err);
}

void AppendOnlineSymbolFlag(std::wstring* cmd, bool allowOnlineSymbols)
{
  if (!cmd) {
    return;
  }
  *cmd += allowOnlineSymbols ? L" --allow-online-symbols" : L" --no-online-symbols";
}

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
  return StartDumpToolProcessWithHandle(exe, std::move(cmd), outBase, CREATE_NO_WINDOW, outProcess, err);
}

struct PendingCrashAnalysis
{
  bool active = false;
  std::wstring dumpPath;
  HANDLE process = nullptr;
  ULONGLONG startedAtTick64 = 0;
  DWORD timeoutMs = 0;
};

bool IsProcessStillAlive(HANDLE process, std::wstring* err);

DWORD CrashAnalysisTimeoutMs(const skydiag::helper::HelperConfig& cfg)
{
  std::uint32_t sec = cfg.autoRecaptureAnalysisTimeoutSec;
  if (sec < 5u) {
    sec = 5u;
  }
  if (sec > 180u) {
    sec = 180u;
  }
  return static_cast<DWORD>(sec * 1000u);
}

void ClearPendingCrashAnalysis(PendingCrashAnalysis* task)
{
  if (!task) {
    return;
  }
  if (task->process) {
    CloseHandle(task->process);
    task->process = nullptr;
  }
  task->active = false;
  task->dumpPath.clear();
  task->startedAtTick64 = 0;
  task->timeoutMs = 0;
}

bool StartPendingCrashAnalysisTask(
  const skydiag::helper::HelperConfig& cfg,
  const std::wstring& dumpPath,
  const std::filesystem::path& outBase,
  PendingCrashAnalysis* task,
  std::wstring* err)
{
  if (!task) {
    if (err) {
      *err = L"missing pending task state";
    }
    return false;
  }
  if (task->active) {
    ClearPendingCrashAnalysis(task);
  }

  HANDLE processHandle = nullptr;
  if (!StartDumpToolHeadlessAsync(cfg, dumpPath, outBase, &processHandle, err)) {
    return false;
  }
  task->active = true;
  task->dumpPath = dumpPath;
  task->process = processHandle;
  task->startedAtTick64 = GetTickCount64();
  task->timeoutMs = CrashAnalysisTimeoutMs(cfg);
  if (err) {
    err->clear();
  }
  return true;
}

void FinalizePendingCrashAnalysisIfReady(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  PendingCrashAnalysis* task)
{
  if (!task || !task->active || !task->process) {
    return;
  }

  if (task->timeoutMs > 0) {
    const ULONGLONG nowTick = GetTickCount64();
    if (nowTick >= task->startedAtTick64 &&
        (nowTick - task->startedAtTick64) > static_cast<ULONGLONG>(task->timeoutMs)) {
      TerminateProcess(task->process, 1);
      AppendLogLine(outBase, L"Crash headless analysis timeout; process terminated.");
      ClearPendingCrashAnalysis(task);
      return;
    }
  }

  const DWORD w = WaitForSingleObject(task->process, 0);
  if (w == WAIT_TIMEOUT) {
    return;
  }
  if (w == WAIT_FAILED) {
    AppendLogLine(outBase, L"Crash headless analysis wait failed: " + std::to_wstring(GetLastError()));
    ClearPendingCrashAnalysis(task);
    return;
  }

  DWORD exitCode = 0;
  if (!GetExitCodeProcess(task->process, &exitCode)) {
    AppendLogLine(outBase, L"Crash headless analysis exit code read failed: " + std::to_wstring(GetLastError()));
    ClearPendingCrashAnalysis(task);
    return;
  }
  if (exitCode != 0) {
    AppendLogLine(outBase, L"Crash headless analysis finished with non-zero exit code: " + std::to_wstring(exitCode));
    ClearPendingCrashAnalysis(task);
    return;
  }

  const auto summaryPath = SummaryPathForDump(task->dumpPath, outBase);
  CrashSummaryInfo summaryInfo{};
  std::wstring summaryErr;
  if (!TryLoadCrashSummaryInfo(summaryPath, &summaryInfo, &summaryErr)) {
    AppendLogLine(outBase, L"Crash summary parse failed for recapture policy: " + summaryErr);
    ClearPendingCrashAnalysis(task);
    return;
  }
  if (summaryInfo.bucketKey.empty()) {
    AppendLogLine(outBase, L"Crash summary has empty crash_bucket_key; recapture policy skipped.");
    ClearPendingCrashAnalysis(task);
    return;
  }

  std::uint32_t unknownStreak = 0;
  std::wstring statsErr;
  if (!UpdateCrashBucketStats(outBase, summaryInfo, &unknownStreak, &statsErr)) {
    AppendLogLine(outBase, L"Crash bucket stats update failed: " + statsErr);
    ClearPendingCrashAnalysis(task);
    return;
  }

  const std::wstring bucketW(summaryInfo.bucketKey.begin(), summaryInfo.bucketKey.end());
  AppendLogLine(
    outBase,
    L"Crash bucket stats updated: bucket=" + bucketW +
      L", schemaVersion=" + std::to_wstring(summaryInfo.schemaVersion) +
      L", unknownFaultModule=" + std::to_wstring(summaryInfo.unknownFaultModule ? 1 : 0) +
      L", unknownStreak=" + std::to_wstring(unknownStreak));

  const auto recaptureDecision = skydiag::helper::DecideCrashFullRecapture(
    cfg.enableAutoRecaptureOnUnknownCrash,
    cfg.autoAnalyzeDump,
    summaryInfo.unknownFaultModule,
    unknownStreak,
    cfg.autoRecaptureUnknownBucketThreshold,
    cfg.dumpMode);
  if (recaptureDecision.shouldRecaptureFullDump) {
    std::wstring aliveErr;
    if (IsProcessStillAlive(proc.process, &aliveErr)) {
      const auto tsFull = Timestamp();
      const auto fullDumpPath = (outBase / (L"SkyrimDiag_Crash_" + tsFull + L"_Full.dmp")).wstring();
      std::wstring fullDumpErr;
      if (!skydiag::helper::WriteDumpWithStreams(
            proc.process,
            proc.pid,
            fullDumpPath,
            proc.shm,
            proc.shmSize,
            /*wctJsonUtf8=*/{},
            /*isCrash=*/true,
            skydiag::helper::DumpMode::kFull,
            &fullDumpErr)) {
        AppendLogLine(outBase, L"Crash full recapture failed: " + fullDumpErr);
      } else {
        AppendLogLine(outBase, L"Crash full recapture written: " + fullDumpPath);
        StartDumpToolHeadlessIfConfigured(cfg, fullDumpPath, outBase);
      }
    } else {
      AppendLogLine(outBase, L"Crash full recapture skipped: " + aliveErr);
    }
  }

  ClearPendingCrashAnalysis(task);
}

bool IsProcessStillAlive(HANDLE process, std::wstring* err)
{
  if (!process) {
    if (err) {
      *err = L"missing process handle";
    }
    return false;
  }
  const DWORD w = WaitForSingleObject(process, 0);
  if (w == WAIT_TIMEOUT) {
    if (err) {
      err->clear();
    }
    return true;
  }
  if (w == WAIT_OBJECT_0) {
    if (err) {
      *err = L"process already exited";
    }
    return false;
  }
  const DWORD le = GetLastError();
  if (err) {
    *err = L"WaitForSingleObject failed: " + std::to_wstring(le);
  }
  return false;
}

void StartDumpToolViewer(
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
  g_maxHelperLogBytes = cfg.maxHelperLogBytes;
  g_maxHelperLogFiles = cfg.maxHelperLogFiles;
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

  {
    skydiag::helper::RetentionLimits limits{};
    limits.maxCrashDumps = cfg.maxCrashDumps;
    limits.maxHangDumps = cfg.maxHangDumps;
    limits.maxManualDumps = cfg.maxManualDumps;
    limits.maxEtwTraces = cfg.maxEtwTraces;
    skydiag::helper::ApplyRetentionToOutputDir(outBase, limits);
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
  PendingCrashAnalysis pendingCrashAnalysis{};

  struct PendingCrashEtwCapture
  {
    bool active = false;
    std::filesystem::path etwPath;
    std::filesystem::path manifestPath;
    ULONGLONG startedAtTick64 = 0;
    std::uint32_t captureSeconds = 0;
    std::wstring profileUsed;
  };

  PendingCrashEtwCapture pendingCrashEtw{};

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

      if (cfg.enableIncidentManifest) {
        const auto manifestPath = outBase / (L"SkyrimDiag_Incident_Manual_" + ts + L".json");
        nlohmann::json ctx = nlohmann::json::object();
        ctx["trigger"] = WideToUtf8(trigger);
        ctx["seconds_since_heartbeat"] = decision.secondsSinceHeartbeat;
        ctx["threshold_sec"] = decision.thresholdSec;
        ctx["is_loading"] = decision.isLoading;
        ctx["in_menu"] = inMenu;

        const auto manifest = MakeIncidentManifestV1(
          "manual",
          ts,
          proc.pid,
          std::filesystem::path(dumpPath),
          std::optional<std::filesystem::path>(wctPath),
          /*etwPath=*/std::nullopt,
          /*etwStatus=*/"disabled",
          stateFlags,
          ctx,
          cfg,
          cfg.incidentManifestIncludeConfigSnapshot);
        WriteTextFileUtf8(manifestPath, manifest.dump(2));
        AppendLogLine(outBase, L"Incident manifest written: " + manifestPath.wstring());
      }

      const bool viewerNow = cfg.autoOpenViewerOnManualCapture;
      if (viewerNow) {
        StartDumpToolViewer(cfg, dumpPath, outBase, L"manual");
      }
      if (ShouldRunHeadlessDumpAnalysis(cfg, viewerNow, /*analysisRequired=*/false)) {
        StartDumpToolHeadlessIfConfigured(cfg, dumpPath, outBase);
      } else if (viewerNow && cfg.autoAnalyzeDump) {
        AppendLogLine(outBase, L"Skipped headless analysis: viewer auto-open is enabled.");
      }
      skydiag::helper::RetentionLimits limits{};
      limits.maxCrashDumps = cfg.maxCrashDumps;
      limits.maxHangDumps = cfg.maxHangDumps;
      limits.maxManualDumps = cfg.maxManualDumps;
      limits.maxEtwTraces = cfg.maxEtwTraces;
      skydiag::helper::ApplyRetentionToOutputDir(outBase, limits);
    }
  };

  auto maybeStopPendingCrashEtw = [&](bool force) {
    if (!pendingCrashEtw.active) {
      return;
    }

    bool procExited = false;
    if (proc.process) {
      const DWORD w = WaitForSingleObject(proc.process, 0);
      procExited = (w == WAIT_OBJECT_0);
    }

    const ULONGLONG nowTick = GetTickCount64();
    bool timeUp = false;
    if (pendingCrashEtw.captureSeconds > 0 && nowTick >= pendingCrashEtw.startedAtTick64) {
      const ULONGLONG elapsedMs = nowTick - pendingCrashEtw.startedAtTick64;
      timeUp = elapsedMs >= (static_cast<ULONGLONG>(pendingCrashEtw.captureSeconds) * 1000ull);
    }

    if (!force && !procExited && !timeUp) {
      return;
    }

    std::wstring etwErr;
    if (StopEtwCaptureToPath(cfg, outBase, pendingCrashEtw.etwPath, &etwErr)) {
      AppendLogLine(outBase, L"ETW crash capture written: " + pendingCrashEtw.etwPath.wstring());
      if (cfg.enableIncidentManifest && !pendingCrashEtw.manifestPath.empty()) {
        std::wstring updErr;
        if (!TryUpdateIncidentManifestEtw(pendingCrashEtw.manifestPath, pendingCrashEtw.etwPath, "written", &updErr)) {
          AppendLogLine(outBase, L"Incident manifest ETW update failed: " + updErr);
        }
      }
    } else {
      AppendLogLine(outBase, L"ETW crash capture stop failed: " + etwErr);
      if (cfg.enableIncidentManifest && !pendingCrashEtw.manifestPath.empty()) {
        std::wstring updErr;
        if (!TryUpdateIncidentManifestEtw(pendingCrashEtw.manifestPath, pendingCrashEtw.etwPath, "stop_failed", &updErr)) {
          AppendLogLine(outBase, L"Incident manifest ETW update failed: " + updErr);
        }
      }
    }

    pendingCrashEtw.active = false;
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

    FinalizePendingCrashAnalysisIfReady(cfg, proc, outBase, &pendingCrashAnalysis);
    maybeStopPendingCrashEtw(/*force=*/false);

    if (proc.process) {
      const DWORD w = WaitForSingleObject(proc.process, 0);
      if (w == WAIT_OBJECT_0) {
        maybeStopPendingCrashEtw(/*force=*/true);
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
        maybeStopPendingCrashEtw(/*force=*/true);
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
        const auto etwPath = outBase / (L"SkyrimDiag_Crash_" + ts + L".etl");
        const auto manifestPath = outBase / (L"SkyrimDiag_Incident_Crash_" + ts + L".json");
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

          const auto stateFlags = proc.shm->header.state_flags;

          bool etwStarted = false;
          std::string etwStatus = cfg.enableEtwCaptureOnCrash ? "start_failed" : "disabled";
          if (cfg.enableEtwCaptureOnCrash && !pendingCrashEtw.active) {
            const std::wstring effectiveProfile = cfg.etwCrashProfile.empty() ? L"GeneralProfile" : cfg.etwCrashProfile;
            std::wstring etwErr;
            if (StartEtwCaptureWithProfile(cfg, outBase, effectiveProfile, &etwErr)) {
              etwStarted = true;
              etwStatus = "recording";

              pendingCrashEtw.active = true;
              pendingCrashEtw.etwPath = etwPath;
              pendingCrashEtw.manifestPath = cfg.enableIncidentManifest ? manifestPath : std::filesystem::path{};
              pendingCrashEtw.startedAtTick64 = GetTickCount64();
              pendingCrashEtw.captureSeconds = cfg.etwCrashCaptureSeconds;
              pendingCrashEtw.profileUsed = effectiveProfile;

              AppendLogLine(
                outBase,
                L"ETW crash capture started (profile=" + effectiveProfile +
                  L", seconds=" + std::to_wstring(pendingCrashEtw.captureSeconds) + L").");
            } else {
              AppendLogLine(outBase, L"ETW crash capture start failed: " + etwErr);
            }
          }

          if (cfg.enableIncidentManifest) {
            nlohmann::json ctx = nlohmann::json::object();
            ctx["reason"] = "crash_event";
            const auto manifest = MakeIncidentManifestV1(
              "crash",
              ts,
              proc.pid,
              std::filesystem::path(dumpPath),
              /*wctPath=*/std::nullopt,
              etwStarted ? std::optional<std::filesystem::path>(etwPath) : std::nullopt,
              etwStatus,
              stateFlags,
              ctx,
              cfg,
              cfg.incidentManifestIncludeConfigSnapshot);
            WriteTextFileUtf8(manifestPath, manifest.dump(2));
            AppendLogLine(outBase, L"Incident manifest written: " + manifestPath.wstring());
          }

          bool crashAnalysisQueued = false;
          if (cfg.autoAnalyzeDump && cfg.enableAutoRecaptureOnUnknownCrash) {
            std::wstring analyzeQueueErr;
            if (StartPendingCrashAnalysisTask(cfg, dumpPath, outBase, &pendingCrashAnalysis, &analyzeQueueErr)) {
              crashAnalysisQueued = true;
              AppendLogLine(outBase, L"Crash headless analysis queued for unknown-bucket recapture policy.");
            } else {
              AppendLogLine(outBase, L"Crash headless analysis queue failed: " + analyzeQueueErr);
            }
          }

          bool viewerNow = false;
          if (cfg.autoOpenViewerOnCrash) {
            if (!cfg.autoOpenCrashOnlyIfProcessExited) {
              StartDumpToolViewer(cfg, dumpPath, outBase, L"crash");
              viewerNow = true;
            } else if (proc.process) {
              const DWORD waitMs = static_cast<DWORD>(std::min<std::uint32_t>(cfg.autoOpenCrashWaitForExitMs, 10000u));
              const DWORD wExit = WaitForSingleObject(proc.process, waitMs);
              if (wExit == WAIT_OBJECT_0) {
                StartDumpToolViewer(cfg, dumpPath, outBase, L"crash_exit");
                viewerNow = true;
                AppendLogLine(outBase, L"Auto-opened DumpTool viewer for crash after process exit.");
              } else if (wExit == WAIT_TIMEOUT) {
                AppendLogLine(outBase, L"Crash dump captured but process is still running; skipping viewer auto-open.");
              } else {
                const DWORD le = GetLastError();
                AppendLogLine(outBase, L"Crash viewer auto-open suppressed due to wait failure: " + std::to_wstring(le));
              }
            } else {
              AppendLogLine(outBase, L"Crash viewer auto-open suppressed: missing process handle.");
            }
          }

          if (!crashAnalysisQueued) {
            if (ShouldRunHeadlessDumpAnalysis(cfg, viewerNow, /*analysisRequired=*/false)) {
              StartDumpToolHeadlessIfConfigured(cfg, dumpPath, outBase);
            } else if (viewerNow && cfg.autoAnalyzeDump) {
              AppendLogLine(outBase, L"Skipped headless analysis: viewer auto-open is enabled.");
            }
          }
          skydiag::helper::RetentionLimits limits{};
          limits.maxCrashDumps = cfg.maxCrashDumps;
          limits.maxHangDumps = cfg.maxHangDumps;
          limits.maxManualDumps = cfg.maxManualDumps;
          limits.maxEtwTraces = cfg.maxEtwTraces;
          skydiag::helper::ApplyRetentionToOutputDir(outBase, limits);
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
    const auto manifestPath = outBase / (L"SkyrimDiag_Incident_Hang_" + ts + L".json");
    bool manifestWritten = false;

    bool etwStarted = false;
    if (cfg.enableEtwCaptureOnHang) {
      std::wstring etwUsedProfile;
      std::wstring etwErr;
      if (StartEtwCaptureForHang(cfg, outBase, &etwUsedProfile, &etwErr)) {
        etwStarted = true;
        AppendLogLine(outBase, L"ETW hang capture started (profile=" + etwUsedProfile + L").");
      } else {
        AppendLogLine(outBase, L"ETW hang capture start failed: " + etwErr);
      }
    }
    std::string etwStatus = cfg.enableEtwCaptureOnHang ? (etwStarted ? "recording" : "start_failed") : "disabled";

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
      if (cfg.enableIncidentManifest) {
        const bool inMenu2 = (stateFlags2 & skydiag::kState_InMenu) != 0u;
        nlohmann::json ctx = nlohmann::json::object();
        ctx["seconds_since_heartbeat"] = decision2.secondsSinceHeartbeat;
        ctx["threshold_sec"] = decision2.thresholdSec;
        ctx["is_loading"] = decision2.isLoading;
        ctx["in_menu"] = inMenu2;

        const auto manifest = MakeIncidentManifestV1(
          "hang",
          ts,
          proc.pid,
          std::filesystem::path(dumpPath),
          std::optional<std::filesystem::path>(wctPath),
          etwStarted ? std::optional<std::filesystem::path>(etwPath) : std::nullopt,
          etwStatus,
          stateFlags2,
          ctx,
          cfg,
          cfg.incidentManifestIncludeConfigSnapshot);
        WriteTextFileUtf8(manifestPath, manifest.dump(2));
        AppendLogLine(outBase, L"Incident manifest written: " + manifestPath.wstring());
        manifestWritten = true;
      }

      const bool viewerNow = cfg.autoOpenViewerOnHang && !cfg.autoOpenHangAfterProcessExit;
      if (cfg.autoOpenViewerOnHang) {
        if (cfg.autoOpenHangAfterProcessExit) {
          pendingHangViewerDumpPath = dumpPath;
          AppendLogLine(outBase, L"Queued hang dump for viewer auto-open on process exit.");
        } else {
          StartDumpToolViewer(cfg, dumpPath, outBase, L"hang");
        }
      }
      if (ShouldRunHeadlessDumpAnalysis(cfg, viewerNow, /*analysisRequired=*/false)) {
        StartDumpToolHeadlessIfConfigured(cfg, dumpPath, outBase);
      } else if (viewerNow && cfg.autoAnalyzeDump) {
        AppendLogLine(outBase, L"Skipped headless analysis: viewer auto-open is enabled.");
      }
      skydiag::helper::RetentionLimits limits{};
      limits.maxCrashDumps = cfg.maxCrashDumps;
      limits.maxHangDumps = cfg.maxHangDumps;
      limits.maxManualDumps = cfg.maxManualDumps;
      limits.maxEtwTraces = cfg.maxEtwTraces;
      skydiag::helper::ApplyRetentionToOutputDir(outBase, limits);
    }

    if (etwStarted) {
      std::wstring etwErr;
      if (StopEtwCaptureToPath(cfg, outBase, etwPath, &etwErr)) {
        AppendLogLine(outBase, L"ETW hang capture written: " + etwPath.wstring());
        etwStatus = "written";
        if (manifestWritten) {
          std::wstring updErr;
          if (!TryUpdateIncidentManifestEtw(manifestPath, etwPath, "written", &updErr)) {
            AppendLogLine(outBase, L"Incident manifest ETW update failed: " + updErr);
          }
        }
      } else {
        AppendLogLine(outBase, L"ETW hang capture stop failed: " + etwErr);
        etwStatus = "stop_failed";
        if (manifestWritten) {
          std::wstring updErr;
          if (!TryUpdateIncidentManifestEtw(manifestPath, etwPath, "stop_failed", &updErr)) {
            AppendLogLine(outBase, L"Incident manifest ETW update failed: " + updErr);
          }
        }
      }
    }

    hangCapturedThisEpisode = true;
  }

  maybeStopPendingCrashEtw(/*force=*/true);

  if (cfg.enableManualCaptureHotkey) {
    UnregisterHotKey(nullptr, kHotkeyId);
  }

  if (pendingCrashAnalysis.active) {
    AppendLogLine(outBase, L"Helper shutting down while crash analysis is still running; detaching from pending recapture task.");
    ClearPendingCrashAnalysis(&pendingCrashAnalysis);
  }

  skydiag::helper::Detach(proc);
  return 0;
}
