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

#include "SkyrimDiagLongPath.h"

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
  // Crash, freeze and manual dumps together, in GiB.
  std::uint32_t maxTotalDumpGB = 5;
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
  // increment(ec), not the range-for's operator++, which throws when the
  // folder becomes unreadable mid-scan (a removed drive).
  std::error_code iterEc;
  for (std::filesystem::directory_iterator it(dir, iterEc), end; !iterEc && it != end; it.increment(iterEc)) {
    std::error_code ec;
    if (!it->is_regular_file(ec)) {
      continue;
    }
    const auto name = PathPartUtf8(it->path().filename());
    if (StartsWith(name, stem + "_")) {
      std::filesystem::remove(it->path(), ec);
    }
  }
}

// The same from a listing already taken, for a sweep that deletes many
// dumps: one folder scan instead of one per dump.
inline void DeleteAssociatedDumpToolArtifacts(const std::vector<std::filesystem::path>& listing, const std::string& stem)
{
  const auto prefix = stem + "_";
  std::error_code ec;
  for (const auto& file : listing) {
    if (StartsWith(PathPartUtf8(file.filename()), prefix)) {
      std::filesystem::remove(file, ec);
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
  std::error_code iterEc;
  for (std::filesystem::directory_iterator it(dir, iterEc), end; !iterEc && it != end; it.increment(iterEc)) {
    std::error_code ec;
    if (!it->is_regular_file(ec)) {
      continue;
    }
    files.push_back(it->path());
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

// Removes one dump and what was written for it. tsRefs counts the dumps of
// its kind per timestamp; the incident manifest goes with the last of them.
// A dump that cannot be deleted (open in an analysis) keeps its reports, so
// it is not left without them; a later sweep tries again. listing, when
// given, is the folder as the sweep found it.
inline void RemoveDumpAndArtifacts(
  const std::filesystem::path& dir,
  const DatedFile& dump,
  std::string_view dumpPrefix,
  std::unordered_map<std::string, std::uint32_t>& tsRefs,
  bool deleteWctForTimestamp,
  bool deleteManualWctForTimestamp,
  bool deleteEtlForStem,
  const std::vector<std::filesystem::path>* listing = nullptr)
{
  std::error_code ec;
  const std::string_view incidentKind = IncidentKindForDumpPrefix(dumpPrefix);
  const auto& p = dump.path;
  const auto stem = PathPartUtf8(p.stem());

  std::filesystem::remove(p, ec);
  if (ec) {
    return;
  }
  if (listing) {
    DeleteAssociatedDumpToolArtifacts(*listing, stem);
  } else {
    DeleteAssociatedDumpToolArtifacts(dir, stem);
  }

  if (deleteEtlForStem) {
    auto etl = p;
    etl.replace_extension(".etl");
    std::filesystem::remove(etl, ec);
  }

  if (!incidentKind.empty()) {
    auto it = tsRefs.find(dump.ts);
    if (it != tsRefs.end()) {
      if (it->second > 0) {
        --it->second;
      }
      if (it->second == 0) {
        const auto manifest = dir / ("SkyrimDiag_Incident_" + std::string(incidentKind) + "_" + dump.ts + ".json");
        std::filesystem::remove(manifest, ec);
        if (incidentKind == "Crash") {
          // A recapture dump (SkyrimDiag_Crash_<ts>_Richer.dmp) writes its
          // own manifest under the same timestamp.
          std::filesystem::remove(dir / ("SkyrimDiag_Incident_CrashRecapture_" + dump.ts + ".json"), ec);
        }
      }
    }
  }

  if (deleteWctForTimestamp) {
    const auto wct = dir / ("SkyrimDiag_WCT_" + dump.ts + ".json");
    std::filesystem::remove(wct, ec);
  }
  if (deleteManualWctForTimestamp) {
    const auto wct = dir / ("SkyrimDiag_WCT_Manual_" + dump.ts + ".json");
    std::filesystem::remove(wct, ec);
  }
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

  auto tsRefs = BuildTimestampRefCounts(dumps);
  for (std::size_t i = maxCount; i < dumps.size(); i++) {
    RemoveDumpAndArtifacts(
      dir, dumps[i], dumpPrefix, tsRefs, deleteWctForTimestamp, deleteManualWctForTimestamp, deleteEtlForStem);
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

// Each dump kind and the files that go with one of its dumps.
struct DumpKindFiles {
  std::string_view prefix;
  bool deleteWctForTimestamp;
  bool deleteManualWctForTimestamp;
  bool deleteEtlForStem;
};

inline constexpr DumpKindFiles kCrashDumpFiles{ "SkyrimDiag_Crash_", false, false, true };
inline constexpr DumpKindFiles kHangDumpFiles{ "SkyrimDiag_Hang_", true, false, true };
inline constexpr DumpKindFiles kManualDumpFiles{ "SkyrimDiag_Manual_", false, true, false };

// The count limits bound each kind, but a dump of a large load order is
// 0.5-0.9 GB (it holds the code of every loaded module), so the default
// counts alone allow about 40 GB. Past maxTotalBytes the oldest dumps of any
// kind go, with what was written for them: newest first, once one does not
// fit, it and every older one go. Two captures always stay, with every dump
// of their timestamp (a crash recapture, _Full or _Richer, shares its
// crash's):
// - the latest capture, so the folder is never emptied;
// - the newest capture of each kind while it is younger than recentAge, which
//   a viewer may still open after the game exits.
// A newest-of-its-kind that is older (a manual capture from months ago) does
// not hold budget forever. 0 = unlimited.
inline void PruneDumpsOverTotalBytes(
  const std::filesystem::path& dir,
  std::uint64_t maxTotalBytes,
  std::chrono::seconds recentAge = std::chrono::hours(24))
{
  if (maxTotalBytes == 0) {
    return;
  }

  struct SizedDump {
    DatedFile dump;
    std::size_t kind = 0;
    std::uint64_t bytes = 0;
  };
  const DumpKindFiles* kinds[] = { &kCrashDumpFiles, &kHangDumpFiles, &kManualDumpFiles };
  std::unordered_map<std::string, std::uint32_t> tsRefs[std::size(kinds)];
  std::string newestOfKind[std::size(kinds)];
  std::vector<SizedDump> all;
  const auto files = CollectRegularFiles(dir);
  for (std::size_t k = 0; k < std::size(kinds); ++k) {
    const auto dumps = CollectFilesByPrefixAndExt(files, kinds[k]->prefix, ".dmp");
    tsRefs[k] = BuildTimestampRefCounts(dumps);
    if (!dumps.empty()) {
      newestOfKind[k] = dumps.front().ts;
    }
    for (const auto& dump : dumps) {
      std::error_code sizeEc;
      const auto size = std::filesystem::file_size(dump.path, sizeEc);
      all.push_back(SizedDump{ dump, k, sizeEc ? 0u : static_cast<std::uint64_t>(size) });
    }
  }
  if (all.empty()) {
    return;
  }
  std::sort(all.begin(), all.end(), [](const SizedDump& a, const SizedDump& b) {
    if (a.dump.ts != b.dump.ts) {
      return a.dump.ts > b.dump.ts;
    }
    return PathPartUtf8(a.dump.path.filename()) < PathPartUtf8(b.dump.path.filename());
  });

  const auto latestTs = all.front().dump.ts;
  const auto now = std::filesystem::file_time_type::clock::now();
  const auto alwaysKept = [&](const SizedDump& entry) {
    if (entry.dump.ts != newestOfKind[entry.kind]) {
      return false;
    }
    if (entry.dump.ts == latestTs) {
      return true;
    }
    std::error_code timeEc;
    const auto written = std::filesystem::last_write_time(entry.dump.path, timeEc);
    return timeEc || now - written < recentAge;
  };

  std::uint64_t total = 0;
  std::vector<const SizedDump*> rest;
  for (const auto& entry : all) {
    if (alwaysKept(entry)) {
      total += entry.bytes;
    } else {
      rest.push_back(&entry);
    }
  }
  bool full = false;
  for (const auto* entry : rest) {
    if (!full && total + entry->bytes <= maxTotalBytes) {
      total += entry->bytes;
      continue;
    }
    full = true;
    const auto& kind = *kinds[entry->kind];
    RemoveDumpAndArtifacts(
      dir,
      entry->dump,
      kind.prefix,
      tsRefs[entry->kind],
      kind.deleteWctForTimestamp,
      kind.deleteManualWctForTimestamp,
      kind.deleteEtlForStem,
      &files);
  }
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
  const auto oldEnough = [&](const DirectoryEntryForIo& entry) {
    return entry.lastWrite.has_value() && now - *entry.lastWrite >= minAge;
  };

  // A copy's files lie up to about 165 characters below outBase with a
  // leftover temporary name, past MAX_PATH for a long output folder, where
  // listing, times and remove_all go through ListDirectoryForIo and
  // RemoveAllForIo (SkyrimDiagLongPath.h).
  const auto cacheBase = PathForIo(outBase, 168);
  std::error_code ec;
  for (const auto& identity : ListDirectoryForIo(cacheBase / ".skydiag-analysis", ec)) {
    if (!identity.directory) {
      continue;
    }
    std::error_code listEc;
    for (const auto& generation : ListDirectoryForIo(identity.path, listEc)) {
      // "<16 hex size>.<16 hex mtime>"; anything else is not ours.
      const auto key = PathPartUtf8(generation.path.filename());
      std::uint64_t size = 0;
      const auto parsed = std::from_chars(key.data(), key.data() + key.size(), size, 16);
      const bool isKey = key.size() == 33 && parsed.ec == std::errc() && parsed.ptr == key.data() + 16 && key[16] == '.';
      if (!isKey || !generation.directory || dumpSizes.count(size) != 0 || !oldEnough(generation)) {
        continue;
      }
      RemoveAllForIo(generation.path, ec);
    }
    if (ListDirectoryForIo(identity.path, listEc).empty() && !listEc) {
      std::filesystem::remove(identity.path, ec);
    }
  }

  for (const auto& lock : ListDirectoryForIo(cacheBase / ".skydiag-locks", ec)) {
    const auto name = PathPartUtf8(lock.path.filename());
    if (!EndsWith(name, ".lock") || dumpStems.count(name.substr(0, name.size() - 5)) != 0 ||
        lock.directory || !oldEnough(lock)) {
      continue;
    }
    // Fails harmlessly while an analysis holds the lock open.
    std::filesystem::remove(lock.path, ec);
  }
}

// Files a helper leaves behind when it stops (or is killed) in the middle of
// something:
// - SkyrimDiag_Released_<ts>.dmp, the dump of a released crash held until
//   the release is settled. A helper holds one at a time, its newest, so the
//   newest is kept for a day and every older one goes.
// - SkyrimDiag_Crash_<ts>.dmp.full.tmp, a full-size rewrite of a crash dump
//   that did not finish. Kept for a while so a rewrite in progress is never
//   touched.
// - SkyrimDiag_CrashRearm_<...>.marker, a crash generation signaled again for
//   a restarted helper (HelperMain RearmFrozenCrashFromEarlierHelper),
//   deleted when its game exits. A day old, that game is long gone.
inline void PruneHelperLeftovers(
  const std::vector<std::filesystem::path>& files,
  std::chrono::seconds heldMaxAge = std::chrono::hours(24),
  std::chrono::seconds partialMinAge = std::chrono::minutes(10))
{
  const auto now = std::filesystem::file_time_type::clock::now();
  const auto olderThan = [&](const std::filesystem::path& path, std::chrono::seconds age) {
    std::error_code timeEc;
    const auto written = std::filesystem::last_write_time(path, timeEc);
    return !timeEc && now - written >= age;
  };

  std::error_code ec;
  const auto released = CollectFilesByPrefixAndExt(files, "SkyrimDiag_Released_", ".dmp");
  for (std::size_t i = 0; i < released.size(); ++i) {
    if (i > 0 || olderThan(released[i].path, heldMaxAge)) {
      std::filesystem::remove(released[i].path, ec);
    }
  }
  for (const auto& file : files) {
    const auto name = PathPartUtf8(file.filename());
    const bool partialDump = StartsWith(name, "SkyrimDiag_Crash_") && EndsWith(name, ".dmp.full.tmp") &&
      olderThan(file, partialMinAge);
    const bool staleMarker = StartsWith(name, "SkyrimDiag_CrashRearm_") && EndsWith(name, ".marker") &&
      olderThan(file, heldMaxAge);
    if (partialDump || staleMarker) {
      std::filesystem::remove(file, ec);
    }
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
  const auto pruneByCount = [&](const DumpKindFiles& kind, std::uint32_t maxCount) {
    PruneDumpFilesFromCollected(
      outBase,
      CollectFilesByPrefixAndExt(files, kind.prefix, ".dmp"),
      kind.prefix,
      maxCount,
      kind.deleteWctForTimestamp,
      kind.deleteManualWctForTimestamp,
      kind.deleteEtlForStem);
  };
  pruneByCount(kCrashDumpFiles, limits.maxCrashDumps);
  pruneByCount(kHangDumpFiles, limits.maxHangDumps);
  pruneByCount(kManualDumpFiles, limits.maxManualDumps);
  PruneDumpsOverTotalBytes(outBase, static_cast<std::uint64_t>(limits.maxTotalDumpGB) << 30u);

  // The steps above deleted dumps with their traces; a trace already gone
  // must not take one of the MaxEtwTraces places.
  const auto remaining = CollectRegularFiles(outBase);
  auto etls = CollectFilesByPrefixAndExt(remaining, "SkyrimDiag_Hang_", ".etl");
  auto crashEtls = CollectFilesByPrefixAndExt(remaining, "SkyrimDiag_Crash_", ".etl");
  etls.insert(etls.end(), crashEtls.begin(), crashEtls.end());
  PruneEtwTracesFromCollected(std::move(etls), limits.maxEtwTraces);

  PruneHelperLeftovers(remaining);
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
