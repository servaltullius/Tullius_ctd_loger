#include "Mo2Index.h"

#include <cassert>
#include <string>
#include <vector>

using skydiag::dump_tool::InferMo2ModNameFromPath;
using skydiag::dump_tool::TryInferMo2BaseDirFromModulePaths;

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

  return 0;
}
