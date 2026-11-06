#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skydiag::dump_tool {

// One finished load from loaded_saves.json, which the helper keeps beside
// crash_history.json (ADR-0025, kLoadedSavesFileName in SkyrimDiagSaveLoad.h).
struct LoadedSave
{
  std::string key;  // SaveKeyHex of the whole name
  std::string name_utf8;
  std::uint64_t loaded_unix = 0;
  std::string attempt_key;  // SaveLoadAttemptKey: whose session it was
};

// Its loads, oldest first; empty when the file is missing, does not parse or
// is of another version. Rows without a key or a time are left out.
std::vector<LoadedSave> ReadLoadedSaves(const std::filesystem::path& path);

// The newest load of a save other than saveKey in the day up to and
// including captureUnix: what a failed load of saveKey can be told about.
// Later loads are left out, so a capture's answer does not change as the
// helper keeps recording. None without a save key or a capture time.
std::optional<LoadedSave> NewestOtherLoadedSave(
  const std::vector<LoadedSave>& loads, std::string_view saveKey, std::uint64_t captureUnix);

}  // namespace skydiag::dump_tool
