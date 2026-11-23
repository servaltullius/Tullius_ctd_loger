#include <Windows.h>

#include <filesystem>
#include <string>
#include <string_view>

#include "HelperLog.h"
#include "HelperMainInternal.h"
#include "HelperRuntimeTestUtils.h"
#include "RetentionWorker.h"
#include "SkyrimDiagHelper/DumpWriter.h"
#include "SkyrimDiagHelper/Retention.h"
#include "SkyrimDiagLongPath.h"

namespace {

using skydiag::PathForIo;
using skydiag::helper::internal::AppendLogLine;
using skydiag::helper::internal::ClearSessionDumpTokensForTests;
using skydiag::helper::internal::HelperSessionLogPath;
using skydiag::helper::internal::KeepSessionHelperLogBesideDumps;
using skydiag::helper::internal::RecordSessionDump;
using skydiag::helper::internal::SetHelperSessionLogMarker;
using skydiag::helper::internal::StartHelperLogSession;
using skydiag::tests::runtime::AssertContains;
using skydiag::tests::runtime::MakeTempDir;
using skydiag::tests::runtime::ReadAllTextUtf8;
using skydiag::tests::runtime::Require;
using skydiag::tests::runtime::WriteAllTextUtf8;

constexpr auto kMarker = L"Attached to pid=101 at 2026-11-23 12:00:01";
constexpr std::string_view kMarkerPrefix = "Attached to pid=101 at 2026-11-23 12:00:01";
constexpr std::string_view kHeader = "Attached to pid=101 at 2026-11-23 12:00:01, output=Logs\r\n";
constexpr std::string_view kSummaryPrefix = "Kept this session's helper log beside ";
constexpr std::string_view kMissingLine = "Could not find this session's helper log";
constexpr std::string_view kTruncationLine = "[Helper log truncated to its last 2 MiB]\r\n";
constexpr std::size_t kTailBytes = 2u * 1024u * 1024u;

std::filesystem::path WriteDumpFixture(const std::filesystem::path& outBase, std::wstring_view filename)
{
  const auto dump = outBase / filename;
  WriteAllTextUtf8(PathForIo(dump), "dump bytes");
  return dump;
}

std::filesystem::path DumpLogCopy(const std::filesystem::path& dump)
{
  return dump.parent_path() / (dump.stem().native() + L"_HelperLog.txt");
}

void RemoveTestDir(const std::filesystem::path& root)
{
  std::error_code ec;
  skydiag::RemoveAllForIo(skydiag::ExtendedLengthPath(root), ec);
  Require(!ec, "Could not remove test files");
}

void TestCopiesRecordedDumpVariantsAtShutdown()
{
  ClearSessionDumpTokensForTests();
  const auto root = MakeTempDir(L"slog");
  auto outBase = root;
  while (outBase.native().size() < 270) {
    outBase /= L"a_long_output_folder_segment";
  }
  Require(skydiag::PlainPath(outBase) == outBase, "The long output folder must be passed as a plain path");
  std::error_code ec;
  Require(skydiag::CreateDirectoriesForIo(outBase, ec) && !ec, "Could not create the long output folder");

  const auto crash = WriteDumpFixture(outBase, L"SkyrimDiag_Crash_20261123_120001_123.dmp");
  const auto released = WriteDumpFixture(outBase, L"SkyrimDiag_Released_20261123_120001_123.dmp");
  const auto richer = WriteDumpFixture(outBase, L"SkyrimDiag_Crash_20261123_120001_123_Richer.dmp");
  const auto foreign = WriteDumpFixture(outBase, L"SkyrimDiag_Crash_20261123_120002_123.dmp");
  const auto untimed = WriteDumpFixture(outBase, L"SkyrimDiag_Crash_untimed.dmp");
  const auto otherExt = WriteDumpFixture(outBase, L"SkyrimDiag_Hang_20261123_120001_123.etl");
  RecordSessionDump(crash.native());
  RecordSessionDump(richer.native()); // Records the same incident token again.
  RecordSessionDump(untimed.native());

  const auto manifest = outBase / L"SkyrimDiag_Incident_Crash_20261123_120001_123.json";
  constexpr std::string_view manifestBytes = R"({"artifacts":{"dump":"SkyrimDiag_Crash_20261123_120001_123.dmp","helper_log":"SkyrimDiagHelper.log"}})";
  WriteAllTextUtf8(PathForIo(manifest), manifestBytes);
  WriteAllTextUtf8(PathForIo(DumpLogCopy(crash)), "stale copy");

  SetHelperSessionLogMarker(kMarker);
  skydiag::helper::internal::QueueSessionStartLogLine(L"Output folder fallback warning");
  StartHelperLogSession(outBase);
  const auto currentLog = outBase / L"SkyrimDiagHelper.log";
  skydiag::helper::internal::HelperLoopState state{};
  state.crashCaptured.releasedDumpPath = released.native();
  skydiag::helper::internal::ShutdownLoopState(skydiag::tests::runtime::MakeTestConfig(), {}, outBase, &state);
  skydiag::helper::internal::ShutdownRetentionWorker();
  const auto sessionBytes = ReadAllTextUtf8(PathForIo(currentLog));
  Require(sessionBytes.starts_with(kMarkerPrefix), "A queued startup warning must follow the session marker");
  AssertContains(sessionBytes, "Output folder fallback warning", "A queued startup warning must still be logged");
  AssertContains(sessionBytes, "the helper stopped before the game's exit settled it",
                 "The session log must contain ShutdownLoopState's last decision");

  KeepSessionHelperLogBesideDumps(outBase);

  const auto current = ReadAllTextUtf8(PathForIo(currentLog));
  Require(current.starts_with(sessionBytes + std::string(kSummaryPrefix) + "3 dump(s): "),
          "The summary must follow the session bytes and count each copy once");
  for (const auto& dump : {crash, released, richer}) {
    const auto copy = DumpLogCopy(dump);
    Require(ReadAllTextUtf8(PathForIo(copy)) == sessionBytes, "A matching dump must have the exact session bytes");
    AssertContains(current.substr(sessionBytes.size()), copy.filename().string(), "The summary must name the copy");
  }
  for (const auto& file : {foreign, untimed, otherExt}) {
    Require(!skydiag::IsRegularFileForIo(DumpLogCopy(file)), "An unmatched file must have no copy");
  }
  Require(ReadAllTextUtf8(PathForIo(manifest)) == manifestBytes, "The manifest must stay byte-for-byte identical");

  skydiag::helper::PruneDumpFiles(outBase, "SkyrimDiag_Crash_", 1, false, true, false);
  Require(!skydiag::IsRegularFileForIo(crash), "Retention must remove the older crash dump");
  Require(!skydiag::IsRegularFileForIo(DumpLogCopy(crash)), "Retention must remove the older dump's helper log copy");
  Require(skydiag::IsRegularFileForIo(DumpLogCopy(released)), "Retention must keep the surviving dump's log copy");
  RemoveTestDir(root);
}

void TestDumpWriterRecordsOnlySuccessfulWrites()
{
  ClearSessionDumpTokensForTests();
  const auto outBase = MakeTempDir(L"dump");
  const auto dump = outBase / L"SkyrimDiag_Manual_20261123_120001_123.dmp";
  const auto failed = outBase / L"SkyrimDiag_Manual_20261123_120002_123.dmp";
  using namespace skydiag::helper;
  const auto profile = ResolveDumpProfile(DumpMode::kMini, CaptureKind::CrashRecapture);
  auto child = skydiag::tests::runtime::LaunchSleepingChildProcess();
  std::wstring err;
  const bool written = WriteDumpWithStreams(child.pi.hProcess, child.pi.dwProcessId, dump.native(), nullptr, 0,
                                            {}, {}, false, profile, false, &err);
  skydiag::tests::runtime::TerminateChildProcess(&child);
  Require(written, "Failed to write the fixture minidump");
  Require(!WriteDumpWithStreams(nullptr, 0, failed.native(), nullptr, 0, {}, {}, false, profile, false, &err),
          "The second write must fail");
  WriteAllTextUtf8(failed, "unrelated dump bytes");
  SetHelperSessionLogMarker(kMarker);
  StartHelperLogSession(outBase);

  KeepSessionHelperLogBesideDumps(outBase);

  Require(skydiag::IsRegularFileForIo(DumpLogCopy(dump)), "The dump writer must register the successful capture");
  Require(!skydiag::IsRegularFileForIo(DumpLogCopy(failed)), "The dump writer must ignore a failed capture");
  RemoveTestDir(outBase);
}

void TestMatchesEarlierSessionLog()
{
  ClearSessionDumpTokensForTests();
  const auto outBase = MakeTempDir(L"prev");
  const auto dump = WriteDumpFixture(outBase, L"SkyrimDiag_Crash_20261123_120001_123.dmp");
  RecordSessionDump(dump.native());
  SetHelperSessionLogMarker(kMarker);
  WriteAllTextUtf8(outBase / L"SkyrimDiagHelper.log", "Attached to pid=101 at 2026-11-23 12:00:02, output=Logs\r\nnewer\r\n");
  WriteAllTextUtf8(HelperSessionLogPath(outBase, 1), "Attached to pid=102 at 2026-11-23 12:00:01, output=Logs\r\nother pid\r\n");
  const auto previousBytes = std::string(kHeader) + "this session's decision\r\n";
  WriteAllTextUtf8(HelperSessionLogPath(outBase, 2), previousBytes);
  WriteAllTextUtf8(HelperSessionLogPath(outBase, 3), std::string(kHeader) + "later match in search order\r\n");

  KeepSessionHelperLogBesideDumps(outBase);

  Require(ReadAllTextUtf8(DumpLogCopy(dump)) == previousBytes, "The first matching previous log must supply the copy");
  RemoveTestDir(outBase);
}

void TestMissingSessionLogIsReported()
{
  ClearSessionDumpTokensForTests();
  const auto outBase = MakeTempDir(L"miss");
  const auto dump = WriteDumpFixture(outBase, L"SkyrimDiag_Crash_20261123_120001_123.dmp");
  RecordSessionDump(dump.native());
  SetHelperSessionLogMarker(kMarker);
  skydiag::helper::internal::SetHelperLogRotation(8u * 1024u * 1024u, 3u);
  const auto currentLog = outBase / L"SkyrimDiagHelper.log";
  // The current log is another session's, so the archive with this session's
  // marker is not the start of the current log's session.
  WriteAllTextUtf8(currentLog, "Attached to pid=102 at 2026-11-23 12:00:05, output=Logs\r\n" + std::string(kHeader));
  WriteAllTextUtf8(HelperSessionLogPath(outBase, 1), "no match\r\n");
  WriteAllTextUtf8(outBase / L"SkyrimDiagHelper.log.1", kHeader);

  KeepSessionHelperLogBesideDumps(outBase);
  skydiag::helper::internal::SetHelperLogRotation(0u, 0u);

  Require(!skydiag::IsRegularFileForIo(DumpLogCopy(dump)), "The marker must occur at the beginning of a session log");
  const auto log = ReadAllTextUtf8(currentLog);
  AssertContains(log, kMissingLine, "A missing session log must be reported");
  Require(log.find(kSummaryPrefix) == std::string::npos, "Zero copies must produce no success summary");
  RemoveTestDir(outBase);
}

void TestSizeRotatedSessionLogIsJoined()
{
  ClearSessionDumpTokensForTests();
  const auto outBase = MakeTempDir(L"rot");
  const auto dump = WriteDumpFixture(outBase, L"SkyrimDiag_Crash_20261123_120001_123.dmp");
  RecordSessionDump(dump.native());
  SetHelperSessionLogMarker(kMarker);
  skydiag::helper::internal::SetHelperLogRotation(8u * 1024u * 1024u, 3u);
  // The session outgrew MaxHelperLogBytes twice: its start is in .log.2.
  WriteAllTextUtf8(outBase / L"SkyrimDiagHelper.log.3", "Attached to pid=99 at 2026-11-22 09:00:00, output=Logs\r\nold\r\n");
  WriteAllTextUtf8(outBase / L"SkyrimDiagHelper.log.2", std::string(kHeader) + "early decision\r\n");
  WriteAllTextUtf8(outBase / L"SkyrimDiagHelper.log.1", "middle decision\r\n");
  WriteAllTextUtf8(outBase / L"SkyrimDiagHelper.log", "last decision\r\n");

  KeepSessionHelperLogBesideDumps(outBase);
  skydiag::helper::internal::SetHelperLogRotation(0u, 0u);

  Require(ReadAllTextUtf8(DumpLogCopy(dump)) ==
            std::string(kHeader) + "early decision\r\nmiddle decision\r\nlast decision\r\n",
          "A size-rotated session must be copied from its first archive through the current log");
  RemoveTestDir(outBase);
}

void TestLargeLogStartsAtLineBoundary()
{
  ClearSessionDumpTokensForTests();
  const auto outBase = MakeTempDir(L"tail");
  const auto dump = WriteDumpFixture(outBase, L"SkyrimDiag_Crash_20261123_120001_123.dmp");
  RecordSessionDump(dump.native());
  SetHelperSessionLogMarker(kMarker);
  const std::string lastLine = "last decision\r\n";
  const auto tail = "\x82\xac" + std::string(kTailBytes - 4 - lastLine.size(), 'x') + "\r\n" + lastLine;
  WriteAllTextUtf8(outBase / L"SkyrimDiagHelper.log", std::string(kHeader) + "partial line \xe2" + tail);

  KeepSessionHelperLogBesideDumps(outBase);

  Require(ReadAllTextUtf8(DumpLogCopy(dump)) == std::string(kTruncationLine) + lastLine,
          "The copy must discard the partial UTF-8 line at the 2 MiB cutoff");
  RemoveTestDir(outBase);
}

void TestCopyFailureIsReported()
{
  ClearSessionDumpTokensForTests();
  const auto outBase = MakeTempDir(L"fail");
  const auto blocked = WriteDumpFixture(outBase, L"SkyrimDiag_Crash_20261123_120001_123.dmp");
  const auto writable = WriteDumpFixture(outBase, L"SkyrimDiag_Released_20261123_120001_123.dmp");
  RecordSessionDump(blocked.native());
  SetHelperSessionLogMarker(kMarker);
  StartHelperLogSession(outBase);
  std::error_code ec;
  Require(std::filesystem::create_directory(DumpLogCopy(blocked), ec) && !ec, "Could not block the copy destination");

  KeepSessionHelperLogBesideDumps(outBase);

  Require(skydiag::IsRegularFileForIo(DumpLogCopy(writable)), "A failed copy must not prevent other copies");
  const auto log = ReadAllTextUtf8(outBase / L"SkyrimDiagHelper.log");
  AssertContains(log, "Could not keep the helper log beside: " + blocked.filename().string(),
                 "The failure line must name the affected dump");
  AssertContains(log, std::string(kSummaryPrefix) + "1 dump(s): ", "The summary must count successful writes only");
  RemoveTestDir(outBase);
}

}  // namespace

int main(int argc, char** argv)
{
  if (argc == 2 && std::string_view(argv[1]) == "--defaults") {
    Require(skydiag::helper::LoadConfig(nullptr).maxHelperSessionLogs == 20u, "The INI default must keep 20 sessions");
    Require(skydiag::helper::HelperConfig{}.maxHelperSessionLogs == 20u, "HelperConfig must default to 20 sessions");
    Require(skydiag::helper::internal::kDefaultHelperSessionLogsToKeep == 20u, "The logger must default to 20 sessions");
    return 0;
  }
  SKYDIAG_RUN_TEST(TestCopiesRecordedDumpVariantsAtShutdown);
  SKYDIAG_RUN_TEST(TestDumpWriterRecordsOnlySuccessfulWrites);
  SKYDIAG_RUN_TEST(TestMatchesEarlierSessionLog);
  SKYDIAG_RUN_TEST(TestMissingSessionLogIsReported);
  SKYDIAG_RUN_TEST(TestSizeRotatedSessionLogIsJoined);
  SKYDIAG_RUN_TEST(TestLargeLogStartsAtLineBoundary);
  SKYDIAG_RUN_TEST(TestCopyFailureIsReported);
  return 0;
}
