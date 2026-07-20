#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace skydiag::dump_tool {

struct Mo2Index;

struct CrashLoggerPairingMetadata
{
  std::uint64_t time_delta_ms = 0;
  std::uint64_t runner_up_time_delta_ms = 0;
  std::uint32_t eligible_candidate_count = 0;
  std::uint32_t nearby_competitor_count = 0;
  std::string selected_kind;
  bool selected_same_directory = false;
  bool ambiguous = false;
};

// Best-effort: locate the Crash Logger SSE/AE log file closest in time to the given dump.
// mo2BaseDir is optional; if provided, we also search typical MO2 folders (overwrite/profiles).
// mo2Index is optional; if provided, we can consult the active MO2 left-pane ordering for config files.
// gameRootDir is optional; if provided, it is used to resolve CrashLogger.ini and relative Crashlog Directory paths.
std::optional<std::filesystem::path> TryFindCrashLoggerLogForDump(
  const std::filesystem::path& dumpPath,
  const std::optional<std::filesystem::path>& mo2BaseDir,
  const Mo2Index* mo2Index,
  const std::optional<std::filesystem::path>& gameRootDir,
  std::wstring* err,
  CrashLoggerPairingMetadata* pairingMetadata = nullptr);

// Reads a whole text file as UTF-8/bytes (Crash Logger logs are typically ASCII/UTF-8).
std::optional<std::string> ReadWholeFileUtf8(const std::filesystem::path& path, std::wstring* err);

// Parse "PROBABLE CALL STACK:" section and return modules sorted by frequency.
// canonicalByFilenameLower maps lowercased filename (e.g. "hdtsmp64.dll") to a canonical display filename (e.g. "hdtSMP64.dll").
std::vector<std::wstring> ParseCrashLoggerTopModules(
  std::string_view logUtf8,
  const std::unordered_map<std::wstring, std::wstring>& canonicalByFilenameLower);

}  // namespace skydiag::dump_tool
