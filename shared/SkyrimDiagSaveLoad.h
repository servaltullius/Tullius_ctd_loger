#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "SkyrimDiagHash.h"
#include "SkyrimDiagShared.h"

namespace skydiag {

// ADR-0025: a SaveLoadStart event names the save a load started with:
// payload.a holds SaveNameKey of the whole name, b..d its first bytes
// (MakeSaveLoadStartPayload). SKSE documents the data as the save's file
// path; a folder or ".ess" is dropped so a key never depends on how it was
// written, and so no folder (with the Windows user name) is recorded.
constexpr std::string_view SaveNameForEvent(std::string_view name) noexcept
{
  if (const auto slash = name.find_last_of("\\/"); slash != std::string_view::npos) {
    name.remove_prefix(slash + 1);
  }
  constexpr std::string_view kExtension = ".ess";
  if (name.size() > kExtension.size()) {
    bool extension = true;
    const auto tail = name.substr(name.size() - kExtension.size());
    for (std::size_t i = 0; i < kExtension.size(); ++i) {
      const char c = tail[i];
      if (((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c) != kExtension[i]) {
        extension = false;
        break;
      }
    }
    if (extension) {
      name.remove_suffix(kExtension.size());
    }
  }
  return name;
}

// FNV-1a 64 of the name with ASCII case folded, as Windows matches file names.
// 0 for no name, which a reader takes as "no save": unnamed loads must never
// match each other.
constexpr std::uint64_t SaveNameKey(std::string_view name) noexcept
{
  const auto save = SaveNameForEvent(name);
  if (save.empty()) {
    return 0u;
  }
  std::uint64_t hash = hash::kFnv1a64Offset;
  for (const char ch : save) {
    hash = hash::Fnv1a64Byte(hash, static_cast<unsigned char>((ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch));
  }
  return hash != 0u ? hash : 1u;
}

// SaveLoadEnd's payload.a: SKSE's PostLoadGame result, or a new game started
// (NewGame), after which no save is in play.
enum class SaveLoadResult : std::uint64_t
{
  kFailed = 0,
  kLoaded = 1,
  kNewGame = 2,
};

// The name's bytes kept in b..d: 23 and a terminating 0.
inline constexpr std::size_t kSaveNameTextBytes = sizeof(EventPayload::b) * 3u - 1u;

// A cut name ends before a UTF-8 character it would split (a byte 10xxxxxx
// continues the one before it).
constexpr std::size_t SaveNameTextLength(std::string_view name) noexcept
{
  std::size_t len = name.size() < kSaveNameTextBytes ? name.size() : kSaveNameTextBytes;
  if (len < name.size()) {
    while (len > 0u && (static_cast<unsigned char>(name[len]) & 0xC0u) == 0x80u) {
      --len;
    }
  }
  return len;
}

inline EventPayload MakeSaveLoadStartPayload(std::string_view saveName) noexcept
{
  const auto name = SaveNameForEvent(saveName);
  EventPayload payload{};
  payload.a = SaveNameKey(name);
  char text[sizeof(EventPayload::b) * 3u]{};
  name.copy(text, SaveNameTextLength(name));
  std::memcpy(&payload.b, text, sizeof(payload.b));
  std::memcpy(&payload.c, text + sizeof(payload.b), sizeof(payload.c));
  std::memcpy(&payload.d, text + sizeof(payload.b) * 2u, sizeof(payload.d));
  return payload;
}

}  // namespace skydiag
