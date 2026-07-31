#include "CrashHistory.h"

#ifdef _WIN32
#include <Windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
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

}  // namespace

bool CrashHistory::LoadFromFile(const std::filesystem::path& path)
{
  try {
    std::ifstream f(path);
    if (!f.is_open()) {
      return false;
    }
    const auto j = nlohmann::json::parse(f, nullptr, true);
    if (!j.is_object() || !j.contains("entries") || !j["entries"].is_array()) {
      return false;
    }
    const auto historyVersion = j.value("version", 1u);
    if (historyVersion != 1u && historyVersion != 2u && historyVersion != 3u) {
      return false;
    }

    std::vector<CrashHistoryEntry> loaded;
    loaded.reserve(j["entries"].size());

    for (const auto& e : j["entries"]) {
      if (!e.is_object()) {
        continue;
      }
      CrashHistoryEntry row{};
      row.candidate_key_version = e.value("candidate_key_version", historyVersion >= 2u ? 2u : 1u);
      if (row.candidate_key_version != 1u && row.candidate_key_version != 2u) {
        row.candidate_key_version = 1u;
      }
      row.timestamp_utc = e.value("timestamp_utc", "");
      row.dump_file = e.value("dump_file", "");
      row.dump_identity_key = e.value("dump_identity_key", "");
      row.bucket_key = e.value("bucket_key", "");
      row.top_suspect = e.value("top_suspect", "");
      row.confidence = e.value("confidence", "");
      row.signature_id = e.value("signature_id", "");
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
    return true;
  } catch (...) {
    return false;
  }
}

bool CrashHistory::SaveToFile(const std::filesystem::path& path) const
{
  try {
    nlohmann::json j = nlohmann::json::object();
    j["version"] = 3;
    j["entries"] = nlohmann::json::array();
    for (const auto& e : m_entries) {
      j["entries"].push_back({
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
      });
    }

    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent, ec);
      if (ec) {
        return false;
      }
    }
    auto tempPath = path;
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
          path.c_str(),
          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      std::filesystem::remove(tempPath, ec);
      return false;
    }
#else
    std::filesystem::rename(tempPath, path, ec);
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
      if (!dumpIdentityKey.empty() && !row.dump_identity_key.empty()) {
        return row.dump_identity_key == dumpIdentityKey;
      }
      // Legacy rows have no identity. Exclude same-basename legacy evidence
      // conservatively for current-incident correlation, but never merge it
      // into a new identity-bound row during persistence.
      return row.dump_identity_key.empty() &&
        !dumpKey.empty() &&
        NormalizeStoredCandidateKey(row.dump_file) == dumpKey;
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

}  // namespace skydiag::dump_tool
