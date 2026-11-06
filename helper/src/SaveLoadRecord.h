#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "SkyrimDiagShared.h"

namespace skydiag::helper::internal {

// ADR-0025 (2026-11-06): a save that finished loading, for loaded_saves.json
// (kLoadedSavesFileName in SkyrimDiagSaveLoad.h).
struct LoadedSave
{
  std::uint64_t key = 0;     // SaveNameKey of the whole name
  std::string name;          // the event's first 23 bytes, as the plugin wrote them
  std::string attempt_key;   // SaveLoadAttemptKey of its SaveLoadStart
  std::uint64_t loaded_unix = 0;  // when SaveLoadEnd was written
};

// Follows the blackbox ring for a SaveLoadStart and the SaveLoadEnd that says
// it loaded. The first poll reads what the ring still holds, so a helper
// started again finds the session's loads (attempt keys keep them once).
class SaveLoadWatcher
{
public:
  // Loads that finished since the last poll. nowQpc and nowUnix are the same
  // moment, to date an event by its QPC.
  std::vector<LoadedSave> Poll(const skydiag::SharedLayout* shm, std::uint64_t nowQpc, std::uint64_t nowUnix);

private:
  bool started_ = false;  // the ring position below is set
  std::uint32_t next_ = 0;
  // The newest SaveLoadStart, until its SaveLoadEnd.
  bool loading_ = false;
  std::uint64_t loadingKey_ = 0;
  std::string loadingName_;
  std::uint32_t loadingEvent_ = 0;
};

// The file's text with `added` merged into `existingJson` (any text: a
// damaged or foreign file starts over): one row per attempt key, oldest first,
// the newest kLoadedSavesKept.
std::string MergeLoadedSavesJson(const std::string& existingJson, const std::vector<LoadedSave>& added);

// Adds them to outBase/loaded_saves.json; false when it could not be written.
bool RecordLoadedSaves(const std::filesystem::path& outBase, const std::vector<LoadedSave>& added);

// "Save load finished: Save75_..." for the helper log, noting a record that
// could not be written.
std::wstring LoadedSaveLogLine(const LoadedSave& load, bool recorded);

}  // namespace skydiag::helper::internal
