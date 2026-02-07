#include "SkyrimDiagHelper/Retention.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

using skydiag::helper::RetentionLimits;
using skydiag::helper::RotateLogFileIfNeeded;
using skydiag::helper::ApplyRetentionToOutputDir;

static void WriteFile(const std::filesystem::path& path, std::size_t bytes = 1)
{
  std::filesystem::create_directories(path.parent_path());
  std::ofstream f(path, std::ios::binary);
  assert(f && "Failed to open file for writing");
  const std::string blob(bytes, 'x');
  f.write(blob.data(), static_cast<std::streamsize>(blob.size()));
}

static bool Exists(const std::filesystem::path& path)
{
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

static std::filesystem::path MakeTempDir()
{
  const auto base = std::filesystem::temp_directory_path();
  const auto dir = base / ("skydiag_retention_test_" + std::to_string(std::rand()));
  std::filesystem::create_directories(dir);
  return dir;
}

static void Test_PrunesCrashDumpsAndArtifacts()
{
  const auto dir = MakeTempDir();

  // 3 crash dumps with matching analyzer artifacts; keep newest 2.
  const char* ts0 = "20260101_000000";
  const char* ts1 = "20260101_000001";
  const char* ts2 = "20260101_000002";

  const auto stem0 = std::string("SkyrimDiag_Crash_") + ts0;
  const auto stem1 = std::string("SkyrimDiag_Crash_") + ts1;
  const auto stem2 = std::string("SkyrimDiag_Crash_") + ts2;

  WriteFile(dir / (stem0 + ".dmp"));
  WriteFile(dir / (stem0 + "_SkyrimDiagSummary.json"));
  WriteFile(dir / (stem0 + "_SkyrimDiagReport.txt"));
  WriteFile(dir / (stem0 + "_SkyrimDiagBlackbox.jsonl"));

  WriteFile(dir / (stem1 + ".dmp"));
  WriteFile(dir / (stem1 + "_SkyrimDiagSummary.json"));

  WriteFile(dir / (stem2 + ".dmp"));
  WriteFile(dir / (stem2 + "_SkyrimDiagReport.txt"));

  RetentionLimits limits{};
  limits.maxCrashDumps = 2;
  limits.maxHangDumps = 0;
  limits.maxManualDumps = 0;
  limits.maxEtwTraces = 0;
  ApplyRetentionToOutputDir(dir, limits);

  assert(!Exists(dir / (stem0 + ".dmp")));
  assert(!Exists(dir / (stem0 + "_SkyrimDiagSummary.json")));
  assert(!Exists(dir / (stem0 + "_SkyrimDiagReport.txt")));
  assert(!Exists(dir / (stem0 + "_SkyrimDiagBlackbox.jsonl")));

  assert(Exists(dir / (stem1 + ".dmp")));
  assert(Exists(dir / (stem2 + ".dmp")));
}

static void Test_PrunesHangWctAndEtlWithDump()
{
  const auto dir = MakeTempDir();

  // 2 hang dumps with WCT+ETL; keep newest 1.
  const char* ts0 = "20260101_010000";
  const char* ts1 = "20260101_010001";

  const auto dump0 = std::string("SkyrimDiag_Hang_") + ts0;
  const auto dump1 = std::string("SkyrimDiag_Hang_") + ts1;

  WriteFile(dir / (dump0 + ".dmp"));
  WriteFile(dir / (dump0 + "_SkyrimDiagSummary.json"));
  WriteFile(dir / (std::string("SkyrimDiag_WCT_") + ts0 + ".json"));
  WriteFile(dir / (dump0 + ".etl"));

  WriteFile(dir / (dump1 + ".dmp"));
  WriteFile(dir / (std::string("SkyrimDiag_WCT_") + ts1 + ".json"));

  RetentionLimits limits{};
  limits.maxCrashDumps = 0;
  limits.maxHangDumps = 1;
  limits.maxManualDumps = 0;
  limits.maxEtwTraces = 1;
  ApplyRetentionToOutputDir(dir, limits);

  assert(!Exists(dir / (dump0 + ".dmp")));
  assert(!Exists(dir / (dump0 + "_SkyrimDiagSummary.json")));
  assert(!Exists(dir / (std::string("SkyrimDiag_WCT_") + ts0 + ".json")));
  assert(!Exists(dir / (dump0 + ".etl")));

  assert(Exists(dir / (dump1 + ".dmp")));
  assert(Exists(dir / (std::string("SkyrimDiag_WCT_") + ts1 + ".json")));
}

static void Test_RotatesHelperLog()
{
  const auto dir = MakeTempDir();
  const auto log = dir / "SkyrimDiagHelper.log";

  WriteFile(log, 64);
  RotateLogFileIfNeeded(log, /*maxBytes=*/16, /*maxFiles=*/2);

  // Current file should be rotated away. New log file is not created until next append.
  assert(!Exists(log));
  assert(Exists(dir / "SkyrimDiagHelper.log.1"));
}

int main()
{
  Test_PrunesCrashDumpsAndArtifacts();
  Test_PrunesHangWctAndEtlWithDump();
  Test_RotatesHelperLog();
  return 0;
}

