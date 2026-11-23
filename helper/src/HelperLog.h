#pragma once

#include <cstdint>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>

namespace skydiag::helper::internal {

void SetHelperLogRotation(std::uint64_t maxBytes, std::uint32_t maxFiles);
// Earlier sessions' logs kept by StartHelperLogSession (MaxHelperSessionLogs).
inline constexpr std::uint32_t kDefaultHelperSessionLogsToKeep = 20;
void SetHelperSessionLogsToKeep(std::uint32_t sessions);
// The log of the session `age` starts back: 1 = SkyrimDiagHelper.previous.log,
// 2 = SkyrimDiagHelper.previous.2.log, ...
std::filesystem::path HelperSessionLogPath(const std::filesystem::path& outBase, std::uint32_t age);
void ClearLog(const std::filesystem::path& outBase);
// Set the "Attached to pid=... at ..." prefix before starting the session.
void SetHelperSessionLogMarker(std::wstring marker);
// Starts a new log for a newly attached game. The previous session's log is
// kept as SkyrimDiagHelper.previous.log, older ones as previous.2.log and so
// on (SetHelperSessionLogsToKeep): users are told to read the helper log after
// a crash, often after relaunching the game more than once.
// The session marker is written first, followed by queued startup lines.
void StartHelperLogSession(const std::filesystem::path& outBase);
// A startup warning found before the session log exists (e.g. the output
// folder fallback); written once by StartHelperLogSession.
void QueueSessionStartLogLine(std::wstring line);
void AppendLogLine(const std::filesystem::path& outBase, std::wstring_view line);
// Remember the timestamp of a successfully written dump. Renames and
// recaptures keep the timestamp of the incident.
void RecordSessionDump(std::wstring_view dumpPath) noexcept;
void ClearSessionDumpTokensForTests();
// Call once after the capture loop and retention worker have shut down.
void KeepSessionHelperLogBesideDumps(const std::filesystem::path& outBase) noexcept;
// Where the helper logs before StartHelperLogSession: the configured
// OutputDir, set once the config is read.
void SetHelperPreSessionOutBase(const std::filesystem::path& outBase);
// The output folder of the session log StartHelperLogSession started, or the
// pre-session one before that (empty before the config is read). For the
// last line of a helper that stops on an exception.
std::filesystem::path HelperSessionOutBase();
// what() of a standard-library exception is in the ANSI code page (e.g. a
// localized system message or a path); widen it for the log.
std::wstring AnsiExceptionText(const std::exception& ex);

}  // namespace skydiag::helper::internal

