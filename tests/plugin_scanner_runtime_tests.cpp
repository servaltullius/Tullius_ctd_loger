// ScanPlugins against a throwaway MO2-style game folder.
//
// Field case (v0.2.59-rc3): MO2 profiles do not list the base masters or the
// Anniversary Edition Creation Club files, which the game loads on its own
// from Skyrim.ccc. Without them every plugin that depends on a CC file was
// reported with a missing master.

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "SkyrimDiagHelper/PluginScanner.h"

namespace {

using skydiag::helper::ParseCreationClubContentList;
using skydiag::helper::PluginScanResult;
using skydiag::helper::ScanPlugins;

void Require(bool condition, const char* message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void WriteText(const std::filesystem::path& path, const std::string& text)
{
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << text;
  Require(static_cast<bool>(out), "failed to write a test file");
}

std::vector<std::string> ActiveNames(const PluginScanResult& result)
{
  std::vector<std::string> names;
  for (const auto& plugin : result.plugins) {
    if (plugin.is_active) {
      names.push_back(plugin.filename);
    }
  }
  return names;
}

bool Contains(const std::vector<std::string>& names, const std::string& name)
{
  return std::find(names.begin(), names.end(), name) != names.end();
}

void TestCreationClubListParsing()
{
  const auto names = ParseCreationClubContentList("\xEF\xBB\xBF" "ccA.esl\r\n\r\n  ccB.esm  \n# note\n");
  Require(names.size() == 2u && names[0] == "ccA.esl" && names[1] == "ccB.esm", "Skyrim.ccc lines must parse");
}

void TestImplicitPluginsAreActiveUnderMo2()
{
  const auto root = std::filesystem::temp_directory_path() /
    (L"skydiag_plugin_scanner_" + std::to_wstring(GetCurrentProcessId()));
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  const auto data = root / L"Data";
  WriteText(root / L"ModOrganizer.ini", "[General]\nselected_profile=@ByteArray(Default)\n");
  // The profile lists one CC file itself; it must not appear twice.
  WriteText(root / L"profiles" / L"Default" / L"plugins.txt", "*ccListed.esl\n*Mod.esp\n");
  WriteText(root / L"Skyrim.ccc", "ccPresent.esl\nccListed.esl\nccNotInstalled.esl\n");
  for (const auto* name : { L"Skyrim.esm", L"Update.esm", L"ccPresent.esl", L"ccListed.esl", L"Mod.esp" }) {
    WriteText(data / name, "");
  }

  const auto result = ScanPlugins(root, { L"usvfs_x64.dll" }, nullptr);
  std::filesystem::remove_all(root, ec);

  Require(result.plugins_source == "mo2_profile", "the MO2 profile plugins.txt must be used");
  Require(result.implicit_plugins_included, "the scan must say it lists implicit plugins");
  Require(
    skydiag::helper::SerializePluginScanResult(result).find("\"implicit_plugins_included\":true") != std::string::npos,
    "the sidecar must carry the implicit-plugins flag");
  const auto names = ActiveNames(result);
  Require(Contains(names, "Skyrim.esm") && Contains(names, "Update.esm"), "installed base masters must be active");
  Require(!Contains(names, "Dawnguard.esm"), "a base master that is not installed must not be added");
  Require(Contains(names, "ccPresent.esl"), "an installed Skyrim.ccc plugin must be active");
  Require(!Contains(names, "ccNotInstalled.esl"), "a Skyrim.ccc plugin that is not installed must not be added");
  Require(Contains(names, "Mod.esp"), "plugins.txt entries must stay active");
  Require(
    std::count(names.begin(), names.end(), "ccListed.esl") == 1,
    "a plugin both in Skyrim.ccc and plugins.txt must be listed once");
}

}  // namespace

int main()
{
  try {
    TestCreationClubListParsing();
    TestImplicitPluginsAreActiveUnderMo2();
    std::puts("plugin scanner runtime tests passed");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
