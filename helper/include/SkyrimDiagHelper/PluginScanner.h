#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
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
  // matched / no_matching_log / not_found / no_image_base; empty when not collected
  std::string status;
  SkseLogSummary summary;  // filled when status == "matched"
};

struct PluginScanResult
{
  std::string game_exe_version;
  std::string plugins_source;  // "standard", "mo2_profile", "fallback", "error"
  bool mo2_detected = false;
  // True when `plugins` also lists the base masters and Skyrim.ccc plugins the
  // game loads without a plugins.txt entry. Older scans listed only plugins.txt.
  bool implicit_plugins_included = false;
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

// Finds the skse64.log written by the game session whose executable is mapped
// at gameImageBase. Only a log from that session is returned, so a log left
// over from an earlier launch is never reported as current.
SkseLogScan CollectSkseLogBestEffort(std::uint64_t gameImageBase);
// The matching step of CollectSkseLogBestEffort over explicit log files.
SkseLogScan MatchSkseLogFiles(const std::vector<std::filesystem::path>& logPaths, std::uint64_t gameImageBase);

PluginScanResult ScanPlugins(
  const std::filesystem::path& gameExeDir,
  const std::vector<std::wstring>& moduleFilenames,
  const std::vector<std::wstring>* modulePaths = nullptr);

std::string SerializePluginScanResult(const PluginScanResult& result);

}  // namespace skydiag::helper
