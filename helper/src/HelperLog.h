#pragma once

#include <cstdint>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>

namespace skydiag::helper::internal {

void SetHelperLogRotation(std::uint64_t maxBytes, std::uint32_t maxFiles);
void ClearLog(const std::filesystem::path& outBase);
// Starts a new log for a newly attached game. The previous session's log is
// kept as SkyrimDiagHelper.previous.log: users are told to read the helper log
// after a crash, often after relaunching the game.
void StartHelperLogSession(const std::filesystem::path& outBase);
void AppendLogLine(const std::filesystem::path& outBase, std::wstring_view line);
// what() of a standard-library exception is in the ANSI code page (e.g. a
// localized system message or a path); widen it for the log.
std::wstring AnsiExceptionText(const std::exception& ex);

}  // namespace skydiag::helper::internal

