#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace skydiag::helper {

// path::string() converts through the ANSI code page: it substitutes '?' for
// characters it cannot represent and can throw when that code page is UTF-8.
// path::u8string() throws on an unpaired UTF-16 surrogate, which an NTFS name
// can hold. The output folder can hold any file a user drops there, so encode
// UTF-8 by hand with U+FFFD for a broken surrogate; this never throws. The
// names retention manages are all ASCII.
inline std::string PathPartUtf8(const std::filesystem::path& part)
{
  const auto& native = part.native();
  std::string out;
  out.reserve(native.size());
  if constexpr (sizeof(std::filesystem::path::value_type) == 1) {
    for (const auto c : native) {
      out.push_back(static_cast<char>(c));
    }
  } else {
    for (std::size_t i = 0; i < native.size(); ++i) {
      std::uint32_t cp = static_cast<std::uint16_t>(native[i]);
      if (cp >= 0xD800u && cp <= 0xDBFFu && i + 1u < native.size()) {
        const std::uint32_t low = static_cast<std::uint16_t>(native[i + 1u]);
        if (low >= 0xDC00u && low <= 0xDFFFu) {
          cp = 0x10000u + ((cp - 0xD800u) << 10u) + (low - 0xDC00u);
          ++i;
        } else {
          cp = 0xFFFDu;
        }
      } else if (cp >= 0xD800u && cp <= 0xDFFFu) {
        cp = 0xFFFDu;
      }
      if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
      } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6u)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
      } else if (cp < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12u)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
      } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18u)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
      }
    }
  }
  return out;
}

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
    const auto name = PathPartUtf8(ent.path().filename());
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
    const auto name = PathPartUtf8(p.filename());
    if (!StartsWith(name, prefix) || !EndsWith(name, ext)) {
      continue;
    }

    const std::string stem = PathPartUtf8(p.stem());
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
    return PathPartUtf8(a.path.filename()) < PathPartUtf8(b.path.filename());
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
    const auto stem = PathPartUtf8(p.stem());

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
    return PathPartUtf8(a.path.filename()) < PathPartUtf8(b.path.filename());
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

// The dump tool keeps an identity-bound copy of every analysis under
// .skydiag-analysis/<sha256>/<size>.<mtime>/ and a lock file per dump under
// .skydiag-locks/<stem>.lock (dump_tool OutputWriter.cpp). Neither went away
// with its dump, so they piled up. A copy is orphaned when no dump left in
// the folder has its size, the first half of the key (size alone can only
// keep too much, never delete a live copy), and a lock when no dump has its
// stem. Anything changed within minAge is kept, so an analysis still being
// written, or one of a dump stored elsewhere, is left alone. Review state in
// .skydiag-triage is user data and is not touched.
inline void PruneOrphanedAnalysisCaches(
  const std::filesystem::path& outBase,
  std::chrono::seconds minAge = std::chrono::hours(24))
{
  std::unordered_set<std::uint64_t> dumpSizes;
  std::unordered_set<std::string> dumpStems;
  for (const auto& file : CollectRegularFiles(outBase)) {
    if (!EndsWith(PathPartUtf8(file.filename()), ".dmp")) {
      continue;
    }
    std::error_code sizeEc;
    const auto size = std::filesystem::file_size(file, sizeEc);
    if (!sizeEc) {
      dumpSizes.insert(static_cast<std::uint64_t>(size));
    }
    dumpStems.insert(PathPartUtf8(file.stem()));
  }

  const auto now = std::filesystem::file_time_type::clock::now();
  const auto oldEnough = [&](const std::filesystem::path& path) {
    std::error_code timeEc;
    const auto written = std::filesystem::last_write_time(path, timeEc);
    return !timeEc && now - written >= minAge;
  };
  const auto listDir = [](const std::filesystem::path& dir) {
    std::vector<std::filesystem::directory_entry> entries;
    std::error_code iterEc;
    for (std::filesystem::directory_iterator it(dir, iterEc), end; !iterEc && it != end; it.increment(iterEc)) {
      entries.push_back(*it);
    }
    return entries;
  };

  std::error_code ec;
  for (const auto& identity : listDir(outBase / ".skydiag-analysis")) {
    if (!identity.is_directory(ec)) {
      continue;
    }
    for (const auto& generation : listDir(identity.path())) {
      // "<16 hex size>.<16 hex mtime>"; anything else is not ours.
      const auto key = PathPartUtf8(generation.path().filename());
      std::uint64_t size = 0;
      const auto parsed = std::from_chars(key.data(), key.data() + key.size(), size, 16);
      const bool isKey = key.size() == 33 && parsed.ec == std::errc() && parsed.ptr == key.data() + 16 && key[16] == '.';
      if (!isKey || !generation.is_directory(ec) || dumpSizes.count(size) != 0 || !oldEnough(generation.path())) {
        continue;
      }
      std::filesystem::remove_all(generation.path(), ec);
    }
    if (std::filesystem::is_empty(identity.path(), ec) && !ec) {
      std::filesystem::remove(identity.path(), ec);
    }
  }

  for (const auto& lock : listDir(outBase / ".skydiag-locks")) {
    const auto name = PathPartUtf8(lock.path().filename());
    if (!EndsWith(name, ".lock") || dumpStems.count(name.substr(0, name.size() - 5)) != 0 ||
        !lock.is_regular_file(ec) || !oldEnough(lock.path())) {
      continue;
    }
    // Fails harmlessly while an analysis holds the lock open.
    std::filesystem::remove(lock.path(), ec);
  }
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

  PruneOrphanedAnalysisCaches(outBase);
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
