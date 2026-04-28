#include "Mo2Index.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
  WriteTextFile(
    base / "profiles" / "Default" / "modlist.txt",
    "+Active Low\n"
    "-Disabled Source\n"
    "+Active Winner\n");

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

int main()
{
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

  return 0;
}
