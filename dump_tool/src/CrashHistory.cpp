#include "CrashHistory.h"
#include "JsonValue.h"
#include "SkyrimDiagHash.h"
#include "SkyrimDiagLongPath.h"

#ifdef _WIN32
#include <Windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace skydiag::dump_tool {
namespace {

std::atomic<std::uint64_t> g_historyTempCounter{ 0u };

std::string NormalizeStoredCandidateKey(std::string_view value)
{
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
    value.remove_prefix(1);
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
    value.remove_suffix(1);
  }
  std::string key(value);
  for (char& ch : key) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return key;
}

void UpsertHistoryEntry(std::vector<CrashHistoryEntry>* entries, CrashHistoryEntry entry)
{
  // Old writers emitted an exception code with three empty fields, or a
  // freeze prefix without a bucket. Neither identifies a failure place.
  if (entry.loading_fault_place.ends_with("|||") || entry.loading_fault_place == "freeze|") {
    entry.loading_fault_place.clear();
  }
  if (!entries || entry.dump_file.empty()) {
    if (entries) {
      entries->push_back(std::move(entry));
    }
    return;
  }

  const auto dumpKey = NormalizeStoredCandidateKey(entry.dump_file);
  const auto existing = std::find_if(entries->begin(), entries->end(), [&](const CrashHistoryEntry& row) {
    if (!entry.dump_identity_key.empty()) {
      return !row.dump_identity_key.empty() &&
        row.dump_identity_key == entry.dump_identity_key;
    }
    return row.dump_identity_key.empty() &&
      NormalizeStoredCandidateKey(row.dump_file) == dumpKey;
  });
  if (existing == entries->end()) {
    entries->push_back(std::move(entry));
    return;
  }

  // Reanalysis refreshes the diagnosis for the same incident without turning it
  // into a second crash. Keep the original observation time and list position.
  if (!existing->timestamp_utc.empty()) {
    entry.timestamp_utc = existing->timestamp_utc;
  }
  *existing = std::move(entry);
}

// The rows RemoveEntriesForDumpFile removes for a dump (dumpKey normalized).
bool IsEntryForDump(const CrashHistoryEntry& row, std::string_view dumpKey, std::string_view dumpIdentityKey)
{
  if (!dumpIdentityKey.empty() && !row.dump_identity_key.empty()) {
    return row.dump_identity_key == dumpIdentityKey;
  }
  // Legacy rows have no identity. Exclude same-basename legacy evidence
  // conservatively for current-incident correlation, but never merge it
  // into a new identity-bound row during persistence.
  return row.dump_identity_key.empty() && !dumpKey.empty() && NormalizeStoredCandidateKey(row.dump_file) == dumpKey;
}

// Seconds since 1970 of "YYYY-MM-DDTHH:MM:SS..." (UTC, as timestamp_utc is
// written), or nothing.
std::optional<std::int64_t> ParseUtcSeconds(std::string_view text)
{
  if (text.size() < 19u || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':') {
    return std::nullopt;
  }
  const auto number = [&](std::size_t at, std::size_t len) -> std::optional<int> {
    int value = 0;
    for (std::size_t i = at; i < at + len; ++i) {
      if (text[i] < '0' || text[i] > '9') {
        return std::nullopt;
      }
      value = value * 10 + (text[i] - '0');
    }
    return value;
  };
  const auto y = number(0, 4);
  const auto mo = number(5, 2);
  const auto d = number(8, 2);
  const auto h = number(11, 2);
  const auto mi = number(14, 2);
  const auto s = number(17, 2);
  if (!y || !mo || !d || !h || !mi || !s || *mo < 1 || *mo > 12 || *d < 1 || *d > 31) {
    return std::nullopt;
  }
  // Days from 1970-01-01 (H. Hinnant's days_from_civil).
  const std::int64_t year = *y - (*mo <= 2 ? 1 : 0);
  const std::int64_t era = year / 400;
  const std::int64_t yoe = year - era * 400;
  const std::int64_t doy = (153 * (*mo + (*mo > 2 ? -3 : 9)) + 2) / 5 + *d - 1;
  const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const std::int64_t days = era * 146097 + doe - 719468;
  return days * 86400 + std::int64_t{ *h } * 3600 + std::int64_t{ *mi } * 60 + *s;
}

}  // namespace

CrashHistoryLoad CrashHistory::Load(const std::filesystem::path& path)
{
  try {
    std::ifstream f(skydiag::PathForIo(path));
    if (!f.is_open()) {
      std::error_code ec;
      const bool exists = skydiag::ExistsForIo(path, ec);
      return (!exists && !ec) ? CrashHistoryLoad::kMissing : CrashHistoryLoad::kUnreadable;
    }
    const auto j = nlohmann::json::parse(f, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object() || !j.contains("entries") || !j["entries"].is_array()) {
      return CrashHistoryLoad::kDamaged;
    }
    const auto historyVersion = JsonValueOr(j, "version", 1u);
    if (historyVersion != 1u && historyVersion != 2u && historyVersion != 3u) {
      return CrashHistoryLoad::kUnsupportedVersion;
    }

    std::vector<CrashHistoryEntry> loaded;
    loaded.reserve(j["entries"].size());

    for (const auto& e : j["entries"]) {
      if (!e.is_object()) {
        continue;
      }
      CrashHistoryEntry row{};
      row.candidate_key_version = JsonValueOr(e, "candidate_key_version", historyVersion >= 2u ? 2u : 1u);
      if (row.candidate_key_version != 1u && row.candidate_key_version != 2u) {
        row.candidate_key_version = 1u;
      }
      row.timestamp_utc = JsonValueOr(e, "timestamp_utc", std::string{});
      row.dump_file = JsonValueOr(e, "dump_file", std::string{});
      row.dump_identity_key = JsonValueOr(e, "dump_identity_key", std::string{});
      row.bucket_key = JsonValueOr(e, "bucket_key", std::string{});
      row.top_suspect = JsonValueOr(e, "top_suspect", std::string{});
      row.confidence = JsonValueOr(e, "confidence", std::string{});
      row.signature_id = JsonValueOr(e, "signature_id", std::string{});
      row.capture_unix = JsonValueOr(e, "capture_unix", std::uint64_t{ 0 });
      row.loading_save_key = JsonValueOr(e, "loading_save_key", std::string{});
      row.loading_save_name = JsonValueOr(e, "loading_save_name", std::string{});
      row.loading_fault_place = JsonValueOr(e, "loading_fault_place", std::string{});
      row.loading_attempt_key = JsonValueOr(e, "loading_attempt_key", std::string{});
      if (e.contains("all_suspects") && e["all_suspects"].is_array()) {
        for (const auto& s : e["all_suspects"]) {
          if (s.is_string()) {
            row.all_suspects.push_back(s.get<std::string>());
          }
        }
      }
      if (e.contains("candidate_keys") && e["candidate_keys"].is_array()) {
        for (const auto& key : e["candidate_keys"]) {
          if (key.is_string()) {
            row.candidate_keys.push_back(key.get<std::string>());
          }
        }
      }
      UpsertHistoryEntry(&loaded, std::move(row));
    }

    while (loaded.size() > kMaxEntries) {
      loaded.erase(loaded.begin());
    }

    m_entries = std::move(loaded);
    return CrashHistoryLoad::kLoaded;
  } catch (...) {
    // Out of memory or a read error: not proof the file is damaged.
    return CrashHistoryLoad::kUnreadable;
  }
}

bool CrashHistory::SaveToFile(const std::filesystem::path& path) const
{
  try {
    nlohmann::json j = nlohmann::json::object();
    j["version"] = 3;
    j["entries"] = nlohmann::json::array();
    for (const auto& e : m_entries) {
      nlohmann::json row = {
        { "candidate_key_version", e.candidate_key_version },
        { "timestamp_utc", e.timestamp_utc },
        { "dump_file", e.dump_file },
        { "dump_identity_key", e.dump_identity_key },
        { "bucket_key", e.bucket_key },
        { "top_suspect", e.top_suspect },
        { "confidence", e.confidence },
        { "signature_id", e.signature_id },
        { "all_suspects", e.all_suspects },
        { "candidate_keys", e.candidate_keys },
      };
      // ADR-0025: only a crash while loading a save has the loading fields.
      if (e.capture_unix != 0u) {
        row["capture_unix"] = e.capture_unix;
      }
      if (!e.loading_save_key.empty()) {
        row["loading_save_key"] = e.loading_save_key;
        row["loading_save_name"] = e.loading_save_name;
        row["loading_fault_place"] = e.loading_fault_place;
        if (!e.loading_attempt_key.empty()) {
          row["loading_attempt_key"] = e.loading_attempt_key;
        }
      }
      j["entries"].push_back(std::move(row));
    }

    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
      skydiag::CreateDirectoriesForIo(parent, ec);
      if (ec) {
        return false;
      }
    }
    // A long output folder (SkyrimDiagLongPath.h); the temporary name adds up
    // to 48 characters.
    const auto ioPath = skydiag::PathForIo(path, 48);
    auto tempPath = ioPath;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    tempPath += ".tmp." + std::to_string(now) + "." +
      std::to_string(g_historyTempCounter.fetch_add(1u, std::memory_order_relaxed));

    bool wrote = false;
    {
      std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
      if (out) {
        out << j.dump(2);
        out.flush();
        out.close();
        wrote = static_cast<bool>(out);
      }
    }
    if (!wrote) {
      std::filesystem::remove(tempPath, ec);
      return false;
    }

    // Replace in one filesystem operation. Never unlink the authoritative
    // history first: if replacement fails, callers keep the previous file.
#ifdef _WIN32
    if (!MoveFileExW(
          tempPath.c_str(),
          ioPath.c_str(),
          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      std::filesystem::remove(tempPath, ec);
      return false;
    }
#else
    std::filesystem::rename(tempPath, ioPath, ec);
    if (ec) {
      std::filesystem::remove(tempPath, ec);
      return false;
    }
#endif
    return true;
  } catch (...) {
    return false;
  }
}

void CrashHistory::AddEntry(CrashHistoryEntry entry)
{
  UpsertHistoryEntry(&m_entries, std::move(entry));
  while (m_entries.size() > kMaxEntries) {
    m_entries.erase(m_entries.begin());
  }
}

std::size_t CrashHistory::RemoveEntriesForDumpFile(
  std::string_view dumpFile,
  std::string_view dumpIdentityKey)
{
  const auto dumpKey = NormalizeStoredCandidateKey(dumpFile);
  if (dumpKey.empty() && dumpIdentityKey.empty()) {
    return 0;
  }
  const auto oldSize = m_entries.size();
  m_entries.erase(
    std::remove_if(m_entries.begin(), m_entries.end(), [&](const CrashHistoryEntry& row) {
      return IsEntryForDump(row, dumpKey, dumpIdentityKey);
    }),
    m_entries.end());
  return oldSize - m_entries.size();
}

std::vector<ModuleStats> CrashHistory::GetModuleStats(std::size_t lastN) const
{
  std::vector<ModuleStats> result;
  if (m_entries.empty()) {
    return result;
  }

  const std::size_t count = (lastN == 0 || lastN > m_entries.size()) ? m_entries.size() : lastN;
  const std::size_t begin = m_entries.size() - count;

  std::unordered_map<std::string, ModuleStats> byModule;

  for (std::size_t i = begin; i < m_entries.size(); ++i) {
    const auto& entry = m_entries[i];

    if (!entry.top_suspect.empty()) {
      auto& s = byModule[entry.top_suspect];
      s.module_name = entry.top_suspect;
      s.as_top_suspect += 1;
      s.total_crashes = count;
    }

    for (const auto& mod : entry.all_suspects) {
      if (mod.empty()) {
        continue;
      }
      auto& s = byModule[mod];
      s.module_name = mod;
      s.total_appearances += 1;
      s.total_crashes = count;
    }
  }

  result.reserve(byModule.size());
  for (auto& [_, stats] : byModule) {
    result.push_back(std::move(stats));
  }

  std::sort(result.begin(), result.end(), [](const ModuleStats& a, const ModuleStats& b) {
    if (a.as_top_suspect != b.as_top_suspect) {
      return a.as_top_suspect > b.as_top_suspect;
    }
    if (a.total_appearances != b.total_appearances) {
      return a.total_appearances > b.total_appearances;
    }
    return a.module_name < b.module_name;
  });
  return result;
}

BucketStats CrashHistory::GetBucketStats(const std::string& bucketKey) const
{
  BucketStats result{};
  if (bucketKey.empty()) {
    return result;
  }
  for (const auto& e : m_entries) {
    if (e.bucket_key == bucketKey) {
      result.count += 1;
      if (result.first_seen.empty() || e.timestamp_utc < result.first_seen) {
        result.first_seen = e.timestamp_utc;
      }
      if (result.last_seen.empty() || e.timestamp_utc > result.last_seen) {
        result.last_seen = e.timestamp_utc;
      }
    }
  }
  return result;
}

std::vector<BucketCandidateStats> CrashHistory::GetBucketCandidateStats(const std::string& bucketKey) const
{
  std::vector<BucketCandidateStats> result;
  if (bucketKey.empty()) {
    return result;
  }

  std::unordered_map<std::string, BucketCandidateStats> byKey;
  for (const auto& e : m_entries) {
    if (e.bucket_key != bucketKey) {
      continue;
    }

    std::unordered_set<std::string> seenInEntry;
    if (e.candidate_key_version >= 2u && !e.candidate_keys.empty()) {
      for (const auto& rawKey : e.candidate_keys) {
        const auto key = NormalizeStoredCandidateKey(rawKey);
        if (!key.empty() && seenInEntry.insert(key).second) {
          auto& row = byKey[key];
          row.candidate_key = key;
          row.count += 1;
        }
      }
      continue;
    }
    // v1 keys removed all separators/non-ASCII and can alias distinct v2
    // candidates (for example A-B vs AB). Do not let ambiguous legacy keys
    // boost a modern actionable candidate; bucket-level history still works.
  }

  result.reserve(byKey.size());
  for (auto& [_, row] : byKey) {
    result.push_back(std::move(row));
  }
  std::sort(result.begin(), result.end(), [](const BucketCandidateStats& a, const BucketCandidateStats& b) {
    if (a.count != b.count) {
      return a.count > b.count;
    }
    return a.candidate_key < b.candidate_key;
  });
  return result;
}

std::string CrashHistory::SaveLoadHistoryFingerprint(std::string_view excludeContentSha256) const
{
  std::uint64_t sum = 0;
  for (const auto& e : m_entries) {
    if (e.loading_save_key.empty()) {
      continue;
    }
    const std::string_view identity(e.dump_identity_key);
    if (!excludeContentSha256.empty() && identity.substr(0, identity.find('.')) == excludeContentSha256) {
      continue;
    }
    std::string row = e.dump_identity_key;
    for (const std::string* field : { &e.loading_save_key, &e.loading_fault_place, &e.loading_attempt_key }) {
      row += '\x1f';
      row += *field;
    }
    row += '\x1f';
    row += std::to_string(e.capture_unix);
    sum += skydiag::hash::Fnv1a64(row);  // wraps; the order of the rows does not matter
  }
  char hex[17]{};
  std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(sum));
  return hex;
}

SaveLoadCrashStats CrashHistory::GetSaveLoadCrashStats(const SaveLoadCrashQuery& query) const
{
  SaveLoadCrashStats result{};
  if (query.save_key.empty()) {
    return result;
  }
  // Without capture times, history order decides: entries after this dump's
  // own entry are later.
  const auto dumpKey = NormalizeStoredCandidateKey(query.dump_file);
  std::size_t own = m_entries.size();
  if (!dumpKey.empty() || !query.dump_identity_key.empty()) {
    for (std::size_t i = 0; i < m_entries.size(); ++i) {
      if (IsEntryForDump(m_entries[i], dumpKey, query.dump_identity_key)) {
        own = i;
        break;
      }
    }
  }
  // The same bytes are one incident: a copy or a renamed dump, or this dump.
  const auto contentOf = [](const CrashHistoryEntry& e) {
    return std::string_view(e.dump_identity_key).substr(0, std::string_view(e.dump_identity_key).find('.'));
  };
  // Resolve header times before filtering any rows: a reanalyzed copy can
  // date a legacy row even when the copy comes later in the history.
  std::unordered_map<std::string_view, std::uint64_t> captureByContent;
  for (const auto& entry : m_entries) {
    const auto content = contentOf(entry);
    if (!content.empty() && entry.capture_unix != 0u) {
      captureByContent.emplace(content, entry.capture_unix);
    }
  }
  const auto captureOf = [&](const CrashHistoryEntry& e) {
    if (e.capture_unix != 0u) {
      return e.capture_unix;
    }
    const auto it = captureByContent.find(contentOf(e));
    return it != captureByContent.end() ? it->second : std::uint64_t{0};
  };
  const auto isEarlier = [&](std::size_t i) {
    const auto captured = captureOf(m_entries[i]);
    if (query.capture_unix != 0u && captured != 0u) {
      return captured < query.capture_unix;
    }
    return i < own;
  };
  // Not earlier failures: copies of this dump (same bytes), and captures of
  // the same load attempt as this one (a freeze captured while the save was
  // loading, before the crash that ended the same load).
  std::unordered_set<std::string> seen;
  if (!query.content_sha256.empty()) {
    seen.insert(query.content_sha256);
  }
  if (!query.attempt_key.empty()) {
    for (const auto& e : m_entries) {
      if (e.loading_attempt_key == query.attempt_key && !contentOf(e).empty()) {
        seen.insert(std::string(contentOf(e)));
      }
    }
  }
  const auto firstTime = [](std::string_view content, std::unordered_set<std::string>& contents) {
    return content.empty() || contents.insert(std::string(content)).second;
  };
  // When an entry failed: its capture time, else its first analysis.
  const auto whenOf = [&](const CrashHistoryEntry& e) -> std::optional<std::int64_t> {
    if (const auto captured = captureOf(e); captured != 0u) {
      return static_cast<std::int64_t>(captured);
    }
    return ParseUtcSeconds(e.timestamp_utc);
  };

  // One incident per dump: copies (same bytes) are one, and a newer analysis
  // of the same bytes can supply a place or a load attempt an older row lacks
  // (a legacy bucket includes callers and cannot identify a failure place).
  struct Incident
  {
    std::size_t row = 0;
    std::optional<std::int64_t> when;
    std::string place;
    std::string attempt;
  };
  std::vector<Incident> incidents;
  std::unordered_map<std::string, std::size_t> incidentByContent;
  for (std::size_t i = 0; i < m_entries.size(); ++i) {
    const auto& e = m_entries[i];
    if (e.loading_save_key != query.save_key || !isEarlier(i)) {
      continue;
    }
    if (!query.attempt_key.empty() && e.loading_attempt_key == query.attempt_key) {
      continue;
    }
    const auto content = contentOf(e);
    if (!content.empty() && seen.contains(std::string(content))) {
      continue;
    }
    std::size_t slot = incidents.size();
    if (!content.empty()) {
      slot = incidentByContent.emplace(std::string(content), incidents.size()).first->second;
    }
    if (slot == incidents.size()) {
      incidents.push_back(Incident{ i, std::nullopt, {}, {} });
    }
    auto& incident = incidents[slot];
    if (incident.place.empty()) {
      incident.place = e.loading_fault_place;
    }
    if (incident.attempt.empty()) {
      incident.attempt = e.loading_attempt_key;
    }
    if (const auto when = whenOf(e); when.has_value() && (!incident.when.has_value() || when.value() < incident.when.value())) {
      incident.when = when;
    }
  }

  // One failure per load attempt: a freeze captured during the load and the
  // crash that ended it share an attempt key. Its place is the crash's when
  // there is one; the freeze only shows where the load waited first. Entries
  // from before attempt keys are one attempt each.
  struct Attempt
  {
    std::string place;
    bool crashPlace = false;
  };
  std::vector<Attempt> attempts;
  std::unordered_map<std::string, std::size_t> attemptByKey;
  std::size_t first = m_entries.size();
  std::optional<std::int64_t> firstWhen;
  for (const auto& incident : incidents) {
    std::size_t slot = attempts.size();
    if (!incident.attempt.empty()) {
      slot = attemptByKey.emplace(incident.attempt, attempts.size()).first->second;
    }
    if (slot == attempts.size()) {
      attempts.push_back(Attempt{});
    }
    auto& attempt = attempts[slot];
    const bool crashPlace = !incident.place.empty() && !incident.place.starts_with("freeze|");
    if (!incident.place.empty() && (attempt.place.empty() || (crashPlace && !attempt.crashPlace))) {
      attempt.place = incident.place;
      attempt.crashPlace = crashPlace;
    }
    if (incident.row < first) {
      first = incident.row;
    }
    if (incident.when.has_value() && (!firstWhen.has_value() || incident.when.value() < firstWhen.value())) {
      firstWhen = incident.when;
    }
  }
  result.count = attempts.size();
  if (result.count == 0) {
    return result;
  }
  std::size_t located = 0;
  for (const auto& attempt : attempts) {
    if (attempt.place.empty()) {
      continue;
    }
    ++located;
    if (std::find(result.places.begin(), result.places.end(), attempt.place) == result.places.end()) {
      result.places.push_back(attempt.place);
    }
  }
  result.all_places_known = located == result.count && !query.place.empty();

  // Other saves failing before this capture: after the first of these, or in
  // the day before it (the player may have tried them first).
  constexpr std::int64_t kDaySeconds = std::int64_t{ 24 } * 60 * 60;
  std::unordered_set<std::string> otherSeen = seen;
  std::unordered_set<std::string> otherAttempts;
  for (std::size_t i = 0; i < m_entries.size(); ++i) {
    const auto& e = m_entries[i];
    if (e.loading_save_key.empty() || e.loading_save_key == query.save_key || !isEarlier(i)) {
      continue;
    }
    bool counts = i > first;
    if (const auto when = whenOf(e); when.has_value() && firstWhen.has_value()) {
      counts = when.value() >= firstWhen.value() - kDaySeconds;
    }
    if (counts && firstTime(contentOf(e), otherSeen) &&
        (e.loading_attempt_key.empty() || otherAttempts.insert(e.loading_attempt_key).second)) {
      result.other_save_count += 1;
    }
  }

  if (!query.place.empty() && std::find(result.places.begin(), result.places.end(), query.place) == result.places.end()) {
    result.places.push_back(query.place);
  }
  return result;
}

}  // namespace skydiag::dump_tool
