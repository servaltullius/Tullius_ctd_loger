#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace skydiag::dump_tool {

// Best-effort: locate the Crash Logger SSE/AE log file closest in time to the given dump.
// mo2BaseDir is optional; if provided, we also search typical MO2 folders (overwrite/profiles).
std::optional<std::filesystem::path> TryFindCrashLoggerLogForDump(
  const std::filesystem::path& dumpPath,
  const std::optional<std::filesystem::path>& mo2BaseDir,
  std::wstring* err);

// Reads a whole text file as UTF-8/bytes (Crash Logger logs are typically ASCII/UTF-8).
std::optional<std::string> ReadWholeFileUtf8(const std::filesystem::path& path, std::wstring* err);

// Parse "PROBABLE CALL STACK:" section and return modules sorted by frequency.
// canonicalByFilenameLower maps lowercased filename (e.g. "hdtsmp64.dll") to a canonical display filename (e.g. "hdtSMP64.dll").
std::vector<std::wstring> ParseCrashLoggerTopModules(
  std::string_view logUtf8,
  const std::unordered_map<std::wstring, std::wstring>& canonicalByFilenameLower);

}  // namespace skydiag::dump_tool

