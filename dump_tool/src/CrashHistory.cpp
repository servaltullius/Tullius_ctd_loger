#include "CrashHistory.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace skydiag::dump_tool {
namespace {

std::string CanonicalHistoryKey(std::string_view value)
{
  std::string key;
  key.reserve(value.size());
  for (unsigned char ch : value) {
    const char lower = static_cast<char>(std::tolower(ch));
    if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9')) {
      key.push_back(lower);
    }
  }

  const std::string suffixes[] = { "esp", "esm", "esl", "dll", "exe" };
  for (const auto& suffix : suffixes) {
    if (key.size() > suffix.size() && key.ends_with(suffix)) {
      key.erase(key.size() - suffix.size());
      break;
    }
  }
  return key;
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

    std::vector<CrashHistoryEntry> loaded;
    loaded.reserve(j["entries"].size());

    for (const auto& e : j["entries"]) {
      if (!e.is_object()) {
        continue;
      }
      CrashHistoryEntry row{};
      row.timestamp_utc = e.value("timestamp_utc", "");
      row.dump_file = e.value("dump_file", "");
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
      loaded.push_back(std::move(row));
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
    j["version"] = 1;
    j["entries"] = nlohmann::json::array();
    for (const auto& e : m_entries) {
      j["entries"].push_back({
        { "timestamp_utc", e.timestamp_utc },
        { "dump_file", e.dump_file },
        { "bucket_key", e.bucket_key },
        { "top_suspect", e.top_suspect },
        { "confidence", e.confidence },
        { "signature_id", e.signature_id },
        { "all_suspects", e.all_suspects },
        { "candidate_keys", e.candidate_keys },
      });
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto tempPath = path;
    tempPath += L".tmp";

    {
      std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
      if (!out) {
        return false;
      }
      out << j.dump(2);
      if (!out) {
        return false;
      }
    }

    // Best-effort atomic replacement under caller-side lock.
    std::filesystem::remove(path, ec);
    ec.clear();
    std::filesystem::rename(tempPath, path, ec);
    if (ec) {
      std::error_code cleanupEc;
      std::filesystem::remove(tempPath, cleanupEc);
      return false;
    }
    return true;
  } catch (...) {
    return false;
  }
}

void CrashHistory::AddEntry(CrashHistoryEntry entry)
{
  m_entries.push_back(std::move(entry));
  while (m_entries.size() > kMaxEntries) {
    m_entries.erase(m_entries.begin());
  }
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
    if (!e.candidate_keys.empty()) {
      for (const auto& rawKey : e.candidate_keys) {
        const auto key = CanonicalHistoryKey(rawKey);
        if (!key.empty() && seenInEntry.insert(key).second) {
          auto& row = byKey[key];
          row.candidate_key = key;
          row.count += 1;
        }
      }
      continue;
    }

    const auto fallbackKey = CanonicalHistoryKey(e.top_suspect);
    if (!fallbackKey.empty()) {
      auto& row = byKey[fallbackKey];
      row.candidate_key = fallbackKey;
      row.count += 1;
    }
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
