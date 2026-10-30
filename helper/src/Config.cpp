#include "SkyrimDiagHelper/Config.h"

#include <Windows.h>

#include <algorithm>
#include <climits>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

#include "SkyrimDiagHelper/IniText.h"

namespace skydiag::helper {
namespace {

constexpr wchar_t kDefaultOutputSubdir[] = L"Tullius Ctd Logs";

std::wstring ExeDir()
{
  std::vector<wchar_t> buf(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
  if (n == 0 || n >= buf.size()) {
    return {};
  }
  std::filesystem::path p(buf.data(), buf.data() + n);
  return p.parent_path().wstring();
}

std::wstring ReadIniString(const IniDocument& ini, const wchar_t* section, const wchar_t* key, const wchar_t* def)
{
  return ReadIniProfileString(ini, section, key, def ? def : L"");
}

std::wstring AnsiToWide(std::string_view view)
{
  const std::string bytes(view);
  if (bytes.empty() || bytes.size() > static_cast<std::size_t>(INT_MAX)) {
    return {};
  }
  const int len = static_cast<int>(bytes.size());
  const int wideLen = MultiByteToWideChar(CP_ACP, 0, bytes.data(), len, nullptr, 0);
  if (wideLen <= 0) {
    return {};
  }
  std::wstring wide(static_cast<std::size_t>(wideLen), L'\0');
  MultiByteToWideChar(CP_ACP, 0, bytes.data(), len, wide.data(), wideLen);
  return wide;
}

// The ini as written: UTF-8 (with or without BOM), UTF-16 with a BOM, or the
// ANSI code page. A missing or unreadable file reads as empty, so every
// setting keeps its default, as GetPrivateProfile*W did.
IniDocument LoadIniDocument(const std::wstring& path)
{
  std::ifstream in(std::filesystem::path(path), std::ios::binary);
  if (!in) {
    return {};
  }
  // Far above any real SkyrimDiagHelper.ini; a stray huge file is not read whole.
  constexpr std::size_t kMaxIniBytes = std::size_t{4} * 1024 * 1024;
  in.seekg(0, std::ios::end);
  const auto end = in.tellg();
  in.seekg(0, std::ios::beg);
  const std::size_t size = end >= 0 ? (std::min)(static_cast<std::size_t>(end), kMaxIniBytes) : kMaxIniBytes;
  std::string bytes(size, '\0');
  in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  bytes.resize(static_cast<std::size_t>(in.gcount()));
  return IniDocument::Parse(DecodeIniBytes(bytes, &AnsiToWide));
}

std::wstring IniPath()
{
  auto dir = ExeDir();
  if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') {
    dir.push_back(L'\\');
  }
  dir += L"SkyrimDiagHelper.ini";
  return dir;
}

std::uint32_t ReadIniUint32Clamped(
  const IniDocument& ini,
  const wchar_t* section,
  const wchar_t* key,
  int def,
  std::uint32_t minValue,
  std::uint32_t maxValue)
{
  const int raw = ReadIniProfileInt(ini, section, key, def);
  const auto clamped = std::clamp<long long>(
    static_cast<long long>(raw),
    static_cast<long long>(minValue),
    static_cast<long long>(maxValue));
  return static_cast<std::uint32_t>(clamped);
}

std::wstring ResolveEffectiveOutputDir(const std::wstring& rawOutputDir)
{
  return ResolveOutputDirAgainstExeDir(rawOutputDir, ExeDir(), kDefaultOutputSubdir).wstring();
}

// Where captures go when OutputDir cannot be written: the default folder next
// to the helper, then %LOCALAPPDATA%\SkyrimDiag\<default subfolder>.
std::vector<std::wstring> OutputDirFallbacks(const std::wstring& outputDir)
{
  std::vector<std::wstring> out;
  const auto add = [&](const std::filesystem::path& candidate) {
    if (candidate.empty()) {
      return;
    }
    const std::wstring normalized = candidate.lexically_normal().wstring();
    if (CompareStringOrdinal(normalized.c_str(), -1, outputDir.c_str(), -1, TRUE) == CSTR_EQUAL) {
      return;
    }
    for (const auto& existing : out) {
      if (CompareStringOrdinal(normalized.c_str(), -1, existing.c_str(), -1, TRUE) == CSTR_EQUAL) {
        return;
      }
    }
    out.push_back(normalized);
  };
  if (const auto exeDir = ExeDir(); !exeDir.empty()) {
    add(ResolveOutputDirAgainstExeDir(L"", exeDir, kDefaultOutputSubdir));
  }
  std::vector<wchar_t> buf(32768, L'\0');
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf.data(), static_cast<DWORD>(buf.size()));
  if (n > 0 && n < buf.size()) {
    const std::filesystem::path localAppData(std::wstring(buf.data(), n));
    if (localAppData.is_absolute()) {
      add(localAppData / L"SkyrimDiag" / kDefaultOutputSubdir);
    }
  }
  return out;
}

}  // namespace

HelperConfig LoadConfig(std::wstring* err)
{
  HelperConfig cfg{};

  const IniDocument ini = LoadIniDocument(IniPath());

  cfg.hangThresholdInGameSec = ReadIniUint32Clamped(
    ini, L"SkyrimDiagHelper", L"HangThresholdInGameSec", 10, 1, 3600);
  cfg.hangThresholdInMenuSec = ReadIniUint32Clamped(
    ini, L"SkyrimDiagHelper", L"HangThresholdInMenuSec", 30, 1, 7200);
  cfg.hangThresholdLoadingSec = ReadIniUint32Clamped(
    ini, L"SkyrimDiagHelper", L"HangThresholdLoadingSec", 600, 1, 7200);

  const auto dumpMode = ReadIniUint32Clamped(
    ini, L"SkyrimDiagHelper", L"DumpMode", 1, 0, 2);
  if (dumpMode == 0) {
    cfg.dumpMode = DumpMode::kMini;
  } else if (dumpMode == 2) {
    cfg.dumpMode = DumpMode::kFull;
  } else {
    cfg.dumpMode = DumpMode::kDefault;
  }
  cfg.dumpAllModuleCode = ReadIniProfileInt(ini, L"SkyrimDiagHelper", L"DumpAllModuleCode", 0) != 0;

  cfg.outputDir = ResolveEffectiveOutputDir(ReadIniString(ini, L"SkyrimDiagHelper", L"OutputDir", L""));
  cfg.outputDirFallbacks = OutputDirFallbacks(cfg.outputDir);

  cfg.enableManualCaptureHotkey =
    ReadIniProfileInt(ini, L"SkyrimDiagHelper", L"EnableManualCaptureHotkey", 1) != 0;
  std::wstring warning;
  const std::wstring hotkeyText = ReadIniString(ini, L"SkyrimDiagHelper", L"ManualCaptureHotkey", L"");
  if (!hotkeyText.empty()) {
    if (const auto hotkey = ParseManualCaptureHotkey(hotkeyText)) {
      cfg.manualCaptureHotkey = *hotkey;
    } else {
      warning = L"ManualCaptureHotkey=" + hotkeyText + L" is not a valid hotkey; using the default " +
        FormatManualCaptureHotkey(cfg.manualCaptureHotkey) + L".";
    }
  }
  cfg.enableCompatibilityPreflight =
    ReadIniProfileInt(ini, L"SkyrimDiagHelper", L"EnableCompatibilityPreflight", 1) != 0;

  cfg.autoAnalyzeDump = ReadIniProfileInt(ini, L"SkyrimDiagHelper", L"AutoAnalyzeDump", 1) != 0;
  cfg.allowOnlineSymbols = ReadIniProfileInt(ini, L"SkyrimDiagHelper", L"AllowOnlineSymbols", 0) != 0;
  cfg.enableWerDumpFallbackHint =
    ReadIniProfileInt(ini, L"SkyrimDiagHelper", L"EnableWerDumpFallbackHint", 1) != 0;
  cfg.preserveFilteredCrashDumps =
    ReadIniProfileInt(ini, L"SkyrimDiagHelper", L"PreserveFilteredCrashDumps", 0) != 0;
  cfg.enableCleanExitEvidenceQuarantine =
    ReadIniProfileInt(ini, L"SkyrimDiagHelper", L"EnableCleanExitEvidenceQuarantine", 1) != 0;
  cfg.dumpToolExe = ReadIniString(
    ini,
    L"SkyrimDiagHelper",
    L"DumpToolExe",
    L"SkyrimDiagWinUI\\SkyrimDiagDumpToolWinUI.exe");
  cfg.autoOpenViewerOnCrash = ReadIniProfileInt(ini, L"SkyrimDiagHelper", L"AutoOpenViewerOnCrash", 1) != 0;
  cfg.autoOpenCrashOnlyIfProcessExited =
    ReadIniProfileInt(ini, L"SkyrimDiagHelper", L"AutoOpenCrashOnlyIfProcessExited", 1) != 0;
  cfg.autoOpenCrashWaitForExitMs = ReadIniUint32Clamped(
    ini, L"SkyrimDiagHelper", L"AutoOpenCrashWaitForExitMs", 2000, 0, 10000);
  cfg.enableAutoRecaptureOnUnknownCrash =
    ReadIniProfileInt(ini, L"SkyrimDiagHelper", L"EnableAutoRecaptureOnUnknownCrash", 0) != 0;
  cfg.autoRecaptureUnknownBucketThreshold = ReadIniUint32Clamped(
    ini, L"SkyrimDiagHelper", L"AutoRecaptureUnknownBucketThreshold", 2, 1, 10);
  cfg.autoRecaptureAnalysisTimeoutSec = ReadIniUint32Clamped(
    ini, L"SkyrimDiagHelper", L"AutoRecaptureAnalysisTimeoutSec", 20, 1, 300);
  cfg.autoOpenViewerOnHang = ReadIniProfileInt(ini, L"SkyrimDiagHelper", L"AutoOpenViewerOnHang", 1) != 0;
  cfg.autoOpenViewerOnManualCapture =
    ReadIniProfileInt(ini, L"SkyrimDiagHelper", L"AutoOpenViewerOnManualCapture", 0) != 0;
  cfg.autoOpenHangAfterProcessExit =
    ReadIniProfileInt(ini, L"SkyrimDiagHelper", L"AutoOpenHangAfterProcessExit", 1) != 0;
  cfg.autoOpenHangDelayMs = ReadIniUint32Clamped(
    ini, L"SkyrimDiagHelper", L"AutoOpenHangDelayMs", 2000, 0, 10000);
  cfg.autoOpenViewerUnderWine =
    ReadIniProfileInt(ini, L"SkyrimDiagHelper", L"AutoOpenViewerUnderWine", 0) != 0;
  cfg.autoOpenViewerBeginnerMode =
    ReadIniProfileInt(ini, L"SkyrimDiagHelper", L"AutoOpenViewerBeginnerMode", 1) != 0;
  cfg.enablePssSnapshotForFreeze =
    ReadIniProfileInt(ini, L"SkyrimDiagHelper", L"EnablePssSnapshotForFreeze", 0) != 0;

  cfg.enableAdaptiveLoadingThreshold =
    ReadIniProfileInt(ini, L"SkyrimDiagHelper", L"EnableAdaptiveLoadingThreshold", 1) != 0;
  cfg.adaptiveLoadingMinSec = ReadIniUint32Clamped(
    ini, L"SkyrimDiagHelper", L"AdaptiveLoadingMinSec", 120, 30, 3600);
  cfg.adaptiveLoadingMinExtraSec = ReadIniUint32Clamped(
    ini, L"SkyrimDiagHelper", L"AdaptiveLoadingMinExtraSec", 120, 0, 1800);
  cfg.adaptiveLoadingMaxSec = ReadIniUint32Clamped(
    ini, L"SkyrimDiagHelper", L"AdaptiveLoadingMaxSec", 1800, 60, 7200);
  if (cfg.adaptiveLoadingMaxSec < cfg.adaptiveLoadingMinSec) {
    cfg.adaptiveLoadingMaxSec = cfg.adaptiveLoadingMinSec;
  }

  cfg.suppressHangWhenNotForeground =
    ReadIniProfileInt(ini, L"SkyrimDiagHelper", L"SuppressHangWhenNotForeground", 1) != 0;

  cfg.foregroundGraceSec = ReadIniUint32Clamped(
    ini, L"SkyrimDiagHelper", L"ForegroundGraceSec", 5, 0, 60);

  cfg.enableEtwCaptureOnHang =
    ReadIniProfileInt(ini, L"SkyrimDiagHelper", L"EnableEtwCaptureOnHang", 0) != 0;
  cfg.etwWprExe = ReadIniString(ini, L"SkyrimDiagHelper", L"EtwWprExe", L"wpr.exe");

  cfg.etwHangProfile = ReadIniString(ini, L"SkyrimDiagHelper", L"EtwHangProfile", L"");
  if (cfg.etwHangProfile.empty()) {
    // Backward compatibility for older ini files.
    cfg.etwHangProfile = ReadIniString(ini, L"SkyrimDiagHelper", L"EtwProfile", L"GeneralProfile");
  }
  cfg.etwHangFallbackProfile = ReadIniString(ini, L"SkyrimDiagHelper", L"EtwHangFallbackProfile", L"");

  cfg.etwMaxDurationSec = ReadIniUint32Clamped(
    ini, L"SkyrimDiagHelper", L"EtwMaxDurationSec", 20, 1, 300);

  cfg.enableEtwCaptureOnCrash =
    ReadIniProfileInt(ini, L"SkyrimDiagHelper", L"EnableEtwCaptureOnCrash", 0) != 0;

  cfg.etwCrashProfile = ReadIniString(ini, L"SkyrimDiagHelper", L"EtwCrashProfile", L"GeneralProfile");
  if (cfg.etwCrashProfile.empty()) {
    // Treat empty as default for safer behavior.
    cfg.etwCrashProfile = L"GeneralProfile";
  }

  const std::uint32_t etwCrashCaptureSeconds = ReadIniUint32Clamped(
    ini, L"SkyrimDiagHelper", L"EtwCrashCaptureSeconds", 8, 1, 30);
  cfg.etwCrashCaptureSeconds = etwCrashCaptureSeconds;

  cfg.enableIncidentManifest =
    ReadIniProfileInt(ini, L"SkyrimDiagHelper", L"EnableIncidentManifest", 1) != 0;
  cfg.incidentManifestIncludeConfigSnapshot =
    ReadIniProfileInt(ini, L"SkyrimDiagHelper", L"IncidentManifestIncludeConfigSnapshot", 1) != 0;
  cfg.suppressDuringGrassCaching =
    ReadIniProfileInt(ini, L"SkyrimDiagHelper", L"SuppressDuringGrassCaching", 1) != 0;

  cfg.maxCrashDumps = ReadIniUint32Clamped(
    ini, L"SkyrimDiagHelper", L"MaxCrashDumps", 20, 0, 500);
  cfg.maxHangDumps = ReadIniUint32Clamped(
    ini, L"SkyrimDiagHelper", L"MaxHangDumps", 20, 0, 500);
  cfg.maxManualDumps = ReadIniUint32Clamped(
    ini, L"SkyrimDiagHelper", L"MaxManualDumps", 20, 0, 500);
  cfg.maxEtwTraces = ReadIniUint32Clamped(
    ini, L"SkyrimDiagHelper", L"MaxEtwTraces", 5, 0, 100);
  cfg.maxTotalDumpGB = ReadIniUint32Clamped(
    ini, L"SkyrimDiagHelper", L"MaxTotalDumpGB", 5, 0, 1024);

  cfg.maxHelperLogBytes = ReadIniUint32Clamped(
    ini, L"SkyrimDiagHelper", L"MaxHelperLogBytes", 8 * 1024 * 1024, 0, 100 * 1024 * 1024);
  cfg.maxHelperLogFiles = ReadIniUint32Clamped(
    ini, L"SkyrimDiagHelper", L"MaxHelperLogFiles", 3, 0, 20);
  cfg.maxHelperSessionLogs = ReadIniUint32Clamped(
    ini, L"SkyrimDiagHelper", L"MaxHelperSessionLogs", 5, 1, 20);

  if (err) {
    *err = warning;
  }
  return cfg;
}

}  // namespace skydiag::helper
