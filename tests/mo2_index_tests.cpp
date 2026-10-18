#include "Mo2Index.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using skydiag::dump_tool::FindMo2ProvidersForDataPath;
using skydiag::dump_tool::InferMo2ModNameFromPath;
using skydiag::dump_tool::TryBuildMo2IndexFromModulePaths;
using skydiag::dump_tool::TryInferMo2BaseDirFromModulePaths;

namespace {

void WriteTextFile(const std::filesystem::path& path, const std::string& text)
{
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  assert(out.is_open());
  out << text;
}

void TouchFile(const std::filesystem::path& path)
{
  WriteTextFile(path, "x");
}

std::string U8(const std::filesystem::path& path)
{
  const auto s = path.u8string();
  return std::string(s.begin(), s.end());
}

}  // namespace

// ── InferMo2ModNameFromPath ────────────────────────

static void Test_InferModName_StandardPath()
{
  const auto result = InferMo2ModNameFromPath(L"C:\\MO2\\mods\\CBBE\\SKSE\\Plugins\\cbpc.dll");
  assert(result == L"CBBE");
}

static void Test_InferModName_CaseInsensitive()
{
  const auto result = InferMo2ModNameFromPath(L"D:\\Modding\\MODS\\SomeHDT\\meshes\\test.nif");
  assert(result == L"SomeHDT");
}

static void Test_InferModName_NoModsDir()
{
  const auto result = InferMo2ModNameFromPath(L"C:\\SkyrimSE\\SkyrimSE.exe");
  assert(result.empty());
}

static void Test_InferModName_EmptyInput()
{
  const auto result = InferMo2ModNameFromPath(L"");
  assert(result.empty());
}

static void Test_InferModName_TrailingMods()
{
  // Path ends with "\\mods\\" but no mod name after
  const auto result = InferMo2ModNameFromPath(L"C:\\MO2\\mods\\");
  assert(result.empty());
}

static void Test_InferModName_NoTrailingBackslash()
{
  // "\\mods\\ModName" with no trailing backslash
  const auto result = InferMo2ModNameFromPath(L"C:\\MO2\\mods\\OnlyMod");
  assert(result.empty());
}

// ── TryInferMo2BaseDirFromModulePaths ──────────────

static void Test_InferBaseDir_SingleMatch()
{
  const std::vector<std::wstring> paths = {
    L"C:\\SkyrimSE\\SkyrimSE.exe",
    L"G:\\Modding\\MO2\\mods\\CBBE\\SKSE\\Plugins\\cbpc.dll",
    L"C:\\Windows\\System32\\ntdll.dll",
  };
  const auto result = TryInferMo2BaseDirFromModulePaths(paths);
  assert(result.has_value());
  assert(result->wstring() == L"G:\\Modding\\MO2");
}

static void Test_InferBaseDir_NoMatch()
{
  const std::vector<std::wstring> paths = {
    L"C:\\SkyrimSE\\SkyrimSE.exe",
    L"C:\\Windows\\System32\\ntdll.dll",
  };
  const auto result = TryInferMo2BaseDirFromModulePaths(paths);
  assert(!result.has_value());
}

static void Test_InferBaseDir_EmptyPaths()
{
  const std::vector<std::wstring> paths = {};
  const auto result = TryInferMo2BaseDirFromModulePaths(paths);
  assert(!result.has_value());
}

static void Test_InferBaseDir_EmptyStringsIgnored()
{
  const std::vector<std::wstring> paths = { L"", L"", L"" };
  const auto result = TryInferMo2BaseDirFromModulePaths(paths);
  assert(!result.has_value());
}

static void Test_ActiveProfileProviderScan_DoesNotIncludeDisabledMods()
{
  const auto base = std::filesystem::temp_directory_path() / "skydiag_mo2_index_active_profile_test";
  std::error_code ec;
  std::filesystem::remove_all(base, ec);

  WriteTextFile(base / "ModOrganizer.ini", "selected_profile=@ByteArray(Default)\n");
  // MO2 writes modlist.txt highest priority first (Profile::doWriteModlist):
  // the mod at the bottom of its left pane, which wins conflicts, is line 1.
  WriteTextFile(
    base / "profiles" / "Default" / "modlist.txt",
    "+Active Winner\n"
    "-Disabled Source\n"
    "+Active Low\n");

  const auto rel = std::filesystem::path("meshes") / "footprints" / "test.nif";
  TouchFile(base / "mods" / "Active Low" / rel);
  TouchFile(base / "mods" / "Active Winner" / rel);
  TouchFile(base / "mods" / "Disabled Source" / rel);

  const std::wstring modulePath = base.wstring() + L"\\mods\\Active Winner\\SKSE\\Plugins\\winner.dll";
  const auto idx = TryBuildMo2IndexFromModulePaths({modulePath});
  assert(idx.has_value());

  const auto providers = FindMo2ProvidersForDataPath(*idx, L"meshes/footprints/test.nif", 8);
  assert(providers.size() == 2);
  assert(providers[0] == L"Active Winner");
  assert(providers[1] == L"Active Low");
  for (const auto& provider : providers) {
    assert(provider != L"Disabled Source");
  }

  std::filesystem::remove_all(base, ec);
}

// Audit F05: a dump kept from one profile and analyzed after switching to
// another described the new profile's mods. The helper now captures the
// profile and its mod list while the game runs; that capture wins over disk.
static void Test_CrashTimeCaptureWinsOverCurrentProfile()
{
  using skydiag::dump_tool::BuildMo2IndexFromCapture;
  using skydiag::dump_tool::Mo2Index;
  using skydiag::dump_tool::TryParseCapturedMo2State;

  const auto base = std::filesystem::temp_directory_path() / "skydiag_mo2_index_capture_test";
  std::error_code ec;
  std::filesystem::remove_all(base, ec);

  // On disk now: MO2 was switched to "Other", whose only mod also has the file.
  WriteTextFile(base / "ModOrganizer.ini", "[General]\nselected_profile=@ByteArray(Other)\n");
  WriteTextFile(base / "profiles" / "Other" / "modlist.txt", "+Other Mod\n");
  const auto rel = std::filesystem::path("meshes") / "capture" / "test.nif";
  TouchFile(base / "mods" / "Other Mod" / rel);
  TouchFile(base / "mods" / "Crash Low" / rel);
  TouchFile(base / "mods" / "Crash Winner" / rel);

  nlohmann::json scan = {
    { "plugins_source", "mo2_profile" },
    { "mo2",
      {
        { "status", "captured" },
        { "base_dir", U8(base) },
        { "mods_dir", U8((base / "mods")) },
        { "profiles_dir", U8((base / "profiles")) },
        { "overwrite_dir", U8((base / "overwrite")) },
        { "profile", "Crash" },
        // "Removed Since" was enabled at the crash and deleted afterwards.
        { "enabled_mods", { "Crash Winner", "Removed Since", "Crash Low" } },
      } },
  };
  const auto captured = TryParseCapturedMo2State(scan.dump());
  assert(captured.has_value());
  assert(captured->profileName == L"Crash");
  const auto idx = BuildMo2IndexFromCapture(*captured);
  assert(idx.source == Mo2Index::Source::kCrashTime);
  assert(idx.profileName == L"Crash");

  const auto providers = FindMo2ProvidersForDataPath(idx, L"meshes/capture/test.nif", 8);
  assert(providers.size() == 2);
  assert(providers[0] == L"Crash Winner");
  assert(providers[1] == L"Crash Low");

  // The disk inference still describes the profile MO2 is on now.
  const auto now = TryBuildMo2IndexFromModulePaths({ base.wstring() + L"\\mods\\Other Mod\\x.dll" });
  assert(now.has_value() && now->source == Mo2Index::Source::kAnalysisTime && now->profileName == L"Other");

  // Anything but a completed capture leaves the analyzer on its disk fallback.
  scan["mo2"]["status"] = "no_modlist";
  assert(!TryParseCapturedMo2State(scan.dump()).has_value());
  scan.erase("mo2");
  assert(!TryParseCapturedMo2State(scan.dump()).has_value());
  assert(!TryParseCapturedMo2State("not json").has_value());
  assert(!TryParseCapturedMo2State("").has_value());

  std::filesystem::remove_all(base, ec);
}

// Folder names from a real MO2 setup that installed Nexus archives without
// metadata, and names that must stay as they are.
static void Test_CleanMo2ModFolderName()
{
  using skydiag::dump_tool::CleanMo2ModFolderName;
  assert(CleanMo2ModFolderName(L"SmoothCam 41252 1.8.1 2026") == L"SmoothCam");
  assert(CleanMo2ModFolderName(L"CrashLogger 59818 1.25.0 2026-08-21T09-21Z yIjvuh041") == L"CrashLogger");
  assert(CleanMo2ModFolderName(L"Texture Downscaler 187049 2.6.0 2026-09-28T13-25Z 6XybdasXK") == L"Texture Downscaler");
  assert(CleanMo2ModFolderName(L"Address Library All in One (1.7.104.0) v13 32444 13 2026-08-27T15-29Z Ae46W7Fw2") ==
         L"Address Library All in One (1.7.104.0) v13");
  assert(CleanMo2ModFolderName(L"LoadAccel 0.3.1 - SE 1.5.97 and AE 1.6.1170 193880 0.3.1 2026") ==
         L"LoadAccel 0.3.1 - SE 1.5.97 and AE 1.6.1170");
  assert(CleanMo2ModFolderName(L"Skyrim Performance Solution (SPS) - BETA 194164 1.0.54 2026") ==
         L"Skyrim Performance Solution (SPS) - BETA");

  assert(CleanMo2ModFolderName(L"Tullius Hair 3 XML Pack 1004 fix") == L"Tullius Hair 3 XML Pack 1004 fix");
  assert(CleanMo2ModFolderName(L"Heels Sound - 2025 Edition") == L"Heels Sound - 2025 Edition");
  assert(CleanMo2ModFolderName(L"Engine Fixes - Main File") == L"Engine Fixes - Main File");
  assert(CleanMo2ModFolderName(L"SkyrimUpscalerAIOBuild16-Hotfix1") == L"SkyrimUpscalerAIOBuild16-Hotfix1");
  assert(CleanMo2ModFolderName(L"Mod 2026") == L"Mod 2026");
  assert(CleanMo2ModFolderName(L"") == L"");

  assert(InferMo2ModNameFromPath(L"G:\\TAKEALOOK\\mods\\SmoothCam 41252 1.8.1 2026\\SKSE\\Plugins\\SmoothCam.dll") ==
         L"SmoothCam");
}

// The folder cache rules out mods without a folder once and gives the same
// providers, in the same order, as asking the disk for each file.
void Test_FolderCacheGivesTheSameProviders()
{
  const auto base = std::filesystem::temp_directory_path() / "skydiag_mo2_folder_cache";
  std::error_code ec;
  std::filesystem::remove_all(base, ec);
  TouchFile(base / "mods" / "Winner" / "meshes" / "actors" / "a.nif");
  TouchFile(base / "mods" / "Winner" / "meshes" / "actors" / "b.nif");
  TouchFile(base / "mods" / "TexturesOnly" / "textures" / "a.dds");
  TouchFile(base / "mods" / "Loser" / "meshes" / "actors" / "a.nif");
  TouchFile(base / "mods" / "Loser" / "meshes" / "clutter" / "c.nif");
  std::filesystem::create_directories(base / "mods" / "EspOnly");

  skydiag::dump_tool::Mo2Index idx{};
  idx.overwriteDir = base / "overwrite";
  for (const char* name : { "Winner", "TexturesOnly", "EspOnly", "Loser" }) {
    idx.modDirs.push_back(base / "mods" / name);
    idx.modNames.push_back(std::filesystem::path(name).wstring());
  }

  skydiag::dump_tool::Mo2FolderCache cache;
  for (const wchar_t* path : { L"meshes\\actors\\a.nif", L"Meshes/Actors/B.NIF", L"meshes\\clutter\\c.nif", L"meshes\\none.nif" }) {
    assert(FindMo2ProvidersForDataPath(idx, path, 8, &cache) == FindMo2ProvidersForDataPath(idx, path, 8));
  }
  // Forward slashes: the Linux CI build has no backslash separator.
  const auto a = FindMo2ProvidersForDataPath(idx, L"meshes/actors/a.nif", 8, &cache);
  assert(a.size() == 2 && a[0] == L"Winner" && a[1] == L"Loser");
  const auto& meshes = cache.presence.at(L"meshes");
  assert(meshes.size() == 4 && meshes[1] == 2u && meshes[2] == 2u);  // ruled out once
  assert(cache.presence.at(L"meshes\\actors")[3] == 1u);
  std::filesystem::remove_all(base, ec);
}


int main()
{
  Test_FolderCacheGivesTheSameProviders();
  Test_CleanMo2ModFolderName();
  Test_InferModName_StandardPath();
  Test_InferModName_CaseInsensitive();
  Test_InferModName_NoModsDir();
  Test_InferModName_EmptyInput();
  Test_InferModName_TrailingMods();
  Test_InferModName_NoTrailingBackslash();

  Test_InferBaseDir_SingleMatch();
  Test_InferBaseDir_NoMatch();
  Test_InferBaseDir_EmptyPaths();
  Test_InferBaseDir_EmptyStringsIgnored();
  Test_ActiveProfileProviderScan_DoesNotIncludeDisabledMods();
  Test_CrashTimeCaptureWinsOverCurrentProfile();

  return 0;
}
