#include "DumpToolCliArgs.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using skydiag::dump_tool::cli::DumpToolCliArgs;
using skydiag::dump_tool::cli::ParseDumpToolCliArgs;

static void Test_ParsesDumpPathAndOutDir()
{
  DumpToolCliArgs a{};
  std::wstring err;
  const std::vector<std::wstring_view> argv = {
    L"SkyrimDiagDumpToolCli.exe",
    L"C:\\dumps\\SkyrimDiag_Crash_123.dmp",
    L"--out-dir",
    L"C:\\out",
  };
  const bool ok = ParseDumpToolCliArgs(argv, &a, &err);
  assert(ok);
  assert(err.empty());
  assert(a.dump_path == L"C:\\dumps\\SkyrimDiag_Crash_123.dmp");
  assert(a.out_dir == L"C:\\out");
}

static void Test_ParsesOnlineSymbolsFlags()
{
  {
    DumpToolCliArgs a{};
    std::wstring err;
    const std::vector<std::wstring_view> argv = {
      L"SkyrimDiagDumpToolCli.exe",
      L"C:\\dumps\\a.dmp",
      L"--allow-online-symbols",
    };
    const bool ok = ParseDumpToolCliArgs(argv, &a, &err);
    assert(ok);
    assert(a.allow_online_symbols.has_value());
    assert(a.allow_online_symbols.value());
  }

  {
    DumpToolCliArgs a{};
    std::wstring err;
    const std::vector<std::wstring_view> argv = {
      L"SkyrimDiagDumpToolCli.exe",
      L"C:\\dumps\\a.dmp",
      L"--no-online-symbols",
      L"--headless",
    };
    const bool ok = ParseDumpToolCliArgs(argv, &a, &err);
    assert(ok);
    assert(a.allow_online_symbols.has_value());
    assert(!a.allow_online_symbols.value());
  }
}

static void Test_RejectsMissingDumpPath()
{
  DumpToolCliArgs a{};
  std::wstring err;
  const std::vector<std::wstring_view> argv = {
    L"SkyrimDiagDumpToolCli.exe",
    L"--out-dir",
    L"C:\\out",
  };
  const bool ok = ParseDumpToolCliArgs(argv, &a, &err);
  assert(!ok);
  assert(!err.empty());
}

static void Test_RejectsUnknownFlag()
{
  DumpToolCliArgs a{};
  std::wstring err;
  const std::vector<std::wstring_view> argv = {
    L"SkyrimDiagDumpToolCli.exe",
    L"C:\\dumps\\a.dmp",
    L"--not-a-real-flag",
  };
  const bool ok = ParseDumpToolCliArgs(argv, &a, &err);
  assert(!ok);
  assert(!err.empty());
}

static std::string ReadAllText(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::in | std::ios::binary);
  assert(in && "Failed to open source file");
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static void Test_CliMainPrintsMachineReadableOutputPaths()
{
  const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto src = ReadAllText(repoRoot / "dump_tool" / "src" / "DumpToolCliMain.cpp");
  assert(src.find("ANALYSIS_OK=1") != std::string::npos);
  assert(src.find("\"SUMMARY_JSON\"") != std::string::npos);
  assert(src.find("\"REPORT_TXT\"") != std::string::npos);
  assert(src.find("PrintUtf8KeyValue") != std::string::npos);
  assert(src.find("WideToUtf8") != std::string::npos);
  assert(src.find("std::cout") != std::string::npos);
}

int main()
{
  Test_ParsesDumpPathAndOutDir();
  Test_ParsesOnlineSymbolsFlags();
  Test_RejectsMissingDumpPath();
  Test_RejectsUnknownFlag();
  Test_CliMainPrintsMachineReadableOutputPaths();
  return 0;
}
