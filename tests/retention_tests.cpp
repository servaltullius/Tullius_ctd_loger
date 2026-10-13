#include "SkyrimDiagHelper/Retention.h"

#include <cassert>
#include <chrono>
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

static void Test_KeepsCrashManifestWhenSiblingDumpSharesTimestamp()
{
  const auto dir = MakeTempDir();
  const char* ts = "20260101_020000";

  const auto miniStem = std::string("SkyrimDiag_Crash_") + ts;
  const auto fullStem = miniStem + "_Full";
  const auto manifest = std::string("SkyrimDiag_Incident_Crash_") + ts + ".json";

  WriteFile(dir / (miniStem + ".dmp"));
  WriteFile(dir / (fullStem + ".dmp"));
  WriteFile(dir / manifest);

  RetentionLimits limits{};
  limits.maxCrashDumps = 1;
  limits.maxHangDumps = 0;
  limits.maxManualDumps = 0;
  limits.maxEtwTraces = 0;
  ApplyRetentionToOutputDir(dir, limits);

  const bool miniExists = Exists(dir / (miniStem + ".dmp"));
  const bool fullExists = Exists(dir / (fullStem + ".dmp"));
  assert(miniExists != fullExists);
  assert(Exists(dir / manifest));
}

static void Test_PrunesEtwTracesAcrossCrashAndHangPrefixes()
{
  const auto dir = MakeTempDir();

  const auto hang0 = std::string("SkyrimDiag_Hang_20260101_030000.etl");
  const auto crash1 = std::string("SkyrimDiag_Crash_20260101_030001.etl");
  const auto hang2 = std::string("SkyrimDiag_Hang_20260101_030002.etl");

  WriteFile(dir / hang0);
  WriteFile(dir / crash1);
  WriteFile(dir / hang2);

  RetentionLimits limits{};
  limits.maxCrashDumps = 0;
  limits.maxHangDumps = 0;
  limits.maxManualDumps = 0;
  limits.maxEtwTraces = 2;
  ApplyRetentionToOutputDir(dir, limits);

  assert(!Exists(dir / hang0));
  assert(Exists(dir / crash1));
  assert(Exists(dir / hang2));
}

static void Test_PrunesCrashManifestWithPrecisionTimestamp()
{
  const auto dir = MakeTempDir();

  const char* ts0 = "20260101_040000_123";
  const char* ts1 = "20260101_040000_456";

  const auto stem0 = std::string("SkyrimDiag_Crash_") + ts0;
  const auto stem1 = std::string("SkyrimDiag_Crash_") + ts1;
  const auto manifest0 = std::string("SkyrimDiag_Incident_Crash_") + ts0 + ".json";
  const auto manifest1 = std::string("SkyrimDiag_Incident_Crash_") + ts1 + ".json";

  WriteFile(dir / (stem0 + ".dmp"));
  WriteFile(dir / (stem1 + ".dmp"));
  WriteFile(dir / manifest0);
  WriteFile(dir / manifest1);

  RetentionLimits limits{};
  limits.maxCrashDumps = 1;
  limits.maxHangDumps = 0;
  limits.maxManualDumps = 0;
  limits.maxEtwTraces = 0;
  ApplyRetentionToOutputDir(dir, limits);

  assert(!Exists(dir / (stem0 + ".dmp")));
  assert(!Exists(dir / manifest0));
  assert(Exists(dir / (stem1 + ".dmp")));
  assert(Exists(dir / manifest1));
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

// Names the ANSI code page cannot represent (here a snowman): retention now
// compares UTF-8 names, which never throw, instead of path::string(), which
// substitutes '?' on most systems but can throw when the code page is UTF-8.
// Pruning must go on around such a file.
static void Test_PrunesAroundNamesOutsideTheAnsiCodePage()
{
  const auto dir = MakeTempDir();
  const auto odd = dir / std::filesystem::u8path("SkyrimDiag_Crash_20260101_000009_\xE2\x98\x83.txt");
  const auto oddDump = dir / std::filesystem::u8path("notes \xE2\x98\x83.dmp");
  WriteFile(odd);
  WriteFile(oddDump);

  const auto stem0 = std::string("SkyrimDiag_Crash_20260101_000000");
  const auto stem1 = std::string("SkyrimDiag_Crash_20260101_000001");
  WriteFile(dir / (stem0 + ".dmp"));
  WriteFile(dir / (stem0 + "_SkyrimDiagReport.txt"));
  WriteFile(dir / (stem1 + ".dmp"));

  RetentionLimits limits{};
  limits.maxCrashDumps = 1;
  limits.maxHangDumps = 0;
  limits.maxManualDumps = 0;
  limits.maxEtwTraces = 0;
  ApplyRetentionToOutputDir(dir, limits);

  assert(!Exists(dir / (stem0 + ".dmp")));
  assert(!Exists(dir / (stem0 + "_SkyrimDiagReport.txt")));
  assert(Exists(dir / (stem1 + ".dmp")));
  assert(Exists(oddDump));
}

#ifdef _WIN32
// An NTFS name can hold an unpaired UTF-16 surrogate, on which
// path::u8string() throws. One such file must not stop every sweep.
static void Test_PrunesAroundUnpairedSurrogateNames()
{
  const auto dir = MakeTempDir();
  const auto odd = dir / std::wstring(L"notes \xD800.dmp");
  WriteFile(odd);
  assert(skydiag::helper::PathPartUtf8(odd.filename()) == "notes \xEF\xBF\xBD.dmp");
  assert(skydiag::helper::PathPartUtf8(std::filesystem::path(L"\xD83D\xDE00")) == "\xF0\x9F\x98\x80");

  const auto stem0 = std::string("SkyrimDiag_Crash_20260101_000000");
  const auto stem1 = std::string("SkyrimDiag_Crash_20260101_000001");
  WriteFile(dir / (stem0 + ".dmp"));
  WriteFile(dir / (stem1 + ".dmp"));

  RetentionLimits limits{};
  limits.maxCrashDumps = 1;
  limits.maxHangDumps = 0;
  limits.maxManualDumps = 0;
  limits.maxEtwTraces = 0;
  ApplyRetentionToOutputDir(dir, limits);

  assert(!Exists(dir / (stem0 + ".dmp")));
  assert(Exists(dir / (stem1 + ".dmp")));
  assert(Exists(odd));
}
#endif

static void SetAge(const std::filesystem::path& path, std::chrono::hours age)
{
  std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() - age);
}

// Analysis copies and locks of dumps that are gone are removed; those of
// dumps still there, recent ones, and folders that are not ours stay.
static void Test_PrunesAnalysisCachesWithoutTheirDump()
{
  const auto dir = MakeTempDir();
  const std::string stem = "SkyrimDiag_Crash_20260101_000000_000";
  WriteFile(dir / (stem + ".dmp"), 10);

  const auto live = dir / ".skydiag-analysis" / "aaaa" / "000000000000000a.01dd000000000001";
  const auto orphan = dir / ".skydiag-analysis" / "bbbb" / "0000000000000014.01dd000000000001";
  const auto recent = dir / ".skydiag-analysis" / "cccc" / "000000000000001e.01dd000000000001";
  const auto foreign = dir / ".skydiag-analysis" / "dddd" / "not-a-key";
  for (const auto& generation : { live, orphan, recent, foreign }) {
    WriteFile(generation / "Summary.json");
  }
  for (const auto& generation : { live, orphan, foreign }) {
    SetAge(generation, std::chrono::hours(48));
  }
  const auto liveLock = dir / ".skydiag-locks" / (stem + ".lock");
  const auto orphanLock = dir / ".skydiag-locks" / "SkyrimDiag_Crash_20250101_000000_000.lock";
  const auto triage = dir / ".skydiag-triage" / "bbbb" / "0000000000000014.01dd000000000001.json";
  WriteFile(liveLock);
  WriteFile(orphanLock);
  WriteFile(triage);
  SetAge(liveLock, std::chrono::hours(48));
  SetAge(orphanLock, std::chrono::hours(48));

  ApplyRetentionToOutputDir(dir, RetentionLimits{});

  assert(Exists(live / "Summary.json"));
  assert(!Exists(orphan));
  assert(!Exists(dir / ".skydiag-analysis" / "bbbb"));
  assert(Exists(recent / "Summary.json"));
  assert(Exists(foreign / "Summary.json"));
  assert(Exists(liveLock));
  assert(!Exists(orphanLock));
  assert(Exists(triage));
  assert(Exists(dir / (stem + ".dmp")));

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

// A dump pruned by the count limit takes its analysis copy with it once
// that copy is old enough.
static void Test_PrunedDumpLeavesNoAnalysisCopy()
{
  const auto dir = MakeTempDir();
  WriteFile(dir / "SkyrimDiag_Crash_20260101_000000_000.dmp", 10);
  WriteFile(dir / "SkyrimDiag_Crash_20260102_000000_000.dmp", 11);
  const auto oldCopy = dir / ".skydiag-analysis" / "aaaa" / "000000000000000a.01dd000000000001";
  const auto newCopy = dir / ".skydiag-analysis" / "bbbb" / "000000000000000b.01dd000000000001";
  WriteFile(oldCopy / "Summary.json");
  WriteFile(newCopy / "Summary.json");
  SetAge(oldCopy, std::chrono::hours(48));
  SetAge(newCopy, std::chrono::hours(48));

  RetentionLimits limits{};
  limits.maxCrashDumps = 1;
  ApplyRetentionToOutputDir(dir, limits);

  assert(!Exists(dir / "SkyrimDiag_Crash_20260101_000000_000.dmp"));
  assert(!Exists(oldCopy));
  assert(Exists(newCopy / "Summary.json"));

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

int main()
{
  Test_PrunesCrashDumpsAndArtifacts();
  Test_PrunesHangWctAndEtlWithDump();
  Test_KeepsCrashManifestWhenSiblingDumpSharesTimestamp();
  Test_PrunesEtwTracesAcrossCrashAndHangPrefixes();
  Test_PrunesCrashManifestWithPrecisionTimestamp();
  Test_RotatesHelperLog();
  Test_PrunesAroundNamesOutsideTheAnsiCodePage();
  Test_PrunesAnalysisCachesWithoutTheirDump();
  Test_PrunedDumpLeavesNoAnalysisCopy();
#ifdef _WIN32
  Test_PrunesAroundUnpairedSurrogateNames();
#endif
  return 0;
}
