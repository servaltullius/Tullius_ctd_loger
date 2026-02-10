#include "SkyrimDiagHelper/DumpToolResolve.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

using skydiag::helper::ResolveDumpToolHeadlessExe;

static std::filesystem::path MakeTempDir()
{
  const auto base = std::filesystem::temp_directory_path();
  const auto dir = base / "skydiag_dump_tool_headless_resolver_tests";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  assert(std::filesystem::exists(dir));
  return dir;
}

static void TouchFile(const std::filesystem::path& p)
{
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream f(p.string(), std::ios::binary);
  f << "x";
  f.close();
  assert(std::filesystem::exists(p));
}

static void Test_PrefersCliNextToHelper()
{
  const auto dir = MakeTempDir();
  const auto cli = dir / "SkyrimDiagDumpToolCli.exe";
  const auto winui = dir / "SkyrimDiagWinUI" / "SkyrimDiagDumpToolWinUI.exe";
  TouchFile(cli);
  TouchFile(winui);

  const auto resolved = ResolveDumpToolHeadlessExe(dir, winui, /*overrideExe=*/{});
  assert(resolved == cli);
}

static void Test_FallsBackToWinUiWhenCliMissing()
{
  const auto dir = MakeTempDir();
  const auto winui = dir / "SkyrimDiagWinUI" / "SkyrimDiagDumpToolWinUI.exe";
  TouchFile(winui);

  const auto resolved = ResolveDumpToolHeadlessExe(dir, winui, /*overrideExe=*/{});
  assert(resolved == winui);
}

static void Test_FallsBackToOverrideWhenBothMissing()
{
  const auto dir = MakeTempDir();
  const auto overrideExe = dir / "SomeOtherDir" / "SomeDumpTool.exe";
  TouchFile(overrideExe);

  const auto resolved = ResolveDumpToolHeadlessExe(dir, /*winuiExe=*/{}, overrideExe);
  assert(resolved == overrideExe);
}

static void Test_ReturnsEmptyWhenNothingExists()
{
  const auto dir = MakeTempDir();
  const auto resolved = ResolveDumpToolHeadlessExe(dir, /*winuiExe=*/{}, /*overrideExe=*/{});
  assert(resolved.empty());
}

int main()
{
  Test_PrefersCliNextToHelper();
  Test_FallsBackToWinUiWhenCliMissing();
  Test_FallsBackToOverrideWhenBothMissing();
  Test_ReturnsEmptyWhenNothingExists();
  return 0;
}

