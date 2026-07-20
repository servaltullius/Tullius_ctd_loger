#include "CrashLogger.h"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

using skydiag::dump_tool::TryFindCrashLoggerLogForDump;
using skydiag::dump_tool::CrashLoggerPairingMetadata;

constexpr const char* kCrashLogText =
  "CrashLoggerSSE v1.20.0\n"
  "CRASH TIME: 2097-07-13 12:00:00\n"
  "PROBABLE CALL STACK:\n"
  "  ExampleMod.dll+0x123\n";

constexpr const char* kThreadDumpText =
  "CrashLoggerSSE v1.20.0\n"
  "THREAD DUMP (Manual Trigger)\n"
  "TIME: 2097-07-13 12:00:00\n"
  "CALLSTACK:\n"
  "  ExampleMod.dll+0x123\n";

void Require(bool condition, const char* message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class TempDirectory final
{
public:
  explicit TempDirectory(std::string_view caseName)
  {
    const auto nonce = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
      ("skydiag-crashlogger-search-" + std::to_string(GetCurrentProcessId()) + "-" +
       std::to_string(nonce) + "-" + std::string(caseName));
    std::error_code ec;
    Require(std::filesystem::create_directories(path_, ec) && !ec,
            "failed to create CrashLogger integration-test directory");
  }

  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;

  ~TempDirectory()
  {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path& path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

std::filesystem::path WriteFile(
  const std::filesystem::path& directory,
  std::wstring_view filename,
  std::string_view contents)
{
  const auto path = directory / std::filesystem::path(filename);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  Require(static_cast<bool>(out), "failed to create CrashLogger integration-test file");
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  out.close();
  Require(static_cast<bool>(out), "failed to write CrashLogger integration-test file");
  return path;
}

void SetLastWriteTimeLocal(
  const std::filesystem::path& path,
  WORD year,
  WORD month,
  WORD day,
  WORD hour,
  WORD minute,
  WORD second)
{
  SYSTEMTIME local{};
  local.wYear = year;
  local.wMonth = month;
  local.wDay = day;
  local.wHour = hour;
  local.wMinute = minute;
  local.wSecond = second;

  SYSTEMTIME utc{};
  Require(TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) != FALSE,
          "failed to convert integration-test local time to UTC");

  FILETIME writeTime{};
  Require(SystemTimeToFileTime(&utc, &writeTime) != FALSE,
          "failed to convert integration-test UTC time to FILETIME");

  HANDLE file = CreateFileW(
    path.c_str(),
    FILE_WRITE_ATTRIBUTES,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    nullptr,
    OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  Require(file != INVALID_HANDLE_VALUE,
          "failed to open integration-test file for timestamp update");
  const BOOL updated = SetFileTime(file, nullptr, nullptr, &writeTime);
  CloseHandle(file);
  Require(updated != FALSE, "failed to update integration-test file timestamp");
}

std::optional<std::filesystem::path> Search(
  const std::filesystem::path& dumpPath,
  CrashLoggerPairingMetadata* metadata = nullptr)
{
  std::wstring error = L"not cleared";
  const auto found = TryFindCrashLoggerLogForDump(
    dumpPath,
    std::nullopt,
    nullptr,
    std::nullopt,
    &error,
    metadata);
  Require(error.empty(), "CrashLogger search unexpectedly returned an error");
  return found;
}

void RequireFoundFilename(
  const std::optional<std::filesystem::path>& found,
  std::wstring_view expectedFilename,
  const char* message)
{
  Require(found.has_value(), message);
  Require(found->filename() == std::filesystem::path(expectedFilename), message);
}

void TestTimestampDistanceWinsBeforeFilenameProvenance()
{
  TempDirectory temp("time-priority");
  const auto dump = WriteFile(temp.path(), L"Tullius_crash_20970713_120000.dmp", "dump");

  const auto renamed = WriteFile(temp.path(), L"renamed-diagnostic.log", kCrashLogText);
  SetLastWriteTimeLocal(renamed, 2097, 7, 13, 12, 0, 0);
  WriteFile(temp.path(), L"crash-2097-07-13-11-58-01.log", kCrashLogText);

  RequireFoundFilename(
    Search(dump),
    L"renamed-diagnostic.log",
    "an exact-time renamed log must beat a known crash log that is 119 seconds away");
}

void TestPairingWindowIncludesExactly120SecondsAndRejects121()
{
  TempDirectory temp("window-boundary");
  const auto dump = WriteFile(temp.path(), L"Tullius_crash_20970714_120000.dmp", "dump");
  const auto boundary =
    WriteFile(temp.path(), L"crash-2097-07-14-11-58-00.log", kCrashLogText);
  WriteFile(temp.path(), L"crash-2097-07-14-11-57-59.log", kCrashLogText);

  RequireFoundFilename(
    Search(dump),
    boundary.filename().wstring(),
    "a CrashLogger log exactly 120 seconds away must remain eligible");

  std::error_code ec;
  Require(std::filesystem::remove(boundary, ec) && !ec,
          "failed to remove the 120-second boundary fixture");
  Require(!Search(dump).has_value(),
          "a CrashLogger log 121 seconds away must be rejected");
}

void TestKnownCrashAndThreadDumpKindsCannotCrossPair()
{
  {
    TempDirectory temp("crash-kind-mismatch");
    const auto dump = WriteFile(temp.path(), L"Tullius_crash_20970715_120000.dmp", "dump");
    WriteFile(temp.path(), L"threaddump-2097-07-15-12-00-00.log", kThreadDumpText);
    WriteFile(temp.path(), L"crash-2097-07-15-12-00-01.log", kCrashLogText);

    RequireFoundFilename(
      Search(dump),
      L"crash-2097-07-15-12-00-01.log",
      "a crash dump must not pair with an exact-time thread dump");
  }

  {
    TempDirectory temp("thread-kind-mismatch");
    const auto dump = WriteFile(temp.path(), L"Tullius_hang_20970715_130000.dmp", "dump");
    WriteFile(temp.path(), L"crash-2097-07-15-13-00-00.log", kCrashLogText);
    WriteFile(temp.path(), L"threaddump-2097-07-15-13-00-01.log", kThreadDumpText);

    RequireFoundFilename(
      Search(dump),
      L"threaddump-2097-07-15-13-00-01.log",
      "a thread dump must not pair with an exact-time crash log");
  }
}

void TestEqualTimestampTieBreakIsDeterministic()
{
  TempDirectory temp("equal-time-tie");
  const auto dump = WriteFile(temp.path(), L"Tullius_crash_20970716_140000.dmp", "dump");

  WriteFile(temp.path(), L"renamed-2097-07-16-14-00-00.log", kCrashLogText);
  WriteFile(temp.path(), L"crash-2097-07-16-14-00-00-b.log", kCrashLogText);
  WriteFile(temp.path(), L"crash-2097-07-16-14-00-00-a.log", kCrashLogText);

  RequireFoundFilename(
    Search(dump),
    L"crash-2097-07-16-14-00-00-a.log",
    "equal-time pairing must prefer a known kind, then the stable path order");
}

void TestNearbyCompetingLogMarksPairingAmbiguous()
{
  TempDirectory temp("ambiguous-nearby-log");
  const auto dump = WriteFile(temp.path(), L"Tullius_crash_20970717_150000.dmp", "dump");
  WriteFile(temp.path(), L"crash-2097-07-17-15-00-00-a.log", kCrashLogText);
  WriteFile(temp.path(), L"crash-2097-07-17-15-00-01-b.log", kCrashLogText);

  CrashLoggerPairingMetadata metadata{};
  RequireFoundFilename(
    Search(dump, &metadata),
    L"crash-2097-07-17-15-00-00-a.log",
    "the exact-time log must remain the deterministic selection");
  Require(metadata.ambiguous, "a second valid log within two seconds must mark pairing ambiguous");
  Require(metadata.eligible_candidate_count == 2u, "pairing metadata must count eligible logs");
  Require(metadata.nearby_competitor_count == 1u, "pairing metadata must count nearby competitors");
  Require(metadata.time_delta_ms == 0u, "selected exact-time log must report zero delta");
  Require(metadata.runner_up_time_delta_ms == 1000u, "runner-up delta must be reported");
  Require(metadata.selected_kind == "crash", "selected artifact kind must be recorded");
}

}  // namespace

int main()
{
  try {
    TestTimestampDistanceWinsBeforeFilenameProvenance();
    TestPairingWindowIncludesExactly120SecondsAndRejects121();
    TestKnownCrashAndThreadDumpKindsCannotCrossPair();
    TestEqualTimestampTieBreakIsDeterministic();
    TestNearbyCompetingLogMarksPairingAmbiguous();
    std::cout << "crashlogger search integration tests passed\n";
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "crashlogger search integration tests failed: " << ex.what() << '\n';
    return 1;
  }
}
