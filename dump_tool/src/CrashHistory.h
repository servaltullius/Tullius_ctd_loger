#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace skydiag::dump_tool {

struct CrashHistoryEntry
{
  std::uint32_t candidate_key_version = 2;
  std::string timestamp_utc;
  std::string dump_file;
  std::string dump_identity_key;
  std::string bucket_key;
  std::string top_suspect;
  std::string confidence;
  std::string signature_id;
  std::vector<std::string> all_suspects;
  std::vector<std::string> candidate_keys;
};

struct ModuleStats
{
  std::string module_name;
  std::size_t total_appearances = 0;
  std::size_t as_top_suspect = 0;
  std::size_t total_crashes = 0;
};

struct BucketStats
{
  std::size_t count = 0;
  std::string first_seen;
  std::string last_seen;
};

struct BucketCandidateStats
{
  std::string candidate_key;
  std::size_t count = 0;
};

// Why a history was or was not loaded: a caller must not save over a file it
// could not read, or every earlier entry is replaced by the new one.
enum class CrashHistoryLoad
{
  kLoaded,
  kMissing,             // no file yet
  kUnreadable,          // there, but it could not be read (locked, no access)
  kDamaged,             // not JSON, or not a crash history
  kUnsupportedVersion,  // a version this analyzer does not know (a newer one wrote it)
};

class CrashHistory
{
public:
  static constexpr std::size_t kMaxEntries = 100;

  CrashHistoryLoad Load(const std::filesystem::path& path);
  bool LoadFromFile(const std::filesystem::path& path) { return Load(path) == CrashHistoryLoad::kLoaded; }
  bool SaveToFile(const std::filesystem::path& path) const;

  void AddEntry(CrashHistoryEntry entry);
  std::size_t RemoveEntriesForDumpFile(
    std::string_view dumpFile,
    std::string_view dumpIdentityKey = {});
  std::vector<ModuleStats> GetModuleStats(std::size_t lastN = 0) const;
  BucketStats GetBucketStats(const std::string& bucketKey) const;
  std::vector<BucketCandidateStats> GetBucketCandidateStats(const std::string& bucketKey) const;

  std::size_t Size() const { return m_entries.size(); }

private:
  std::vector<CrashHistoryEntry> m_entries;
};

}  // namespace skydiag::dump_tool
