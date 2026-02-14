#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace skydiag::dump_tool {

struct CrashHistoryEntry
{
  std::string timestamp_utc;
  std::string dump_file;
  std::string bucket_key;
  std::string top_suspect;
  std::string confidence;
  std::string signature_id;
  std::vector<std::string> all_suspects;
};

struct ModuleStats
{
  std::string module_name;
  std::size_t total_appearances = 0;
  std::size_t as_top_suspect = 0;
  std::size_t total_crashes = 0;
};

class CrashHistory
{
public:
  static constexpr std::size_t kMaxEntries = 100;

  bool LoadFromFile(const std::filesystem::path& path);
  bool SaveToFile(const std::filesystem::path& path) const;

  void AddEntry(CrashHistoryEntry entry);
  std::vector<ModuleStats> GetModuleStats(std::size_t lastN = 0) const;

  std::size_t Size() const { return m_entries.size(); }

private:
  std::vector<CrashHistoryEntry> m_entries;
};

}  // namespace skydiag::dump_tool
