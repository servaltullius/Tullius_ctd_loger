// ScanPlugins against a throwaway MO2-style game folder.
//
// Field case (v0.2.59-rc3): MO2 profiles do not list the base masters or the
// Anniversary Edition Creation Club files, which the game loads on its own
// from Skyrim.ccc. Without them every plugin that depends on a CC file was
// reported with a missing master.

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
  WriteText(root / L"portable.txt", "");  // MO2 runs this portable install whatever the registry says
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
  // This test process is not inside MO2's virtual file system, so it may not
  // see CC files kept in a mod folder: the implicit masters are not verified.
  Require(!result.implicit_masters_verified, "implicit masters are unverified outside the game's Data view");
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

// Audit F05: the analyzer used whatever profile MO2 had on disk when a dump
// was analyzed. The helper now records the instance, the profile and its
// enabled mods while the game runs, honouring MO2's [Settings] directories.
void TestMo2StateIsCapturedWithConfiguredDirectories()
{
  const auto ini = skydiag::helper::ParseMo2Ini(
    "\xEF\xBB\xBF[General]\nselected_profile=@ByteArray(Caf\\xe9)\n"
    "[Settings]\nbase_directory=\"D:\\\\Mod \\\"Lists\\\"\"\nmod_directory=%BASE_DIR%/caf\\x00e9\n"
    "[Other]\nprofiles_directory=ignored\n");
  Require(ini.selected_profile == "Caf\xE9", "an @ByteArray \\xNN escape is one byte");
  Require(ini.base_directory == "D:\\Mod \"Lists\"", "quoted values and \\\\ \\\" escapes decode");
  Require(ini.mod_directory == "%BASE_DIR%/caf\xC3\xA9", "a \\xNNNN escape is a character, written as UTF-8");
  Require(ini.profiles_directory.empty(), "only keys of the [Settings] section count");

  const auto root = std::filesystem::temp_directory_path() /
    (L"skydiag_plugin_scanner_mo2_" + std::to_wstring(GetCurrentProcessId()));
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  const auto instance = root / L"instance";
  std::string iniText = "[General]\nselected_profile=@ByteArray(Crash Profile)\n[Settings]\nbase_directory=";
  iniText += instance.generic_string();
  iniText += "\nprofiles_directory=%BASE_DIR%/my profiles\nmod_directory=%BASE_DIR%\\\\my mods\n";
  WriteText(root / L"ModOrganizer.ini", iniText);
  WriteText(root / L"portable.txt", "");  // MO2 runs this portable install whatever the registry says
  // The default profiles folder exists too; the configured one must be used.
  WriteText(root / L"profiles" / L"Crash Profile" / L"plugins.txt", "*Wrong.esp\n");
  const auto profile = instance / L"my profiles" / L"Crash Profile";
  WriteText(profile / L"plugins.txt", "*Mod.esp\n");
  WriteText(profile / L"modlist.txt", "+Winner\n-Disabled\n*DLC: Dawnguard\n+Low\n+winner\n");
  WriteText(root / L"Data" / L"Mod.esp", "");

  const auto result = ScanPlugins(root, { L"usvfs_x64.dll" }, nullptr);
  const auto json = nlohmann::json::parse(SerializePluginScanResult(result));

  // A selected profile that no longer exists is reported, not guessed.
  WriteText(root / L"ModOrganizer.ini", "[General]\nselected_profile=@ByteArray(Gone)\n");
  const auto gone = ScanPlugins(root, { L"usvfs_x64.dll" }, nullptr);
  std::filesystem::remove_all(root, ec);

  Require(result.mo2.status == "captured", "the MO2 state must be captured");
  Require(result.mo2.profile == "Crash Profile", "the selected profile is recorded");
  Require(
    result.mo2.enabled_mods == std::vector<std::string>({ "Winner", "Low" }),
    "only enabled mods are kept, winner first and without duplicates");
  Require(
    std::filesystem::u8path(result.mo2.mods_dir) == (instance / L"my mods").lexically_normal(),
    "%BASE_DIR% and Qt escapes resolve in mod_directory");
  Require(result.plugins_source == "mo2_profile", "plugins.txt comes from the configured profiles directory");
  Require(Contains(ActiveNames(result), "Mod.esp") && !Contains(ActiveNames(result), "Wrong.esp"),
    "the default profiles folder must not be read when another is configured");
  Require(json.at("mo2").at("status") == "captured" && json.at("mo2").at("enabled_mods").size() == 2u,
    "the capture is serialized into the scan");
  Require(gone.mo2.status == "no_profile" && gone.mo2.enabled_mods.empty(), "a missing profile is not guessed");
}

// plugins.txt and Skyrim.ccc are written in the ANSI code page, not UTF-8. A
// name like "Caf\xE9.esp" threw out of the scan (u8path, json dump) and, with
// no catch above it, took the helper and the hang dump down with it.
void TestAnsiPluginNamesDoNotThrow()
{
  const auto root = std::filesystem::temp_directory_path() /
    (L"skydiag_plugin_scanner_ansi_" + std::to_wstring(GetCurrentProcessId()));
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  WriteText(root / L"ModOrganizer.ini", "[General]\nselected_profile=@ByteArray(Default)\n");
  WriteText(root / L"portable.txt", "");  // MO2 runs this portable install whatever the registry says
  WriteText(root / L"profiles" / L"Default" / L"plugins.txt", "*Caf\xE9.esp\n*Mod.esp\n");
  WriteText(root / L"Skyrim.ccc", "cc\xE9.esl\n");
  WriteText(root / L"Data" / L"Mod.esp", "");
  // The file the game would open for that line: the same bytes through the ANSI code page.
  wchar_t wideName[64]{};
  MultiByteToWideChar(CP_ACP, 0, "Caf\xE9.esp", -1, wideName, 64);
  WriteText(root / L"Data" / wideName, "");

  PluginScanResult result{};
  std::string json;
  try {
    result = ScanPlugins(root, { L"usvfs_x64.dll" }, nullptr);
    json = SerializePluginScanResult(result);
  } catch (...) {
    std::filesystem::remove_all(root, ec);
    Require(false, "an ANSI plugin name must not throw out of the scan");
  }
  std::filesystem::remove_all(root, ec);

  const auto parsed = nlohmann::json::parse(json, nullptr, false);
  Require(!parsed.is_discarded(), "the scan JSON must stay valid UTF-8 JSON");
  Require(Contains(ActiveNames(result), "Mod.esp"), "the other plugins must still be scanned");
  char utf8Name[64]{};
  WideCharToMultiByte(CP_UTF8, 0, wideName, -1, utf8Name, 64, nullptr, nullptr);
  Require(Contains(ActiveNames(result), utf8Name), "the ANSI name must be listed, converted to UTF-8");
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
  // Audit F17: an earlier launch in the same boot can share the image base; a
  // log last written before the game started is that launch's.
  const auto started = std::filesystem::last_write_time(currentLog) - std::chrono::minutes(30);
  const auto earlierLaunchOnly = MatchSkseLogFiles({ olderSameBase }, 0x00007FF7B4BD0000ull, started);
  const auto currentAfterStart = MatchSkseLogFiles({ olderSameBase, currentLog }, 0x00007FF7B4BD0000ull, started);
  const auto startUnknown = MatchSkseLogFiles({ olderSameBase }, 0x00007FF7B4BD0000ull);
  std::filesystem::remove_all(root, ec);

  Require(earlierLaunchOnly.status == "no_matching_log", "a log written before the game started is not this session's");
  Require(currentAfterStart.status == "matched" && currentAfterStart.time_checked &&
      currentAfterStart.summary.checked_count == 2u,
    "the log written after the start is matched and marked as time-checked");
  Require(startUnknown.status == "matched" && !startUnknown.time_checked,
    "without the start time the image base still matches, but is not marked time-checked");
  const auto selfStart = skydiag::helper::QueryProcessStartTimeBestEffort(GetCurrentProcess());
  Require(selfStart && *selfStart <= std::filesystem::file_time_type::clock::now(),
    "the start time of a running process is read in file time");

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
  Require(log.at("time_checked") == false, "the scan JSON says whether the log time was checked");
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

void AppendField(std::string& out, const char* type, const std::string& data)
{
  out.append(type, 4);
  const auto size = static_cast<std::uint16_t>(data.size());
  out.push_back(static_cast<char>(size & 0xFF));
  out.push_back(static_cast<char>(size >> 8));
  out += data;
}

// A TES4 record with HEDR 1.71, `masters` MAST/DATA pairs and a trailing INTV.
std::string MakeTes4(std::uint32_t flags, const std::vector<std::string>& masters)
{
  std::string fields;
  const float version = 1.71f;
  std::string hedr(12, '\0');
  std::memcpy(hedr.data(), &version, 4);
  AppendField(fields, "HEDR", hedr);
  AppendField(fields, "CNAM", std::string("author\0", 7));
  for (const auto& master : masters) {
    AppendField(fields, "MAST", master + '\0');
    AppendField(fields, "DATA", std::string(8, '\0'));
  }
  AppendField(fields, "INTV", std::string(4, '\0'));
  std::string record = "TES4";
  const auto dataSize = static_cast<std::uint32_t>(fields.size());
  record.append(reinterpret_cast<const char*>(&dataSize), 4);
  record.append(reinterpret_cast<const char*>(&flags), 4);
  record.append(12, '\0');
  return record + fields;
}

std::vector<std::string> ManyMasters(std::size_t count)
{
  std::vector<std::string> masters;
  for (std::size_t i = 0; i < count; ++i) {
    masters.push_back("A Fairly Long Master Plugin Name Number " + std::to_string(i) + ".esp");
  }
  return masters;
}

std::filesystem::path FreshRoot(const wchar_t* name)
{
  const auto root = std::filesystem::temp_directory_path() / (name + std::to_wstring(GetCurrentProcessId()));
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  return root;
}

// Review M12: only the first 4 KB of a plugin were read, so a plugin with ~90
// or more masters (DynDOLOD.esp, big patches) lost the rest of its list and a
// missing master past that point was never reported.
void TestMastersBeyondFourKilobytesAreRead()
{
  const auto masters = ManyMasters(150);
  const auto tes4 = MakeTes4(0, masters);
  Require(tes4.size() > 8192u, "the synthetic header is larger than the old 4 KB read");

  const auto root = FreshRoot(L"skydiag_plugin_scanner_masters_");
  WriteText(root / L"ModOrganizer.ini", "[General]\nselected_profile=@ByteArray(Default)\n");
  WriteText(root / L"portable.txt", "");
  WriteText(root / L"profiles" / L"Default" / L"plugins.txt", "*Big.esp\n");
  WriteText(root / L"Data" / L"Big.esp", tes4 + std::string(64, 'R'));
  const auto result = ScanPlugins(root, { L"usvfs_x64.dll" }, nullptr);
  const auto json = nlohmann::json::parse(SerializePluginScanResult(result));
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  Require(result.plugins.size() == 1u, "the plugin is scanned");
  const auto& big = result.plugins.front();
  Require(big.masters == masters, "every master of a header larger than 4 KB is read");
  Require(!big.masters_truncated, "a complete header is not marked truncated");
  Require(!json.at("plugins").at(0).contains("masters_truncated"), "the flag is only serialized when set");

  // A record cut off inside the master list says so; one cut off after it
  // (past INTV) still has every master.
  skydiag::helper::PluginMeta cut{};
  Require(skydiag::helper::ParseTes4Header(
            reinterpret_cast<const std::uint8_t*>(tes4.data()), tes4.size() / 2, cut),
    "a cut-off header still parses");
  Require(cut.masters_truncated && !cut.masters.empty() && cut.masters.size() < masters.size(),
    "a header cut inside the master list is marked truncated");
  const auto padded = tes4.substr(0, 4) + std::string("\xFF\xFF\x00\x00", 4) + tes4.substr(8);
  skydiag::helper::PluginMeta past{};
  skydiag::helper::ParseTes4Header(reinterpret_cast<const std::uint8_t*>(padded.data()), padded.size(), past);
  Require(past.masters.size() == masters.size() && !past.masters_truncated,
    "a header cut after the master list keeps every master and is not marked truncated");
}

// Review L5: the game loads every .esl as light; a renamed one without the
// header flag counted as a full plugin toward the 254 limit.
void TestEslExtensionIsLightWithoutTheFlag()
{
  const auto root = FreshRoot(L"skydiag_plugin_scanner_esl_");
  WriteText(root / L"ModOrganizer.ini", "[General]\nselected_profile=@ByteArray(Default)\n");
  WriteText(root / L"portable.txt", "");
  WriteText(root / L"profiles" / L"Default" / L"plugins.txt", "*Renamed.esl\n*Full.esp\n");
  WriteText(root / L"Data" / L"Renamed.esl", MakeTes4(0, {}));
  WriteText(root / L"Data" / L"Full.esp", MakeTes4(0, {}));
  const auto result = ScanPlugins(root, { L"usvfs_x64.dll" }, nullptr);
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  bool sawRenamed = false;
  bool sawFull = false;
  for (const auto& plugin : result.plugins) {
    if (plugin.filename == "Renamed.esl") {
      sawRenamed = true;
      Require(plugin.is_esl && plugin.slot_type_known, "a .esl without the ESL flag is light");
    } else if (plugin.filename == "Full.esp") {
      sawFull = true;
      Require(!plugin.is_esl && plugin.slot_type_known, "a .esp without the flag stays full");
    }
  }
  Require(sawRenamed && sawFull, "both plugins are scanned");
}

// Review M14: a portable ModOrganizer.ini left next to the game won over the
// global instance MO2 actually runs. MO2 runs a portable install holding
// portable.txt, else the registry's CurrentInstance, else the portable one.
void TestMo2InstanceChoiceFollowsMo2()
{
  using skydiag::helper::ChooseMo2InstanceIni;
  using skydiag::helper::Mo2InstanceCandidate;
  const std::filesystem::path stale = L"C:\\Old\\ModOrganizer.ini";
  const std::filesystem::path locked = L"C:\\List\\ModOrganizer.ini";
  const std::filesystem::path global = L"C:\\Users\\u\\AppData\\Local\\ModOrganizer\\SSE\\ModOrganizer.ini";

  const auto prefersGlobal = ChooseMo2InstanceIni({ Mo2InstanceCandidate{ stale, false } }, global);
  Require(prefersGlobal.ini == global && prefersGlobal.source == "global" && prefersGlobal.ambiguous,
    "the registry's global instance wins over an unlocked portable ini, flagged as ambiguous");

  const auto lockedWins = ChooseMo2InstanceIni(
    { Mo2InstanceCandidate{ stale, false }, Mo2InstanceCandidate{ locked, true } }, global);
  Require(lockedWins.ini == locked && lockedWins.source == "portable_locked" && !lockedWins.ambiguous,
    "a portable install locked by portable.txt always wins");

  const auto portableOnly = ChooseMo2InstanceIni({ Mo2InstanceCandidate{ stale, false } }, {});
  Require(portableOnly.ini == stale && portableOnly.source == "portable", "no global instance means portable");

  const auto globalOnly = ChooseMo2InstanceIni({}, global);
  Require(globalOnly.ini == global && !globalOnly.ambiguous, "a lone global instance is not ambiguous");

  Require(ChooseMo2InstanceIni({}, {}).ini.empty(), "nothing found means no ini");

  // portable.txt next to the scanned ini is detected and recorded.
  const auto root = FreshRoot(L"skydiag_plugin_scanner_instance_");
  WriteText(root / L"ModOrganizer.ini", "[General]\nselected_profile=@ByteArray(Default)\n");
  WriteText(root / L"portable.txt", "");
  WriteText(root / L"profiles" / L"Default" / L"modlist.txt", "+Mod\n");
  WriteText(root / L"profiles" / L"Default" / L"plugins.txt", "");
  const auto result = ScanPlugins(root, { L"usvfs_x64.dll" }, nullptr);
  const auto json = nlohmann::json::parse(SerializePluginScanResult(result));
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  Require(result.mo2.instance_source == "portable_locked" && !result.mo2.instance_ambiguous,
    "a locked portable install is recorded as such");
  Require(json.at("mo2").at("instance_source") == "portable_locked", "the instance source is serialized");
}

// path::u8string() throws on an unpaired UTF-16 surrogate and u8path() on
// bytes that are not UTF-8; on MSVC either one cost the whole MO2 capture.
void TestOddPathsDoNotThrow()
{
  bool threw = false;
  std::filesystem::path resolved;
  try {
    resolved = skydiag::helper::ResolveMo2Directory("D:\\caf\xE9\xFF", L"C:\\MO2", L"mods");
  } catch (...) {
    threw = true;
  }
  Require(!threw && !resolved.empty(), "a configured directory that is not UTF-8 must not throw");

  const auto root = FreshRoot(L"skydiag_plugin_scanner_surrogate_\xD800_");
  WriteText(root / L"ModOrganizer.ini", "[General]\nselected_profile=@ByteArray(Default)\n");
  WriteText(root / L"portable.txt", "");
  WriteText(root / L"profiles" / L"Default" / L"plugins.txt", "*Mod.esp\n");
  WriteText(root / L"profiles" / L"Default" / L"modlist.txt", "+Mod\n");
  WriteText(root / L"Data" / L"Mod.esp", MakeTes4(0, { "Skyrim.esm" }));
  PluginScanResult result{};
  std::string json;
  threw = false;
  try {
    result = ScanPlugins(root, { L"usvfs_x64.dll" }, nullptr);
    json = SerializePluginScanResult(result);
  } catch (...) {
    threw = true;
  }
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  Require(!threw, "an unpaired surrogate in the MO2 folder must not throw out of the scan");
  Require(result.mo2.status == "captured", "the MO2 state is still captured");
  Require(result.mo2.ini_path.find("\xEF\xBF\xBD") != std::string::npos, "the odd character becomes U+FFFD");
  Require(!nlohmann::json::parse(json, nullptr, false).is_discarded(), "the scan JSON stays valid");
  Require(Contains(ActiveNames(result), "Mod.esp"), "plugins under that folder are still scanned");
}

}  // namespace

int main()
{
  try {
    TestMastersBeyondFourKilobytesAreRead();
    TestEslExtensionIsLightWithoutTheFlag();
    TestMo2InstanceChoiceFollowsMo2();
    TestOddPathsDoNotThrow();
    TestCreationClubListParsing();
    TestImplicitPluginsAreActiveUnderMo2();
    TestSkseLogIsMatchedByImageBase();
    TestAnsiPluginNamesDoNotThrow();
    TestMo2StateIsCapturedWithConfiguredDirectories();
    std::puts("plugin scanner runtime tests passed");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
