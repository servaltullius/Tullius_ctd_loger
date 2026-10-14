#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "SkseLogParser.h"

namespace skydiag::helper {

struct PluginMeta
{
  std::string filename;
  float header_version = 0.0f;
  bool is_esl = false;
  bool is_active = false;
  bool slot_type_known = false;
  std::vector<std::string> masters;
};

struct SkseLogScan
{
  // matched / no_matching_log / not_found / no_image_base / error; empty when not collected
  std::string status;
  SkseLogSummary summary;  // filled when status == "matched"
  // The matched log was written after the game process started. Without the
  // start time only the image base ties it to this session, and the same
  // executable can map at the same base again within one boot (audit F17).
  bool time_checked = false;
};

// The MO2 instance and profile the game ran under, read while it is still
// running. Analyzing a kept dump later must not use whatever profile and mod
// folders MO2 has on disk by then (audit F05). Strings are UTF-8.
struct Mo2Capture
{
  // captured / no_ini / no_profile / no_modlist; empty when MO2 was not detected
  std::string status;
  std::string ini_path;
  std::string base_dir;
  std::string mods_dir;
  std::string profiles_dir;
  std::string overwrite_dir;
  std::string profile;
  // The profile's enabled mods (modlist.txt "+" entries), highest priority first.
  std::vector<std::string> enabled_mods;
  bool enabled_mods_truncated = false;
};

// ModOrganizer.ini values the capture needs, decoded from Qt's INI format.
struct Mo2IniSettings
{
  std::string selected_profile;
  std::string base_directory;
  std::string mod_directory;
  std::string profiles_directory;
  std::string overwrite_directory;
};

Mo2IniSettings ParseMo2Ini(const std::string& content);
// MO2 path settings default to "%BASE_DIR%/<leaf>"; the base directory itself
// defaults to the folder holding ModOrganizer.ini.
std::filesystem::path ResolveMo2Directory(
  const std::string& configured,
  const std::filesystem::path& baseDir,
  const wchar_t* defaultLeaf);
std::vector<std::string> ParseMo2EnabledModsWinnerFirst(const std::string& modlistContent);

struct PluginScanResult
{
  std::string game_exe_version;
  std::string plugins_source;  // "standard", "mo2_profile", "fallback", "error"
  bool mo2_detected = false;
  Mo2Capture mo2;
  // True when `plugins` also lists the base masters and Skyrim.ccc plugins the
  // game loads without a plugins.txt entry. Older scans listed only plugins.txt.
  bool implicit_plugins_included = false;
  // The implicit masters (base game, Skyrim.ccc, which lists _ResourcePack.esl
  // on 1.6.1130+) were checked in the Data folder the game sees: no MO2, or
  // this helper runs inside MO2's virtual file system. Then an implicit master
  // missing from `plugins` is really absent (audit F12); otherwise it may sit
  // in a mod folder the helper cannot see (CC files kept as an MO2 mod).
  bool implicit_masters_verified = false;
  std::vector<PluginMeta> plugins;
  std::string error;
  SkseLogScan skse_log;
};

bool ParseTes4Header(const std::uint8_t* data, std::size_t size, PluginMeta& out);
std::vector<std::string> ParsePluginsTxt(const std::string& content);
// Plugin names from Skyrim.ccc, the Creation Club files the game loads on its own.
std::vector<std::string> ParseCreationClubContentList(const std::string& content);

bool TryResolveGameExeDir(HANDLE processHandle, std::filesystem::path& outDir);
std::vector<std::wstring> CollectModuleFilenamesBestEffort(std::uint32_t pid);
std::vector<std::wstring> CollectModulePathsBestEffort(std::uint32_t pid);
// Base address of the process executable; 0 when it cannot be read.
std::uint64_t QueryMainModuleBaseBestEffort(std::uint32_t pid);

// When the process was created; empty when it cannot be read. Works on the
// handle of a process that has already exited.
std::optional<std::filesystem::file_time_type> QueryProcessStartTimeBestEffort(HANDLE process);

// Finds the skse64.log written by the game session whose executable is mapped
// at gameImageBase and, when processStart is known, that was written after the
// process started. SKSE rewrites the log from inside the game process, so an
// older one is left over from an earlier launch, even at the same base.
SkseLogScan CollectSkseLogBestEffort(
  std::uint64_t gameImageBase,
  std::optional<std::filesystem::file_time_type> processStart = std::nullopt);
// The matching step of CollectSkseLogBestEffort over explicit log files.
SkseLogScan MatchSkseLogFiles(
  const std::vector<std::filesystem::path>& logPaths,
  std::uint64_t gameImageBase,
  std::optional<std::filesystem::file_time_type> processStart = std::nullopt);

PluginScanResult ScanPlugins(
  const std::filesystem::path& gameExeDir,
  const std::vector<std::wstring>& moduleFilenames,
  const std::vector<std::wstring>* modulePaths = nullptr);

std::string SerializePluginScanResult(const PluginScanResult& result);

}  // namespace skydiag::helper
