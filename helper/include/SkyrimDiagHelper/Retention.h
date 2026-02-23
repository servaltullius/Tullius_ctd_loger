#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace skydiag::helper {

struct RetentionLimits {
  // 0 = unlimited (no cleanup).
  std::uint32_t maxCrashDumps = 20;
  std::uint32_t maxHangDumps = 20;
  std::uint32_t maxManualDumps = 20;
  std::uint32_t maxEtwTraces = 5;
};

inline bool StartsWith(std::string_view s, std::string_view prefix)
{
  return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

inline bool EndsWith(std::string_view s, std::string_view suffix)
{
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

inline std::optional<std::string> TryExtractTimestampToken(std::string_view s)
{
  // Search for pattern: YYYYMMDD_HHMMSS (15 chars)
  // Optional precision suffix is supported: YYYYMMDD_HHMMSS_<digits>
  // (e.g., milliseconds as YYYYMMDD_HHMMSS_123).
  auto is_digits = [](std::string_view v) {
    for (const char c : v) {
      const unsigned char uc = static_cast<unsigned char>(c);
      if (!std::isdigit(uc)) {
        return false;
      }
    }
    return true;
  };

  std::optional<std::string> best;
  for (std::size_t i = 0; i + 15 <= s.size(); i++) {
    const std::string_view date = s.substr(i, 8);
    if (!is_digits(date)) {
      continue;
    }
    if (s[i + 8] != '_') {
      continue;
    }
    const std::string_view time = s.substr(i + 9, 6);
    if (!is_digits(time)) {
      continue;
    }
    std::size_t tokenLen = 15;
    const std::size_t suffixSep = i + tokenLen;
    if (suffixSep < s.size() && s[suffixSep] == '_') {
      std::size_t suffixEnd = suffixSep + 1;
      while (suffixEnd < s.size()) {
        const unsigned char uc = static_cast<unsigned char>(s[suffixEnd]);
        if (!std::isdigit(uc)) {
          break;
        }
        suffixEnd++;
      }
      if (suffixEnd > suffixSep + 1) {
        tokenLen = suffixEnd - i;
      }
    }

    best = std::string(s.substr(i, tokenLen));
  }
  return best;
}

inline void DeleteAssociatedDumpToolArtifacts(const std::filesystem::path& dir, const std::string& stem)
{
  // DumpTool writes files like:
  //   <stem>_SkyrimDiagSummary.json
  //   <stem>_SkyrimDiagReport.txt
  //   <stem>_SkyrimDiagBlackbox.jsonl
  std::error_code ec;
  for (const auto& ent : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) {
      break;
    }
    if (!ent.is_regular_file(ec)) {
      continue;
    }
    const auto name = ent.path().filename().string();
    if (StartsWith(name, stem + "_")) {
      std::filesystem::remove(ent.path(), ec);
    }
  }
}

struct DatedFile {
  std::filesystem::path path;
  std::string ts;
};

inline std::vector<std::filesystem::path> CollectRegularFiles(const std::filesystem::path& dir)
{
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  for (const auto& ent : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) {
      break;
    }
    if (!ent.is_regular_file(ec)) {
      continue;
    }
    files.push_back(ent.path());
  }
  return files;
}

inline std::string_view IncidentKindForDumpPrefix(std::string_view dumpPrefix)
{
  if (dumpPrefix == "SkyrimDiag_Crash_") {
    return "Crash";
  }
  if (dumpPrefix == "SkyrimDiag_Hang_") {
    return "Hang";
  }
  if (dumpPrefix == "SkyrimDiag_Manual_") {
    return "Manual";
  }
  return {};
}

inline std::vector<DatedFile> CollectFilesByPrefixAndExt(
  const std::vector<std::filesystem::path>& files,
  std::string_view prefix,
  std::string_view ext)
{
  std::vector<DatedFile> out;
  for (const auto& p : files) {
    const auto name = p.filename().string();
    if (!StartsWith(name, prefix) || !EndsWith(name, ext)) {
      continue;
    }

    const std::string stem = p.stem().string();
    auto tsOpt = TryExtractTimestampToken(stem);
    if (!tsOpt) {
      continue;
    }
    out.push_back(DatedFile{ p, *tsOpt });
  }

  std::sort(out.begin(), out.end(), [](const DatedFile& a, const DatedFile& b) {
    if (a.ts != b.ts) {
      return a.ts > b.ts;
    }
    return a.path.filename().string() < b.path.filename().string();
  });
  return out;
}

inline std::vector<DatedFile> CollectFilesByPrefixAndExt(
  const std::filesystem::path& dir,
  std::string_view prefix,
  std::string_view ext)
{
  const auto files = CollectRegularFiles(dir);
  return CollectFilesByPrefixAndExt(files, prefix, ext);
}

inline std::unordered_map<std::string, std::uint32_t> BuildTimestampRefCounts(const std::vector<DatedFile>& dumps)
{
  std::unordered_map<std::string, std::uint32_t> refs;
  refs.reserve(dumps.size());
  for (const auto& dump : dumps) {
    refs[dump.ts] += 1u;
  }
  return refs;
}

inline void PruneDumpFilesFromCollected(
  const std::filesystem::path& dir,
  const std::vector<DatedFile>& dumps,
  std::string_view dumpPrefix,
  std::uint32_t maxCount,
  bool deleteWctForTimestamp,
  bool deleteManualWctForTimestamp,
  bool deleteEtlForStem)
{
  if (maxCount == 0 || dumps.size() <= static_cast<std::size_t>(maxCount)) {
    return;
  }

  std::error_code ec;
  const std::string_view incidentKind = IncidentKindForDumpPrefix(dumpPrefix);
  auto tsRefs = BuildTimestampRefCounts(dumps);
  for (std::size_t i = maxCount; i < dumps.size(); i++) {
    const auto p = dumps[i].path;
    const auto stem = p.stem().string();

    std::filesystem::remove(p, ec);
    DeleteAssociatedDumpToolArtifacts(dir, stem);

    if (deleteEtlForStem) {
      auto etl = p;
      etl.replace_extension(".etl");
      std::filesystem::remove(etl, ec);
    }

    if (!incidentKind.empty()) {
      auto it = tsRefs.find(dumps[i].ts);
      if (it != tsRefs.end()) {
        if (it->second > 0) {
          --it->second;
        }
        if (it->second == 0) {
          const auto manifest = dir / ("SkyrimDiag_Incident_" + std::string(incidentKind) + "_" + dumps[i].ts + ".json");
          std::filesystem::remove(manifest, ec);
        }
      }
    }

    if (deleteWctForTimestamp) {
      const auto wct = dir / ("SkyrimDiag_WCT_" + dumps[i].ts + ".json");
      std::filesystem::remove(wct, ec);
    }
    if (deleteManualWctForTimestamp) {
      const auto wct = dir / ("SkyrimDiag_WCT_Manual_" + dumps[i].ts + ".json");
      std::filesystem::remove(wct, ec);
    }
  }
}

inline void PruneDumpFiles(
  const std::filesystem::path& dir,
  std::string_view dumpPrefix,
  std::uint32_t maxCount,
  bool deleteWctForTimestamp,
  bool deleteManualWctForTimestamp,
  bool deleteEtlForStem)
{
  const auto dumps = CollectFilesByPrefixAndExt(dir, dumpPrefix, ".dmp");
  PruneDumpFilesFromCollected(
    dir,
    dumps,
    dumpPrefix,
    maxCount,
    deleteWctForTimestamp,
    deleteManualWctForTimestamp,
    deleteEtlForStem);
}

inline void PruneEtwTracesFromCollected(std::vector<DatedFile> etls, std::uint32_t maxCount)
{
  if (maxCount == 0 || etls.size() <= static_cast<std::size_t>(maxCount)) {
    return;
  }
  std::sort(etls.begin(), etls.end(), [](const DatedFile& a, const DatedFile& b) {
    if (a.ts != b.ts) {
      return a.ts > b.ts;
    }
    return a.path.filename().string() < b.path.filename().string();
  });

  std::error_code ec;
  for (std::size_t i = maxCount; i < etls.size(); i++) {
    std::filesystem::remove(etls[i].path, ec);
  }
}

inline void PruneEtwTraces(const std::filesystem::path& dir, std::uint32_t maxCount)
{
  auto etls = CollectFilesByPrefixAndExt(dir, "SkyrimDiag_Hang_", ".etl");
  auto crash = CollectFilesByPrefixAndExt(dir, "SkyrimDiag_Crash_", ".etl");
  etls.insert(etls.end(), crash.begin(), crash.end());
  PruneEtwTracesFromCollected(std::move(etls), maxCount);
}

inline void ApplyRetentionToOutputDir(const std::filesystem::path& outBase, const RetentionLimits& limits)
{
  if (outBase.empty()) {
    return;
  }
  std::error_code ec;
  if (!std::filesystem::is_directory(outBase, ec)) {
    return;
  }

  const auto files = CollectRegularFiles(outBase);
  const auto crashDumps = CollectFilesByPrefixAndExt(files, "SkyrimDiag_Crash_", ".dmp");
  const auto hangDumps = CollectFilesByPrefixAndExt(files, "SkyrimDiag_Hang_", ".dmp");
  const auto manualDumps = CollectFilesByPrefixAndExt(files, "SkyrimDiag_Manual_", ".dmp");

  PruneDumpFilesFromCollected(
    outBase,
    crashDumps,
    "SkyrimDiag_Crash_",
    limits.maxCrashDumps,
    /*deleteWctForTimestamp=*/false,
    /*deleteManualWctForTimestamp=*/false,
    /*deleteEtlForStem=*/true);

  PruneDumpFilesFromCollected(
    outBase,
    hangDumps,
    "SkyrimDiag_Hang_",
    limits.maxHangDumps,
    /*deleteWctForTimestamp=*/true,
    /*deleteManualWctForTimestamp=*/false,
    /*deleteEtlForStem=*/true);

  PruneDumpFilesFromCollected(
    outBase,
    manualDumps,
    "SkyrimDiag_Manual_",
    limits.maxManualDumps,
    /*deleteWctForTimestamp=*/false,
    /*deleteManualWctForTimestamp=*/true,
    /*deleteEtlForStem=*/false);

  auto etls = CollectFilesByPrefixAndExt(files, "SkyrimDiag_Hang_", ".etl");
  auto crashEtls = CollectFilesByPrefixAndExt(files, "SkyrimDiag_Crash_", ".etl");
  etls.insert(etls.end(), crashEtls.begin(), crashEtls.end());
  PruneEtwTracesFromCollected(std::move(etls), limits.maxEtwTraces);
}

inline void RotateLogFileIfNeeded(const std::filesystem::path& path, std::uint64_t maxBytes, std::uint32_t maxFiles)
{
  if (maxBytes == 0 || maxFiles == 0) {
    return;
  }

  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return;
  }

  const auto sz = std::filesystem::file_size(path, ec);
  if (ec || sz <= maxBytes) {
    return;
  }

  // Delete oldest archive.
  {
    auto oldest = path;
    oldest += "." + std::to_string(maxFiles);
    std::filesystem::remove(oldest, ec);
  }

  // Shift: .(n-1) -> .n
  for (std::uint32_t i = maxFiles; i > 1; i--) {
    auto src = path;
    src += "." + std::to_string(i - 1);
    auto dst = path;
    dst += "." + std::to_string(i);
    if (std::filesystem::exists(src, ec)) {
      std::filesystem::remove(dst, ec);
      std::filesystem::rename(src, dst, ec);
    }
  }

  // Current -> .1
  auto dst = path;
  dst += ".1";
  std::filesystem::remove(dst, ec);
  std::filesystem::rename(path, dst, ec);
}

}  // namespace skydiag::helper
