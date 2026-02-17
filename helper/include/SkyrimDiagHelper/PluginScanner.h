#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace skydiag::helper {

struct PluginMeta
{
  std::string filename;
  float header_version = 0.0f;
  bool is_esl = false;
  bool is_active = false;
  std::vector<std::string> masters;
};

struct PluginScanResult
{
  std::string game_exe_version;
  std::string plugins_source;  // "standard", "mo2_profile", "fallback", "error"
  bool mo2_detected = false;
  std::vector<PluginMeta> plugins;
  std::string error;
};

bool ParseTes4Header(const std::uint8_t* data, std::size_t size, PluginMeta& out);
std::vector<std::string> ParsePluginsTxt(const std::string& content);

bool TryResolveGameExeDir(HANDLE processHandle, std::filesystem::path& outDir);
std::vector<std::wstring> CollectModuleFilenamesBestEffort(std::uint32_t pid);

PluginScanResult ScanPlugins(
  const std::filesystem::path& gameExeDir,
  const std::vector<std::wstring>& moduleFilenames);

std::string SerializePluginScanResult(const PluginScanResult& result);

}  // namespace skydiag::helper
