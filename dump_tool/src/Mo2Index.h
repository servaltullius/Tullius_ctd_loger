#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skydiag::dump_tool {

struct Mo2Index
{
  // Where the profile and mod list come from. kCrashTime: the helper captured
  // them while the game ran. kAnalysisTime: read from disk when the dump is
  // analyzed, so another profile or changed mods may be described (audit F05).
  enum class Source
  {
    kAnalysisTime,
    kCrashTime,
  };

  Source source = Source::kAnalysisTime;
  std::filesystem::path base;
  std::filesystem::path modsDir;
  std::filesystem::path overwriteDir;
  std::filesystem::path profilesDir;
  std::filesystem::path profileDir;
  std::wstring profileName;
  std::filesystem::path modlistPath;
  std::vector<std::filesystem::path> modDirs;
  std::vector<std::wstring> modNames;
};

// The MO2 instance and profile the helper recorded while the game ran (the
// "mo2" object of its plugin scan, status "captured").
struct CapturedMo2State
{
  std::filesystem::path base;
  std::filesystem::path modsDir;
  std::filesystem::path overwriteDir;
  std::filesystem::path profilesDir;
  std::wstring profileName;
  std::vector<std::wstring> enabledModsWinnerFirst;
};

std::optional<CapturedMo2State> TryParseCapturedMo2State(std::string_view pluginScanJsonUtf8);

// Mod folders removed since the crash simply provide nothing.
Mo2Index BuildMo2IndexFromCapture(const CapturedMo2State& state);

// Best-effort: detect "...\\mods\\<ModName>\\..." (MO2)
std::wstring InferMo2ModNameFromPath(std::wstring_view fullPath);

// A mod installed from a Nexus archive without MO2 metadata keeps the
// archive's name: "SmoothCam 41252 1.8.1 2026",
// "CrashLogger 59818 1.25.0 2026-08-21T09-21Z yIjvuh041". This drops the
// trailing Nexus mod ID, version, upload date and download hash, giving
// "SmoothCam" and "CrashLogger". Any other folder name comes back unchanged.
std::wstring CleanMo2ModFolderName(std::wstring_view folderName);

// Best-effort: infer MO2 base folder from any module path containing "\\mods\\".
// Example:
//   "G:\\Modding\\MO2\\mods\\SomeMod\\SKSE\\Plugins\\X.dll" -> "G:\\Modding\\MO2"
std::optional<std::filesystem::path> TryInferMo2BaseDirFromModulePaths(const std::vector<std::wstring>& modulePaths);

// Build an index for mapping Data-relative paths (e.g. "meshes\\foo.nif") to MO2 providers.
// modulePaths are used only to infer the MO2 base folder.
std::optional<Mo2Index> TryBuildMo2IndexFromModulePaths(const std::vector<std::wstring>& modulePaths);

// Returns provider names like "overwrite", "<ModName>" ... (best-effort).
std::vector<std::wstring> FindMo2ProvidersForDataPath(const Mo2Index& idx, std::wstring_view relPath, std::size_t maxProviders);

}  // namespace skydiag::dump_tool
