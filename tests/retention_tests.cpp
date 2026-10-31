#include "SkyrimDiagHelper/Retention.h"
#include "SkyrimDiagHelper/CrashRecapturePolicy.h"
#include "SkyrimDiagLongPath.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#include <winioctl.h>
#endif

using skydiag::helper::RetentionLimits;
using skydiag::helper::RotateLogFileIfNeeded;
using skydiag::helper::ApplyRetentionToOutputDir;
using skydiag::helper::PruneDumpsOverTotalBytes;
using skydiag::ExtendedLengthPath;

static void WriteFile(const std::filesystem::path& path, std::size_t bytes = 1)
{
  std::error_code ec;
  skydiag::CreateDirectoriesForIo(path.parent_path(), ec);
  assert(!ec && "Failed to create the file's folder");
  std::ofstream f(path, std::ios::binary);
  assert(f && "Failed to open file for writing");
  const std::string blob(bytes, 'x');
  f.write(blob.data(), static_cast<std::streamsize>(blob.size()));
}

// A failed check stops the test: read as "not there", it passed every "the
// file is gone" check without checking anything.
static bool Exists(const std::filesystem::path& path)
{
  std::error_code ec;
  const bool exists = std::filesystem::exists(path, ec);
  assert(!ec && "exists() failed");
  return exists;
}

// Unique per run (an unseeded rand() gave every run the same names, so a
// previous run's leftovers were reused) and removed when the tests end.
static std::vector<std::filesystem::path>& TempDirs()
{
  static std::vector<std::filesystem::path> dirs;
  return dirs;
}

static std::filesystem::path MakeTempDir()
{
  static unsigned counter = 0;
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto dir = std::filesystem::temp_directory_path() /
    ("skydiag_retention_test_" + std::to_string(stamp) + "_" + std::to_string(++counter));
  std::filesystem::create_directories(dir);
  TempDirs().push_back(dir);
  return dir;
}

static void RemoveTempDirs()
{
  std::error_code ec;
  for (const auto& dir : TempDirs()) {
    std::filesystem::remove_all(dir, ec);
  }
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

// A recapture dump (SkyrimDiag_Crash_<ts>_Richer.dmp) writes
// SkyrimDiag_Incident_CrashRecapture_<ts>.json; it goes with its dump.
static void Test_PrunesCrashRecaptureManifestWithItsDump()
{
  const auto dir = MakeTempDir();
  const char* oldTs = "20260101_050000_100";
  const char* newTs = "20260101_060000_100";
  const auto oldDump = std::string("SkyrimDiag_Crash_") + oldTs + "_Richer.dmp";
  const auto newDump = std::string("SkyrimDiag_Crash_") + newTs + "_Richer.dmp";
  const auto oldManifest = std::string("SkyrimDiag_Incident_CrashRecapture_") + oldTs + ".json";
  const auto newManifest = std::string("SkyrimDiag_Incident_CrashRecapture_") + newTs + ".json";
  WriteFile(dir / oldDump);
  WriteFile(dir / newDump);
  WriteFile(dir / oldManifest);
  WriteFile(dir / newManifest);

  RetentionLimits limits{};
  limits.maxCrashDumps = 1;
  limits.maxHangDumps = 0;
  limits.maxManualDumps = 0;
  limits.maxEtwTraces = 0;
  ApplyRetentionToOutputDir(dir, limits);

  assert(!Exists(dir / oldDump));
  assert(!Exists(dir / oldManifest));
  assert(Exists(dir / newDump));
  assert(Exists(dir / newManifest));
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

// The same under an output folder long enough that a copy's folders and the
// locks pass MAX_PATH, where Windows reaches them only through
// extended-length paths, and MO2's virtual file system only through a handle
// (SkyrimDiagLongPath.h); hence IsDirectoryForIo, not Exists, below.
static void Test_PrunesAnalysisCachesUnderALongOutputDir()
{
  const auto root = MakeTempDir();
  auto dir = root;
  while (dir.native().size() < 220u) {
    dir /= "a_long_output_folder_segment";
  }
  const auto cache = dir / ".skydiag-analysis";
  const auto orphan = cache / std::string(64, 'b') / "0000000000000014.01dd000000000001";
  const auto recent = cache / std::string(64, 'c') / "0000000000000015.01dd000000000001";
  assert(orphan.native().size() >= 260u);
  WriteFile(ExtendedLengthPath(orphan / "Summary.json"));
  WriteFile(ExtendedLengthPath(recent / "Summary.json"));
  SetAge(ExtendedLengthPath(orphan), std::chrono::hours(48));
  const auto oldLock = dir / ".skydiag-locks" / "SkyrimDiag_Manual_20261031_111106_520.lock";
  const auto newLock = dir / ".skydiag-locks" / "SkyrimDiag_Manual_20261031_111107_521.lock";
  assert(oldLock.native().size() >= 260u);
  WriteFile(ExtendedLengthPath(oldLock), 0);
  WriteFile(ExtendedLengthPath(newLock), 0);
  SetAge(ExtendedLengthPath(oldLock), std::chrono::hours(48));

  ApplyRetentionToOutputDir(dir, RetentionLimits{});

  assert(!skydiag::IsDirectoryForIo(orphan));
  assert(!skydiag::IsDirectoryForIo(cache / std::string(64, 'b')));
  assert(skydiag::IsRegularFileForIo(recent / "Summary.json"));
  assert(!skydiag::IsRegularFileForIo(oldLock));
  assert(skydiag::IsRegularFileForIo(newLock));

  std::error_code ec;
  skydiag::RemoveAllForIo(ExtendedLengthPath(root), ec);
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

// What a stopped helper leaves behind is bounded: older held dumps of
// released crashes, a day-old one, an unfinished full-size rewrite and a
// day-old rearm marker go; the newest held dump, a rewrite that may still be
// in progress and a fresh marker stay. Crash dumps are not touched.
static void Test_PrunesHelperLeftovers()
{
  const auto dir = MakeTempDir();
  const auto crash = dir / "SkyrimDiag_Crash_20260101_000000_000.dmp";
  const auto olderReleased = dir / "SkyrimDiag_Released_20260101_000001_000.dmp";
  const auto newestReleased = dir / "SkyrimDiag_Released_20260101_000002_000.dmp";
  const auto oldPartial = dir / "SkyrimDiag_Crash_20260101_000003_000.dmp.full.tmp";
  const auto freshPartial = dir / "SkyrimDiag_Crash_20260101_000004_000.dmp.full.tmp";
  const auto oldMarker = dir / "SkyrimDiag_CrashRearm_1234_5678_2.marker";
  const auto freshMarker = dir / "SkyrimDiag_CrashRearm_4321_8765_4.marker";
  for (const auto& file : { crash, olderReleased, newestReleased, oldPartial, freshPartial, oldMarker, freshMarker }) {
    WriteFile(file);
  }
  SetAge(oldPartial, std::chrono::hours(1));
  SetAge(oldMarker, std::chrono::hours(48));
  SetAge(crash, std::chrono::hours(48));

  ApplyRetentionToOutputDir(dir, RetentionLimits{});

  assert(Exists(crash));
  assert(!Exists(olderReleased));
  assert(Exists(newestReleased));
  assert(!Exists(oldPartial));
  assert(Exists(freshPartial));
  assert(!Exists(oldMarker));
  assert(Exists(freshMarker));

  // A day old, the newest held dump goes too.
  SetAge(newestReleased, std::chrono::hours(48));
  ApplyRetentionToOutputDir(dir, RetentionLimits{});
  assert(!Exists(newestReleased));
  assert(Exists(crash));

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

// A stats file or crash history that did not parse is kept as it was, one
// more copy per failure: only the newest of each stays, and only for a day.
// The files themselves stay.
static void Test_PrunesCorruptBucketStatsCopies()
{
  const auto dir = MakeTempDir();
  const auto stats = dir / "SkyrimDiag_CrashBucketStats.json";
  const auto oldest = dir / "SkyrimDiag_CrashBucketStats.json.corrupt.20260101_000000_000";
  const auto older = dir / "SkyrimDiag_CrashBucketStats.json.corrupt.20260102_000000_000";
  const auto newest = dir / "SkyrimDiag_CrashBucketStats.json.corrupt.20260103_000000_000";
  const auto history = dir / "crash_history.json";
  const auto olderHistory = dir / "crash_history.json.corrupt.20260101T000000Z";
  const auto newestHistory = dir / "crash_history.json.corrupt.20260102T000000Z";
  for (const auto& file : { stats, oldest, older, newest, history, olderHistory, newestHistory }) {
    WriteFile(file);
  }
  // Write times say nothing here: the name orders them.
  SetAge(newest, std::chrono::hours(2));
  SetAge(stats, std::chrono::hours(48));
  SetAge(history, std::chrono::hours(48));

  ApplyRetentionToOutputDir(dir, RetentionLimits{});

  assert(Exists(stats));
  assert(!Exists(oldest));
  assert(!Exists(older));
  assert(Exists(newest));
  assert(Exists(history));
  assert(!Exists(olderHistory));
  assert(Exists(newestHistory));

  SetAge(newest, std::chrono::hours(48));
  SetAge(newestHistory, std::chrono::hours(48));
  ApplyRetentionToOutputDir(dir, RetentionLimits{});
  assert(!Exists(newest));
  assert(!Exists(newestHistory));
  assert(Exists(stats));
  assert(Exists(history));

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

// Past the total budget the oldest dumps of any kind go with their files,
// newest first: once one does not fit, every older one goes too, even one
// small enough to fit. The newest dump of each kind stays.
static void Test_PrunesOldestDumpsOverTheTotalBudget()
{
  const auto dir = MakeTempDir();
  const auto crash1 = dir / "SkyrimDiag_Crash_20260101_000001_000.dmp";
  const auto hang2 = std::string("SkyrimDiag_Hang_20260101_000002_000");
  const auto crash3 = dir / "SkyrimDiag_Crash_20260101_000003_000.dmp";
  const auto manual4 = dir / "SkyrimDiag_Manual_20260101_000004_000.dmp";
  const auto hang5 = dir / "SkyrimDiag_Hang_20260101_000005_000.dmp";
  const auto crash6 = dir / "SkyrimDiag_Crash_20260101_000006_000.dmp";
  WriteFile(crash1, 5);
  WriteFile(dir / (hang2 + ".dmp"), 30);
  WriteFile(dir / (hang2 + "_SkyrimDiagSummary.json"));
  WriteFile(dir / "SkyrimDiag_WCT_20260101_000002_000.json");
  WriteFile(dir / "SkyrimDiag_Incident_Hang_20260101_000002_000.json");
  WriteFile(crash3, 40);
  WriteFile(manual4, 20);
  WriteFile(dir / "SkyrimDiag_WCT_Manual_20260101_000004_000.json");
  WriteFile(hang5, 30);
  WriteFile(crash6, 40);

  // Newest of each kind: 40 + 30 + 20 = 90. crash3 fits (130), hang2 does
  // not (160), and crash1 goes with it although 135 would fit.
  PruneDumpsOverTotalBytes(dir, 150);

  assert(Exists(crash6));
  assert(Exists(hang5));
  assert(Exists(manual4));
  assert(Exists(dir / "SkyrimDiag_WCT_Manual_20260101_000004_000.json"));
  assert(Exists(crash3));
  assert(!Exists(dir / (hang2 + ".dmp")));
  assert(!Exists(dir / (hang2 + "_SkyrimDiagSummary.json")));
  assert(!Exists(dir / "SkyrimDiag_WCT_20260101_000002_000.json"));
  assert(!Exists(dir / "SkyrimDiag_Incident_Hang_20260101_000002_000.json"));
  assert(!Exists(crash1));
}

// The same under an output folder whose dumps pass MAX_PATH, given in its
// extended form as the helper does. Under MO2 the dump sizes came back as
// errors (file_size by path), every dump counted 0 bytes and the budget
// never pruned (the *ForIo calls of SkyrimDiagLongPath.h).
static void Test_TotalBudgetUnderALongOutputDir()
{
  const auto root = MakeTempDir();
  auto dir = root;
  while (dir.native().size() < 230u) {
    dir /= "a_long_output_folder_segment";
  }
  const auto outBase = ExtendedLengthPath(dir);
  const auto crash1 = dir / "SkyrimDiag_Crash_20260101_000001_000.dmp";
  const auto crash2 = dir / "SkyrimDiag_Crash_20260101_000002_000.dmp";
  const auto crash3 = dir / "SkyrimDiag_Crash_20260101_000003_000.dmp";
  assert(crash1.native().size() >= 260u);
  WriteFile(ExtendedLengthPath(crash1), 40);
  WriteFile(ExtendedLengthPath(crash2), 40);
  WriteFile(ExtendedLengthPath(crash3), 40);

  // 40 + 40 fit, the oldest makes 120.
  PruneDumpsOverTotalBytes(outBase, 100);

  assert(skydiag::IsRegularFileForIo(crash3));
  assert(skydiag::IsRegularFileForIo(crash2));
  assert(!skydiag::IsRegularFileForIo(crash1));

  std::error_code ec;
  skydiag::RemoveAllForIo(ExtendedLengthPath(root), ec);
}

// A budget smaller than the newest dumps keeps the newest of each kind
// written within the last day and only those; 0 is unlimited.
static void Test_TotalBudgetKeepsTheRecentNewestOfEachKind()
{
  const auto dir = MakeTempDir();
  const auto oldCrash = dir / "SkyrimDiag_Crash_20260101_000000_000.dmp";
  const auto crash = dir / "SkyrimDiag_Crash_20260101_000001_000.dmp";
  const auto hang = dir / "SkyrimDiag_Hang_20260101_000002_000.dmp";
  const auto manual = dir / "SkyrimDiag_Manual_20260101_000003_000.dmp";
  WriteFile(oldCrash, 1);
  WriteFile(crash, 100);
  WriteFile(hang, 100);
  WriteFile(manual, 100);

  PruneDumpsOverTotalBytes(dir, 0);
  assert(Exists(oldCrash));

  PruneDumpsOverTotalBytes(dir, 50);
  assert(!Exists(oldCrash));
  assert(Exists(crash));
  assert(Exists(hang));
  assert(Exists(manual));
}

// A newest-of-its-kind older than a day does not hold budget: an old freeze
// dump goes, while the latest capture stays however old and large it is.
static void Test_TotalBudgetDropsAnOldNewestOfItsKind()
{
  const auto dir = MakeTempDir();
  const auto oldHang = dir / "SkyrimDiag_Hang_20260101_000000_000.dmp";
  const auto latestCrash = dir / "SkyrimDiag_Crash_20260101_000001_000.dmp";
  WriteFile(oldHang, 100);
  WriteFile(latestCrash, 200);
  SetAge(oldHang, std::chrono::hours(48));
  SetAge(latestCrash, std::chrono::hours(48));

  PruneDumpsOverTotalBytes(dir, 150);

  assert(!Exists(oldHang));
  assert(Exists(latestCrash));
}

// A crash recapture (SkyrimDiag_Crash_<ts>_Full.dmp, a full-memory dump)
// shares its crash's timestamp and stays with it however large it is; the
// sweep right after the recapture must not delete it. The names come from
// CrashRecaptureTimestamp, as the helper builds them.
static void Test_TotalBudgetKeepsARecaptureWithItsCrash()
{
  const auto dir = MakeTempDir();
  const auto older = dir / "SkyrimDiag_Crash_20260101_000000_000.dmp";
  const auto crash = dir / "SkyrimDiag_Crash_20260101_000001_000.dmp";
  const auto ts = skydiag::helper::CrashRecaptureTimestamp(crash);
  assert(ts == "20260101_000001_000");
  const auto recapture = dir / ("SkyrimDiag_Crash_" + ts + "_Full.dmp");
  const auto manifest = dir / ("SkyrimDiag_Incident_CrashRecapture_" + ts + ".json");
  WriteFile(older, 10);
  WriteFile(crash, 10);
  WriteFile(recapture, 500);
  WriteFile(manifest);

  PruneDumpsOverTotalBytes(dir, 50);

  assert(!Exists(older));
  assert(Exists(crash));
  assert(Exists(recapture));
  assert(Exists(manifest));
}

#ifdef _WIN32
// A dump that cannot be deleted (open without FILE_SHARE_DELETE, as during an
// analysis) keeps its reports; the next sweep removes both.
static void Test_TotalBudgetKeepsReportsOfADumpInUse()
{
  const auto dir = MakeTempDir();
  const auto stem = std::string("SkyrimDiag_Crash_20260101_000000_000");
  const auto older = dir / (stem + ".dmp");
  const auto summary = dir / (stem + "_SkyrimDiagSummary.json");
  WriteFile(older, 100);
  WriteFile(summary);
  WriteFile(dir / "SkyrimDiag_Crash_20260101_000001_000.dmp", 100);

  const HANDLE open = CreateFileW(older.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  assert(open != INVALID_HANDLE_VALUE);
  PruneDumpsOverTotalBytes(dir, 150);
  assert(Exists(older));
  assert(Exists(summary));

  CloseHandle(open);
  PruneDumpsOverTotalBytes(dir, 150);
  assert(!Exists(older));
  assert(!Exists(summary));
}
#endif

// A file of the given size that takes no disk space, so that a budget in
// GiB can be reached. False where the file system cannot make one.
static bool WriteSparseFile(const std::filesystem::path& path, std::uint64_t bytes)
{
#ifdef _WIN32
  const HANDLE file =
    CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD returned = 0;
  FILE_END_OF_FILE_INFO end{};
  end.EndOfFile.QuadPart = static_cast<LONGLONG>(bytes);
  const bool ok = DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) &&
    SetFileInformationByHandle(file, FileEndOfFileInfo, &end, sizeof(end));
  CloseHandle(file);
  return ok;
#else
  WriteFile(path);
  std::error_code ec;
  std::filesystem::resize_file(path, bytes, ec);
  return !ec;
#endif
}

// The sweep applies the budget, in GiB: two dumps of just over 1 GiB under
// MaxTotalDumpGB=1 leave the newer one. The older one's trace goes with it
// and does not take a MaxEtwTraces place from a trace that is left.
static void Test_SweepAppliesTheTotalBudget()
{
  const auto dir = MakeTempDir();
  const auto older = dir / "SkyrimDiag_Crash_20260101_000002_000.dmp";
  const auto newer = dir / "SkyrimDiag_Crash_20260101_000003_000.dmp";
  if (!WriteSparseFile(older, (1ull << 30u) + 1u) || !WriteSparseFile(newer, (1ull << 30u) + 1u)) {
    std::puts("skipped Test_SweepAppliesTheTotalBudget: no sparse files in the temp folder");
    return;
  }
  // The oldest trace has no dump left; the budget deletes the one of
  // `older`. One place: the oldest trace must not lose it to a deleted one.
  const auto keptTrace = dir / "SkyrimDiag_Hang_20260101_000001_000.etl";
  WriteFile(keptTrace);
  WriteFile(dir / "SkyrimDiag_Crash_20260101_000002_000.etl");

  RetentionLimits limits{};
  limits.maxTotalDumpGB = 1;
  limits.maxEtwTraces = 1;
  ApplyRetentionToOutputDir(dir, limits);

  assert(!Exists(older));
  assert(!Exists(dir / "SkyrimDiag_Crash_20260101_000002_000.etl"));
  assert(Exists(newer));
  assert(Exists(keptTrace));
}

// The setting is in GiB: small dumps under a 1 GiB budget all stay.
static void Test_TotalBudgetIsInGibibytes()
{
  const auto dir = MakeTempDir();
  for (int i = 0; i < 4; ++i) {
    WriteFile(dir / ("SkyrimDiag_Crash_20260101_00000" + std::to_string(i) + "_000.dmp"), 10);
  }
  RetentionLimits limits{};
  limits.maxTotalDumpGB = 1;
  ApplyRetentionToOutputDir(dir, limits);
  for (int i = 0; i < 4; ++i) {
    assert(Exists(dir / ("SkyrimDiag_Crash_20260101_00000" + std::to_string(i) + "_000.dmp")));
  }
}

int main()
{
  Test_PrunesCrashDumpsAndArtifacts();
  Test_PrunesOldestDumpsOverTheTotalBudget();
  Test_TotalBudgetUnderALongOutputDir();
  Test_TotalBudgetKeepsTheRecentNewestOfEachKind();
  Test_TotalBudgetDropsAnOldNewestOfItsKind();
  Test_TotalBudgetKeepsARecaptureWithItsCrash();
#ifdef _WIN32
  Test_TotalBudgetKeepsReportsOfADumpInUse();
#endif
  Test_TotalBudgetIsInGibibytes();
  Test_SweepAppliesTheTotalBudget();
  Test_PrunesHelperLeftovers();
  Test_PrunesCorruptBucketStatsCopies();
  Test_PrunesHangWctAndEtlWithDump();
  Test_KeepsCrashManifestWhenSiblingDumpSharesTimestamp();
  Test_PrunesEtwTracesAcrossCrashAndHangPrefixes();
  Test_PrunesCrashManifestWithPrecisionTimestamp();
  Test_PrunesCrashRecaptureManifestWithItsDump();
  Test_RotatesHelperLog();
  Test_PrunesAroundNamesOutsideTheAnsiCodePage();
  Test_PrunesAnalysisCachesWithoutTheirDump();
  Test_PrunesAnalysisCachesUnderALongOutputDir();
  Test_PrunedDumpLeavesNoAnalysisCopy();
#ifdef _WIN32
  Test_PrunesAroundUnpairedSurrogateNames();
#endif
  RemoveTempDirs();
  return 0;
}
