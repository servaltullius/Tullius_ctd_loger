// Review M11 (2026-10-19): when OutputDir could not be created or written,
// the helper kept using it, and every dump, report and log line of the
// session was lost without a trace. MakeOutputBase now falls back to the
// first writable cfg.outputDirFallbacks entry and the helper log says so.

#include <Windows.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "SkyrimDiagHelper/Config.h"

#include "HelperCommon.h"
#include "HelperLog.h"

using skydiag::helper::HelperConfig;
using skydiag::helper::internal::MakeOutputBase;
using skydiag::helper::internal::PrepareWritableOutputDir;
using skydiag::helper::internal::StartHelperLogSession;

namespace {

void Require(bool condition, const char* message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::string ReadAll(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void TestUnusableOutputDirFallsBack(const std::filesystem::path& root)
{
  // A regular file where a folder is needed: create_directories fails.
  const auto blocker = root / L"blocker";
  {
    std::ofstream out(blocker, std::ios::binary);
    out << "x";
  }
  HelperConfig cfg{};
  cfg.outputDir = (blocker / L"logs").wstring();
  const auto fallback = root / L"fallback";
  cfg.outputDirFallbacks = { (root / L"also_blocked" / L"x").wstring(), fallback.wstring() };
  {
    std::ofstream out(root / L"also_blocked", std::ios::binary);
    out << "x";
  }

  std::wstring err;
  Require(!PrepareWritableOutputDir(blocker / L"logs", &err) && !err.empty(), "a folder under a file is not usable");

  const auto outBase = MakeOutputBase(cfg);
  Require(outBase == fallback, "the first writable fallback is used");
  std::error_code ec;
  Require(std::filesystem::is_directory(fallback, ec), "the fallback folder is created");

  // The warning becomes the first line of the new helper log session, after
  // the previous session's log is set aside.
  {
    std::ofstream old(fallback / L"SkyrimDiagHelper.log", std::ios::binary);
    old << "previous session\r\n";
  }
  StartHelperLogSession(outBase);
  const auto log = ReadAll(fallback / L"SkyrimDiagHelper.log");
  Require(log.find("cannot be used") != std::string::npos, "the fallback is logged in the new session");
  Require(log.find("blocker") != std::string::npos, "the log names the unusable OutputDir");
  Require(log.find("previous session") == std::string::npos, "the warning is not rotated away");

  // Logged once, not again by a later session start.
  StartHelperLogSession(outBase);
  Require(ReadAll(fallback / L"SkyrimDiagHelper.log").find("cannot be used") == std::string::npos,
    "a queued warning is written once");

  // Probe files never stay behind.
  for (const auto& entry : std::filesystem::directory_iterator(fallback)) {
    Require(entry.path().filename().wstring().find(L"write_probe") == std::wstring::npos, "probe files are removed");
  }
}

void TestWritableOutputDirIsKept(const std::filesystem::path& root)
{
  HelperConfig cfg{};
  cfg.outputDir = (root / L"configured").wstring();
  cfg.outputDirFallbacks = { (root / L"unused").wstring() };
  Require(MakeOutputBase(cfg) == root / L"configured", "a writable OutputDir is used as configured");
  std::error_code ec;
  Require(!std::filesystem::exists(root / L"unused", ec) && !ec, "no fallback is created when it is not needed");
}

void TestNothingWritableKeepsTheConfiguredPath(const std::filesystem::path& root)
{
  HelperConfig cfg{};
  cfg.outputDir = (root / L"blocker" / L"a").wstring();
  cfg.outputDirFallbacks = { (root / L"blocker" / L"b").wstring() };
  Require(MakeOutputBase(cfg) == root / L"blocker" / L"a", "with no usable folder the configured one is returned");
}

}  // namespace

int main()
{
  const auto root = std::filesystem::temp_directory_path() /
    (L"skydiag_output_dir_fallback_" + std::to_wstring(GetCurrentProcessId()));
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  int rc = 0;
  try {
    TestUnusableOutputDirFallsBack(root);
    TestWritableOutputDirIsKept(root);
    TestNothingWritableKeepsTheConfiguredPath(root);
    std::puts("helper output dir fallback tests passed");
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "FAILED: %s\n", ex.what());
    rc = 1;
  }
  std::filesystem::remove_all(root, ec);
  return rc;
}
