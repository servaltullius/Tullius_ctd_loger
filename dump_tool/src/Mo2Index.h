#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skydiag::dump_tool {

struct Mo2Index
{
  std::filesystem::path base;
  std::filesystem::path modsDir;
  std::filesystem::path overwriteDir;
  std::vector<std::filesystem::path> modDirs;
  std::vector<std::wstring> modNames;
};

// Best-effort: detect "...\\mods\\<ModName>\\..." (MO2)
std::wstring InferMo2ModNameFromPath(std::wstring_view fullPath);

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
