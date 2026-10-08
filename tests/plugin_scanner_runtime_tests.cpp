// ScanPlugins against a throwaway MO2-style game folder.
//
// Field case (v0.2.59-rc3): MO2 profiles do not list the base masters or the
// Anniversary Edition Creation Club files, which the game loads on its own
// from Skyrim.ccc. Without them every plugin that depends on a CC file was
// reported with a missing master.

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "SkyrimDiagHelper/PluginScanner.h"

namespace {

using skydiag::helper::MatchSkseLogFiles;
using skydiag::helper::ParseCreationClubContentList;
using skydiag::helper::PluginScanResult;
using skydiag::helper::ScanPlugins;
using skydiag::helper::SerializePluginScanResult;

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

// skse64.log keeps no timestamps; only the log whose imagebase is the running
// game's executable base belongs to the captured session.
void TestSkseLogIsMatchedByImageBase()
{
  const auto root = std::filesystem::temp_directory_path() /
    (L"skydiag_skse_log_" + std::to_wstring(GetCurrentProcessId()));
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  const auto oldLog = root / L"Skyrim Special Edition GOG" / L"SKSE" / L"skse64.log";
  WriteText(oldLog,
    "SKSE64 runtime: initialize (version = 2.2.6 01064920 01DD574A9E6B4D48, os = 6.2 (9200))\r\n"
    "imagebase = 00007FF711110000\r\n"
    "plugin Stale.dll (00000000  00000000) no version data 0 (handle 0)\r\n");
  // "Caf\xE9" is "Café" in the ANSI code pages of the CI runner and most
  // Western systems; SKSE writes DLL names in the ANSI code page.
  const auto currentLog = root / L"Skyrim Special Edition" / L"SKSE" / L"skse64.log";
  WriteText(currentLog,
    "SKSE64 runtime: initialize (version = 2.2.6 01064920 01DD574A9E6B4D48, os = 6.2 (9200))\r\n"
    "imagebase = 00007FF7B4BD0000\r\n"
    "checking plugin Caf\xE9.dll\r\n"
    "plugin Caf\xE9.dll (00000000  00000000) no version data 0 (handle 0)\r\n"
    "checking plugin Good.dll\r\n"
    "plugin Good.dll (00000001 Good 00000001) loaded correctly (handle 1)\r\n");

  // A third folder whose log has the same base but is older than the current one.
  const auto olderSameBase = root / L"Skyrim Special Edition EPIC" / L"SKSE" / L"skse64.log";
  WriteText(olderSameBase,
    "SKSE64 runtime: initialize (version = 2.2.6 01064920 01DD574A9E6B4D48, os = 6.2 (9200))\r\n"
    "imagebase = 00007FF7B4BD0000\r\n"
    "plugin Older.dll (00000000  00000000) no version data 0 (handle 0)\r\n"
    "plugin Older2.dll (00000000  00000000) no version data 0 (handle 0)\r\n");
  std::filesystem::last_write_time(
    olderSameBase, std::filesystem::last_write_time(currentLog) - std::chrono::hours(1));

  const auto none = MatchSkseLogFiles({}, 0x00007FF7B4BD0000ull);
  const auto newestFirst = MatchSkseLogFiles({ currentLog, olderSameBase }, 0x00007FF7B4BD0000ull);
  const auto newestLast = MatchSkseLogFiles({ olderSameBase, currentLog }, 0x00007FF7B4BD0000ull);
  const auto noBase = MatchSkseLogFiles({ currentLog }, 0);
  const auto stale = MatchSkseLogFiles({ oldLog }, 0x00007FF7B4BD0000ull);
  const auto matched = MatchSkseLogFiles({ oldLog, currentLog }, 0x00007FF7B4BD0000ull);
  std::filesystem::remove_all(root, ec);

  Require(none.status == "not_found", "no log file is reported as not_found");
  Require(noBase.status == "no_image_base", "without the game's image base no log is trusted");
  Require(stale.status == "no_matching_log", "a log from another session must not be used");
  Require(stale.summary.issues.empty(), "a stale log must not leak its plugin results");
  Require(matched.status == "matched", "the log of the running session must be found");
  Require(matched.summary.checked_count == 2u && matched.summary.loaded_count == 1u, "plugin counts come from the matched log");
  Require(matched.summary.issues.size() == 1u, "only the DLL SKSE did not load is listed");
  for (const auto* scan : { &newestFirst, &newestLast }) {
    Require(
      scan->status == "matched" && scan->summary.issues.size() == 1u && scan->summary.checked_count == 2u,
      "among logs with the same image base the most recently written one wins");
  }

  PluginScanResult scan{};
  scan.skse_log = matched;
  // nlohmann throws on invalid UTF-8, so a successful dump and parse proves the
  // ANSI name was converted.
  const auto json = nlohmann::json::parse(SerializePluginScanResult(scan));
  const auto& log = json.at("skse_log");
  Require(log.at("status") == "matched", "the scan JSON carries the match status");
  Require(log.at("checked_count") == 2 && log.at("loaded_count") == 1 && log.at("issue_count") == 1, "counts are serialized");
  const auto& issue = log.at("issues").at(0);
  Require(issue.at("status") == "no version data" && issue.at("code") == 0, "the SKSE status is serialized as written");
  Require(issue.at("dll").get<std::string>().size() > std::string("Caf.dll").size(), "the ANSI DLL name survives as UTF-8");

  PluginScanResult staleScan{};
  staleScan.skse_log = stale;
  const auto staleJson = nlohmann::json::parse(SerializePluginScanResult(staleScan));
  Require(staleJson.at("skse_log").at("status") == "no_matching_log", "an unmatched status is still recorded");
  Require(!staleJson.at("skse_log").contains("issues"), "an unmatched log contributes no plugin results");
}

}  // namespace

int main()
{
  try {
    TestCreationClubListParsing();
    TestImplicitPluginsAreActiveUnderMo2();
    TestSkseLogIsMatchedByImageBase();
    std::puts("plugin scanner runtime tests passed");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
