#pragma once

#include <cstdint>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>

namespace skydiag::helper::internal {

void SetHelperLogRotation(std::uint64_t maxBytes, std::uint32_t maxFiles);
// Earlier sessions' logs kept by StartHelperLogSession (MaxHelperSessionLogs).
inline constexpr std::uint32_t kDefaultHelperSessionLogsToKeep = 5;
void SetHelperSessionLogsToKeep(std::uint32_t sessions);
// The log of the session `age` starts back: 1 = SkyrimDiagHelper.previous.log,
// 2 = SkyrimDiagHelper.previous.2.log, ...
std::filesystem::path HelperSessionLogPath(const std::filesystem::path& outBase, std::uint32_t age);
void ClearLog(const std::filesystem::path& outBase);
// Starts a new log for a newly attached game. The previous session's log is
// kept as SkyrimDiagHelper.previous.log, older ones as previous.2.log and so
// on (SetHelperSessionLogsToKeep): users are told to read the helper log after
// a crash, often after relaunching the game more than once.
// Lines queued with QueueSessionStartLogLine are written right after the new
// log starts.
void StartHelperLogSession(const std::filesystem::path& outBase);
// A startup warning found before the session log exists (e.g. the output
// folder fallback); written once by StartHelperLogSession.
void QueueSessionStartLogLine(std::wstring line);
void AppendLogLine(const std::filesystem::path& outBase, std::wstring_view line);
// The output folder of the session log StartHelperLogSession started; empty
// before that. For the last line of a helper that stops on an exception.
std::filesystem::path HelperSessionOutBase();
// what() of a standard-library exception is in the ANSI code page (e.g. a
// localized system message or a path); widen it for the log.
std::wstring AnsiExceptionText(const std::exception& ex);

}  // namespace skydiag::helper::internal

