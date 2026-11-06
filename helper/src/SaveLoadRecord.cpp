#include "SaveLoadRecord.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <string_view>

#include <nlohmann/json.hpp>

#include "HelperCommon.h"
#include "SeqlockCopy.h"
#include "SkyrimDiagLongPath.h"
#include "SkyrimDiagSaveLoad.h"

namespace skydiag::helper::internal {

namespace {

// The plugin writes the name as SKSE gives it: UTF-8, or else the ANSI code
// page (the analyzer reads the event the same way).
std::string NameAsUtf8(const std::string& bytes)
{
  if (bytes.empty()) {
    return {};
  }
  const int len = static_cast<int>(bytes.size());
  for (const UINT codePage : { static_cast<UINT>(CP_UTF8), static_cast<UINT>(CP_ACP) }) {
    const DWORD flags = codePage == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0u;
    const int wideLen = MultiByteToWideChar(codePage, flags, bytes.data(), len, nullptr, 0);
    if (wideLen <= 0) {
      continue;
    }
    std::wstring wide(static_cast<std::size_t>(wideLen), L'\0');
    MultiByteToWideChar(codePage, flags, bytes.data(), len, wide.data(), wideLen);
    return WideToUtf8(wide);
  }
  return {};
}

std::string DecodeName(const skydiag::EventPayload& payload)
{
  char text[skydiag::kSaveNameTextBytes + 1u]{};
  std::memcpy(text, &payload.b, sizeof(payload.b));
  std::memcpy(text + sizeof(payload.b), &payload.c, sizeof(payload.c));
  std::memcpy(text + sizeof(payload.b) * 2u, &payload.d, sizeof(payload.d) - 1u);
  return NameAsUtf8(std::string(text, strnlen(text, skydiag::kSaveNameTextBytes)));
}

std::uint32_t ReadWriteIndex(const skydiag::SharedHeader& header) noexcept
{
  auto* const index = reinterpret_cast<volatile LONG*>(const_cast<volatile std::uint32_t*>(&header.write_index));
  return static_cast<std::uint32_t>(InterlockedCompareExchange(index, 0, 0));
}

}  // namespace

std::vector<LoadedSave> SaveLoadWatcher::Poll(
  const skydiag::SharedLayout* shm, std::uint64_t nowQpc, std::uint64_t nowUnix)
{
  std::vector<LoadedSave> loads;
  if (!shm || shm->header.magic != skydiag::kMagic) {
    return loads;
  }
  const auto& header = shm->header;
  std::uint32_t capacity = header.capacity;
  if (capacity == 0u || capacity > skydiag::kEventCapacity) {
    capacity = skydiag::kEventCapacity;
  }
  const std::uint32_t writeIndex = ReadWriteIndex(header);
  const std::uint32_t oldest = writeIndex > capacity ? writeIndex - capacity : 0u;
  if (!started_ || next_ < oldest) {
    // First poll, or a lap behind: start at the oldest entry the ring holds.
    if (started_) {
      loading_ = false;
    }
    next_ = oldest;
    started_ = true;
  }
  for (; next_ < writeIndex; ++next_) {
    skydiag::BlackboxEvent event{};
    if (!CopyStableSeqlockEntry(&shm->events[next_ % capacity], &event)) {
      break;  // being written: read it on the next poll
    }
    // The plugin commits ring index i as seq i*2.
    const std::uint32_t committed = next_ * 2u;
    if (event.seq != committed) {
      if (event.seq < committed) {
        break;  // claimed but not written yet
      }
      continue;  // a later lap already took the slot
    }
    if (event.type == static_cast<std::uint16_t>(skydiag::EventType::kSaveLoadStart)) {
      loading_ = true;
      loadingKey_ = event.payload.a;
      loadingName_ = DecodeName(event.payload);
      loadingEvent_ = next_;
    } else if (event.type == static_cast<std::uint16_t>(skydiag::EventType::kSaveLoadEnd)) {
      if (loading_ && loadingKey_ != 0u &&
          event.payload.a == static_cast<std::uint64_t>(skydiag::SaveLoadResult::kLoaded)) {
        LoadedSave load{};
        load.key = loadingKey_;
        load.name = loadingName_;
        load.attempt_key = skydiag::SaveLoadAttemptKey(header.pid, header.start_qpc, loadingEvent_);
        const std::uint64_t ago = (header.qpc_freq != 0u && nowQpc > event.qpc)
          ? (nowQpc - event.qpc) / header.qpc_freq
          : 0u;
        load.loaded_unix = nowUnix > ago ? nowUnix - ago : nowUnix;
        loads.push_back(std::move(load));
      }
      loading_ = false;  // loaded, failed or a new game: no load in progress
    }
  }
  return loads;
}

std::string MergeLoadedSavesJson(const std::string& existingJson, const std::vector<LoadedSave>& added)
{
  nlohmann::json rows = nlohmann::json::array();
  const auto existing = nlohmann::json::parse(existingJson, nullptr, /*allow_exceptions=*/false);
  if (!existing.is_discarded() && existing.is_object()) {
    const auto version = existing.find("version");
    const auto loads = existing.find("loads");
    if (version != existing.end() && version->is_number_unsigned() && version->get<std::uint64_t>() == 1u &&
        loads != existing.end() && loads->is_array()) {
      rows = *loads;
    }
  }
  for (const auto& load : added) {
    const bool known = !load.attempt_key.empty() && std::any_of(rows.begin(), rows.end(), [&](const nlohmann::json& row) {
      const auto it = row.is_object() ? row.find("attempt_key") : row.end();
      return it != row.end() && it->is_string() && it->get<std::string>() == load.attempt_key;
    });
    if (known) {
      continue;  // read again by a helper started again
    }
    rows.push_back({
      { "save_key", skydiag::SaveKeyHex(load.key) },
      { "save_name", load.name },
      { "loaded_unix", load.loaded_unix },
      { "attempt_key", load.attempt_key },
    });
  }
  if (rows.size() > skydiag::kLoadedSavesKept) {
    rows.erase(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(rows.size() - skydiag::kLoadedSavesKept));
  }
  return DumpJsonForFile(nlohmann::json{ { "version", 1 }, { "loads", rows } });
}

bool RecordLoadedSaves(const std::filesystem::path& outBase, const std::vector<LoadedSave>& added)
{
  if (added.empty()) {
    return true;
  }
  // A long output folder (SkyrimDiagLongPath.h); the write adds a temporary suffix.
  const auto path = skydiag::PathForIo(outBase / skydiag::kLoadedSavesFileName, 48);
  std::string existing;
  if (!ReadTextFileUtf8(path, &existing)) {
    std::error_code ec;
    if (skydiag::ExistsForIo(path, ec) || ec) {
      return false;  // there but unreadable: keep it rather than start over
    }
  }
  return WriteTextFileUtf8(path, MergeLoadedSavesJson(existing, added));
}

std::wstring LoadedSaveLogLine(const LoadedSave& load, bool recorded)
{
  std::wstring name;
  if (!load.name.empty()) {
    const int len = static_cast<int>(load.name.size());
    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, load.name.data(), len, nullptr, 0);
    if (wideLen > 0) {
      name.resize(static_cast<std::size_t>(wideLen));
      MultiByteToWideChar(CP_UTF8, 0, load.name.data(), len, name.data(), wideLen);
    }
  }
  return L"Save load finished: " + (name.empty() ? std::wstring(L"(no name)") : name) +
    (recorded ? L"" : L" (loaded_saves.json could not be written)");
}

}  // namespace skydiag::helper::internal
