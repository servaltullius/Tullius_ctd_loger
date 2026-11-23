#include "HelperLog.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "HelperCommon.h"
#include "SkyrimDiagHelper/Retention.h"
#include "SkyrimDiagHandle.h"
#include "SkyrimDiagLongPath.h"
#include "SkyrimDiagSessionLogs.h"

namespace skydiag::helper::internal {
namespace {

constexpr LONGLONG kMaxPreservedHelperLogBytes = 2LL * 1024 * 1024;
constexpr std::string_view kHelperLogTruncationLine = "[Helper log truncated to its last 2 MiB]\r\n";
// Every session log starts with "Attached to pid=<pid> at <time>" (SetHelperSessionLogMarker).
constexpr std::string_view kSessionMarkerPrefix = "Attached to pid=";

std::uint64_t g_maxHelperLogBytes = 0;
std::uint32_t g_maxHelperLogFiles = 0;
std::uint32_t g_helperSessionLogsToKeep = kDefaultHelperSessionLogsToKeep;
std::wstring g_sessionLogMarker;
std::vector<std::wstring> g_sessionStartLines;
std::filesystem::path g_sessionOutBase;
std::filesystem::path g_preSessionOutBase;
std::mutex g_logMutex;
std::unordered_set<std::string> g_sessionDumpTokens;
std::mutex g_sessionDumpMutex;

void ClearLogUnlocked(const std::filesystem::path& outBase)
{
  std::error_code ec;
  const auto path = outBase / L"SkyrimDiagHelper.log";
  if (skydiag::ExistsForIo(path, ec) && !ec) {
    // Truncate: open in non-append mode and immediately close.
    std::ofstream f(skydiag::PathForIo(path), std::ios::binary | std::ios::trunc);
  }
}

void RotateAndStartSession(const std::filesystem::path& outBase)
{
  std::lock_guard<std::mutex> lock(g_logMutex);
  // A crash is often followed by several relaunches before anyone reads the
  // logs.
  if (!skydiag::KeepSessionLog(outBase / L"SkyrimDiagHelper.log", g_helperSessionLogsToKeep)) {
    ClearLogUnlocked(outBase);  // a locked log is still restarted, as before
  }
}

// Opened so that a newly attached helper can still rotate the file while it is read.
skydiag::UniqueHandle OpenLogForSharedRead(const std::filesystem::path& path)
{
  return skydiag::UniqueHandle(CreateFileW(
    skydiag::PathForIo(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
}

bool LogStartsWith(const std::filesystem::path& path, std::string_view prefix)
{
  const auto file = OpenLogForSharedRead(path);
  if (file.get() == INVALID_HANDLE_VALUE || prefix.empty()) {
    return false;
  }
  std::string head(prefix.size(), '\0');
  DWORD read = 0;
  const auto headBytes = static_cast<DWORD>(head.size());
  return ReadFile(file.get(), head.data(), headBytes, &read, nullptr) && read == headBytes && head == prefix;
}

// The last kMaxPreservedHelperLogBytes of `files` (oldest first) read as one
// log, starting at a line boundary when cut.
std::optional<std::string> ReadLogsTail(const std::vector<std::filesystem::path>& files)
{
  std::vector<std::string> parts;
  LONGLONG remaining = kMaxPreservedHelperLogBytes;
  bool truncated = false;
  for (auto it = files.rbegin(); it != files.rend(); ++it) {
    if (remaining == 0) {
      truncated = true;
      break;
    }
    const auto file = OpenLogForSharedRead(*it);
    if (file.get() == INVALID_HANDLE_VALUE) {
      return std::nullopt;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) {
      return std::nullopt;
    }
    const auto take = std::min(size.QuadPart, remaining);
    LARGE_INTEGER offset{};
    offset.QuadPart = size.QuadPart - take;
    std::string part(static_cast<std::size_t>(take), '\0');
    DWORD read = 0;
    if (take > 0 && (!SetFilePointerEx(file.get(), offset, nullptr, FILE_BEGIN) ||
                     !ReadFile(file.get(), part.data(), static_cast<DWORD>(take), &read, nullptr) ||
                     read != static_cast<DWORD>(take))) {
      return std::nullopt;
    }
    remaining -= take;
    if (offset.QuadPart > 0) {
      truncated = true;
    }
    parts.push_back(std::move(part));
  }
  std::string tail;
  for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
    tail += *it;
  }
  if (truncated) {
    const auto newline = tail.find('\n');
    tail.erase(0, newline == std::string::npos ? tail.size() : newline + 1);
    tail.insert(0, kHelperLogTruncationLine);
  }
  return tail;
}

// This session's log: the current log or an earlier session's
// (previous*.log) that starts with the session marker. A log that grew past
// MaxHelperLogBytes in this session was moved to SkyrimDiagHelper.log.1, .2,
// ...; then the session is the archive that starts with the marker, the newer
// archives and the current log, which does not start a session of its own.
std::optional<std::string> ReadSessionLogTail(const std::filesystem::path& outBase)
{
  const auto marker = WideToUtf8(g_sessionLogMarker);
  if (marker.empty()) {
    return std::nullopt;
  }
  const auto current = outBase / L"SkyrimDiagHelper.log";
  {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (LogStartsWith(current, marker)) {
      return ReadLogsTail({ current });
    }
    if (!LogStartsWith(current, kSessionMarkerPrefix)) {
      for (std::uint32_t k = 1; k <= g_maxHelperLogFiles; ++k) {
        auto archive = current;
        archive += L"." + std::to_wstring(k);
        if (!LogStartsWith(archive, marker)) {
          continue;
        }
        std::vector<std::filesystem::path> files;
        for (std::uint32_t i = k; i >= 1; --i) {
          auto part = current;
          part += L"." + std::to_wstring(i);
          files.push_back(std::move(part));
        }
        files.push_back(current);
        return ReadLogsTail(files);
      }
    }
  }
  for (std::uint32_t i = 0; i < g_helperSessionLogsToKeep; ++i) {
    const auto previous = HelperSessionLogPath(outBase, i + 1u);
    if (LogStartsWith(previous, marker)) {
      return ReadLogsTail({ previous });
    }
  }
  return std::nullopt;
}

}  // namespace

void QueueSessionStartLogLine(std::wstring line)
{
  if (std::find(g_sessionStartLines.begin(), g_sessionStartLines.end(), line) == g_sessionStartLines.end()) {
    g_sessionStartLines.push_back(std::move(line));
  }
}

void SetHelperLogRotation(std::uint64_t maxBytes, std::uint32_t maxFiles)
{
  g_maxHelperLogBytes = maxBytes;
  g_maxHelperLogFiles = maxFiles;
}

void SetHelperSessionLogsToKeep(std::uint32_t sessions)
{
  g_helperSessionLogsToKeep = sessions;
}

std::filesystem::path HelperSessionLogPath(const std::filesystem::path& outBase, std::uint32_t age)
{
  return skydiag::PreviousSessionLogPath(outBase / L"SkyrimDiagHelper.log", age);
}

void ClearLog(const std::filesystem::path& outBase)
{
  std::lock_guard<std::mutex> lock(g_logMutex);
  ClearLogUnlocked(outBase);
}

void SetHelperSessionLogMarker(std::wstring marker)
{
  g_sessionLogMarker = std::move(marker);
}

void StartHelperLogSession(const std::filesystem::path& outBase)
{
  g_sessionOutBase = outBase;
  RotateAndStartSession(outBase);
  if (!g_sessionLogMarker.empty()) {
    AppendLogLine(outBase, g_sessionLogMarker + L", output=" + outBase.native());
  }
  for (const auto& line : g_sessionStartLines) {
    AppendLogLine(outBase, line);
  }
  g_sessionStartLines.clear();
}

void SetHelperPreSessionOutBase(const std::filesystem::path& outBase)
{
  g_preSessionOutBase = outBase;
}

std::filesystem::path HelperSessionOutBase()
{
  return g_sessionOutBase.empty() ? g_preSessionOutBase : g_sessionOutBase;
}

void AppendLogLine(const std::filesystem::path& outBase, std::wstring_view line)
{
  std::lock_guard<std::mutex> lock(g_logMutex);
  std::error_code ec;
  skydiag::CreateDirectoriesForIo(outBase, ec);

  const auto path = outBase / L"SkyrimDiagHelper.log";
  skydiag::helper::RotateLogFileIfNeeded(path, g_maxHelperLogBytes, g_maxHelperLogFiles);
  std::ofstream f(skydiag::PathForIo(path), std::ios::binary | std::ios::app);
  if (!f) {
    return;
  }

  std::wstring msg(line);
  msg += L"\r\n";
  const auto utf8 = WideToUtf8(msg);
  if (!utf8.empty()) {
    f.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
  }
}

void RecordSessionDump(std::wstring_view dumpPath) noexcept
{
  try {
    auto token = TryExtractTimestampToken(PathPartUtf8(std::filesystem::path(dumpPath).stem()));
    if (token) {
      std::lock_guard<std::mutex> lock(g_sessionDumpMutex);
      g_sessionDumpTokens.insert(std::move(*token));
    }
  } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): recording must never fail an already written dump
  }
}

void ClearSessionDumpTokensForTests()
{
  std::lock_guard<std::mutex> lock(g_sessionDumpMutex);
  g_sessionDumpTokens.clear();
}

static void KeepSessionHelperLogBesideDumpsImpl(const std::filesystem::path& outBase)
{
  std::unordered_set<std::string> tokens;
  {
    std::lock_guard<std::mutex> lock(g_sessionDumpMutex);
    tokens = g_sessionDumpTokens;
  }
  if (tokens.empty()) {
    return;
  }
  const auto logBytes = ReadSessionLogTail(outBase);
  if (!logBytes) {
    AppendLogLine(outBase, L"Could not find this session's helper log.");
    return;
  }

  std::error_code ec;
  const auto entries = skydiag::ListDirectoryForIo(outBase, ec);
  if (ec) {
    return;
  }
  std::size_t keptCount = 0;
  std::wstring copies;
  std::wstring failures;
  for (const auto& entry : entries) {
    if (entry.directory || entry.path.extension() != L".dmp") {
      continue;
    }
    const auto stem = entry.path.stem();
    const auto token = TryExtractTimestampToken(PathPartUtf8(stem));
    if (!token || !tokens.contains(*token)) {
      continue;
    }
    const auto copyName = stem.native() + L"_HelperLog.txt";
    const bool kept = WriteTextFileUtf8(outBase / copyName, *logBytes);
    auto& names = kept ? copies : failures;
    if (!names.empty()) {
      names += L", ";
    }
    if (kept) {
      ++keptCount;
      names += copyName;
    } else {
      names += entry.path.filename().native();
    }
  }
  if (keptCount > 0) {
    AppendLogLine(outBase, L"Kept this session's helper log beside " + std::to_wstring(keptCount) + L" dump(s): " + copies);
  }
  if (!failures.empty()) {
    AppendLogLine(outBase, L"Could not keep the helper log beside: " + failures);
  }
}

std::wstring AnsiExceptionText(const std::exception& ex)
{
  const std::string what = ex.what();
  if (what.empty() || what.size() > 0x7FFFFFFFu) {
    return {};
  }
  const int len = static_cast<int>(what.size());
  const int wideLen = MultiByteToWideChar(CP_ACP, 0, what.data(), len, nullptr, 0);
  if (wideLen <= 0) {
    return std::wstring(what.begin(), what.end());
  }
  std::wstring wide(static_cast<std::size_t>(wideLen), wchar_t{});
  MultiByteToWideChar(CP_ACP, 0, what.data(), len, wide.data(), wideLen);
  return wide;
}

void KeepSessionHelperLogBesideDumps(const std::filesystem::path& outBase) noexcept
{
  // The last thing the helper does: a failure here must not end it with an error.
  std::wstring failure;
  try {
    KeepSessionHelperLogBesideDumpsImpl(outBase);
    return;
  } catch (const std::exception& ex) {
    try {
      failure = L"Could not keep this session's helper log: " + AnsiExceptionText(ex);
    } catch (...) {  // NOLINT(bugprone-empty-catch): the plain line below is logged instead
    }
  } catch (...) {  // NOLINT(bugprone-empty-catch): reported by the plain line below
  }
  try {
    AppendLogLine(outBase, failure.empty() ? std::wstring_view(L"Could not keep this session's helper log.") : failure);
  } catch (...) {  // NOLINT(bugprone-empty-catch): nothing is left to report to
  }
}

}  // namespace skydiag::helper::internal

