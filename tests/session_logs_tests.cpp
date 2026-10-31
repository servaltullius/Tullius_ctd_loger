// Earlier sessions' logs (shared/SkyrimDiagSessionLogs.h), kept by the plugin
// for SkyrimDiag.log and by the helper for SkyrimDiagHelper.log.
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include "SkyrimDiagSessionLogs.h"

namespace {

using skydiag::KeepSessionLog;
using skydiag::PreviousSessionLogPath;

std::filesystem::path MakeTempDir()
{
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto dir = std::filesystem::temp_directory_path() / ("skydiag_session_logs_test_" + std::to_string(stamp));
  std::filesystem::create_directories(dir);
  return dir;
}

void WriteText(const std::filesystem::path& path, const std::string& text)
{
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

std::string ReadText(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

void TestNames()
{
  const std::filesystem::path dir = "logs";
  assert(PreviousSessionLogPath(dir / "SkyrimDiag.log", 1) == dir / "SkyrimDiag.previous.log");
  assert(PreviousSessionLogPath(dir / "SkyrimDiag.log", 0) == dir / "SkyrimDiag.previous.log");
  assert(PreviousSessionLogPath(dir / "SkyrimDiag.log", 2) == dir / "SkyrimDiag.previous.2.log");
  // The names the helper has always used.
  assert(PreviousSessionLogPath(dir / "SkyrimDiagHelper.log", 5) == dir / "SkyrimDiagHelper.previous.5.log");
}

void TestSessionsShiftDownAndTheOldestGoes()
{
  const auto dir = MakeTempDir();
  const auto log = dir / "SkyrimDiag.log";

  // Nothing to keep yet: a missing log, then an empty one.
  assert(KeepSessionLog(log, 3));
  WriteText(log, "");
  assert(KeepSessionLog(log, 3));
  assert(std::filesystem::exists(log));
  assert(!std::filesystem::exists(PreviousSessionLogPath(log, 1)));

  for (int session = 1; session <= 4; ++session) {
    WriteText(log, "session " + std::to_string(session));
    assert(KeepSessionLog(log, 3));
    assert(!std::filesystem::exists(log));
  }
  assert(ReadText(PreviousSessionLogPath(log, 1)) == "session 4");
  assert(ReadText(PreviousSessionLogPath(log, 2)) == "session 3");
  assert(ReadText(PreviousSessionLogPath(log, 3)) == "session 2");
  assert(!std::filesystem::exists(PreviousSessionLogPath(log, 4)));

  // Keeping none still keeps the last one.
  WriteText(log, "session 5");
  assert(KeepSessionLog(log, 0));
  assert(ReadText(PreviousSessionLogPath(log, 1)) == "session 5");

#ifdef _WIN32
  // Held open without delete sharing, it cannot be moved: false, and it is
  // still there for the caller to start over in.
  WriteText(log, "held");
  {
    std::ofstream held(log, std::ios::binary | std::ios::app);
    assert(held);
    assert(!KeepSessionLog(log, 3));
  }
  assert(ReadText(log) == "held");
#endif

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

}  // namespace

int main()
{
  TestNames();
  TestSessionsShiftDownAndTheOldestGoes();
  return 0;
}
