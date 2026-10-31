#include "HelperCommon.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "HelperLog.h"
#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagLongPath.h"

namespace skydiag::helper::internal {
namespace {

// The longest name the helper writes in the output folder, with the
// separator: SkyrimDiag_PostExitCrashEvidence_<timestamp>_<seq>.json while
// WriteTextFileUtf8 adds .tmp.<pid>.<tick>.<n>. The analysis copies lie
// deeper; Retention.h and the dump tool convert those paths themselves.
constexpr std::size_t kLongestOutputNameChars = 100;

constexpr std::uint32_t kMinSupportedSummarySchemaVersion = 1;
constexpr std::uint32_t kMaxSupportedSummarySchemaVersion = 2;

std::filesystem::path CrashBucketStatsPath(const std::filesystem::path& outBase)
{
  return outBase / L"SkyrimDiag_CrashBucketStats.json";
}

// The unsigned number or string at `key`, or `fallback` when it is missing or
// of another type. nlohmann's value() throws type_error on a wrong type, and
// these files can be edited by hand or come from another version; an
// exception here would end the helper (only wmain catches, and exits).
std::uint32_t JsonUintOr(const nlohmann::json& object, const char* key, std::uint32_t fallback)
{
  if (!object.is_object()) {
    return fallback;
  }
  const auto it = object.find(key);
  if (it == object.end() || !it->is_number_unsigned() || it->get<std::uint64_t>() > UINT32_MAX) {
    return fallback;
  }
  return static_cast<std::uint32_t>(it->get<std::uint64_t>());
}

std::string JsonStringOr(const nlohmann::json& object, const char* key)
{
  if (!object.is_object()) {
    return {};
  }
  const auto it = object.find(key);
  return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

bool JsonArrayContainsString(const nlohmann::json& array, std::string_view needle)
{
  if (!array.is_array() || needle.empty()) {
    return false;
  }
  for (const auto& item : array) {
    if (!item.is_string()) {
      continue;
    }
    const auto value = item.get<std::string>();
    if (value.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::wstring Timestamp()
{
  SYSTEMTIME st{};
  GetLocalTime(&st);

  wchar_t buf[80]{};
  swprintf_s(
    buf,
    L"%04u%02u%02u_%02u%02u%02u_%03u",
    st.wYear,
    st.wMonth,
    st.wDay,
    st.wHour,
    st.wMinute,
    st.wSecond,
    st.wMilliseconds);
  return buf;
}

bool IsRunningUnderWine()
{
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  return ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

bool PrepareWritableOutputDir(const std::filesystem::path& dir, std::wstring* err)
{
  if (err) {
    err->clear();
  }
  if (dir.empty()) {
    if (err) {
      *err = L"empty path";
    }
    return false;
  }
  std::error_code ec;
  // CreateDirectoriesForIo: create_directories fails on a long folder that
  // exists under MO2 (SkyrimDiagLongPath.h).
  CreateDirectoriesForIo(dir, ec);
  if (ec) {
    if (err) {
      *err = L"create_directories failed (error " + std::to_wstring(ec.value()) + L")";
    }
    return false;
  }
  // A folder that exists can still refuse writes (Program Files, a read-only
  // share): create and drop a probe file the way captures write theirs.
  static std::atomic<std::uint32_t> probeSequence{0};
  DWORD lastError = ERROR_SUCCESS;
  for (int attempt = 0; attempt < 4; ++attempt) {
    const auto probe = dir /
      (L".SkyrimDiag_write_probe_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
        std::to_wstring(probeSequence.fetch_add(1, std::memory_order_relaxed)) + L".tmp");
    const HANDLE file = CreateFileW(
      probe.c_str(),
      GENERIC_WRITE,
      0,
      nullptr,
      CREATE_NEW,
      FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE,
      nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      CloseHandle(file);
      return true;
    }
    lastError = GetLastError();
    if (lastError != ERROR_FILE_EXISTS) {
      break;
    }
  }
  if (err) {
    *err = L"folder is not writable (error " + std::to_wstring(lastError) + L")";
  }
  return false;
}

std::filesystem::path MakeOutputBase(const skydiag::helper::HelperConfig& cfg)
{
  // A long output folder is used in its extended-length form, so every path
  // the helper builds under it works (SkyrimDiagLongPath.h); a usual one is
  // left as it is.
  const std::filesystem::path configured = skydiag::PathForIo(cfg.outputDir, kLongestOutputNameChars);
  std::wstring configuredErr;
  if (PrepareWritableOutputDir(configured, &configuredErr)) {
    return configured;
  }
  // Without a writable folder every dump, report and log line of the session
  // is lost without a trace; use the first fallback that works and say so.
  for (const auto& fallback : cfg.outputDirFallbacks) {
    const std::filesystem::path candidate = skydiag::PathForIo(fallback, kLongestOutputNameChars);
    std::wstring fallbackErr;
    if (!PrepareWritableOutputDir(candidate, &fallbackErr)) {
      continue;
    }
    const std::wstring warning =
      L"WARNING: OutputDir \"" + cfg.outputDir + L"\" cannot be used (" + configuredErr +
      L"); captures and logs go to \"" + fallback + L"\" instead. Fix OutputDir in SkyrimDiagHelper.ini.";
    std::wcerr << L"[SkyrimDiagHelper] " << warning << L"\n";
    QueueSessionStartLogLine(warning);
    return candidate;
  }
  std::wcerr << L"[SkyrimDiagHelper] WARNING: OutputDir \"" << cfg.outputDir << L"\" cannot be used ("
             << configuredErr << L") and no fallback folder is writable; captures may be lost.\n";
  return configured;
}

bool WriteTextFileUtf8(const std::filesystem::path& path, const std::string& s)
{
  if (path.empty() || path.filename().empty()) {
    return false;
  }

  // Stateful helper JSON must never expose a truncated destination file. Write
  // and flush a sibling temporary file first, then atomically replace the
  // destination on the same volume.
  static std::atomic<std::uint64_t> tempSequence{0};
  std::filesystem::path tempPath;
  HANDLE file = INVALID_HANDLE_VALUE;
  for (int attempt = 0; attempt < 8 && file == INVALID_HANDLE_VALUE; ++attempt) {
    tempPath = path;
    tempPath +=
      L".tmp."
      + std::to_wstring(GetCurrentProcessId())
      + L"."
      + std::to_wstring(GetTickCount64())
      + L"."
      + std::to_wstring(tempSequence.fetch_add(1, std::memory_order_relaxed));
    file = CreateFileW(
      tempPath.c_str(),
      GENERIC_WRITE,
      0,
      nullptr,
      CREATE_NEW,
      FILE_ATTRIBUTE_TEMPORARY,
      nullptr);
  }
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }

  bool ok = true;
  std::size_t offset = 0;
  while (offset < s.size()) {
    const auto remaining = s.size() - offset;
    const DWORD chunk = static_cast<DWORD>(
      std::min<std::size_t>(remaining, static_cast<std::size_t>(MAXDWORD)));
    DWORD written = 0;
    if (!WriteFile(file, s.data() + offset, chunk, &written, nullptr) ||
        written != chunk) {
      ok = false;
      break;
    }
    offset += written;
  }
  if (ok && !FlushFileBuffers(file)) {
    ok = false;
  }
  if (!CloseHandle(file)) {
    ok = false;
  }

  if (ok) {
    ok = MoveFileExW(
      tempPath.c_str(),
      path.c_str(),
      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
  }
  if (!ok) {
    // remove, not DeleteFileW, which MO2 refuses past MAX_PATH (SkyrimDiagLongPath.h).
    std::error_code removeEc;
    std::filesystem::remove(tempPath, removeEc);
  }
  return ok;
}

std::string DumpJsonForFile(const nlohmann::json& j)
{
  return j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
}

bool ReadTextFileUtf8(const std::filesystem::path& path, std::string* out)
{
  if (out) {
    out->clear();
  }
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  if (out) {
    *out = ss.str();
  }
  return true;
}

std::string TrimAscii(std::string_view s)
{
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
    b++;
  }
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
    e--;
  }
  return std::string(s.substr(b, e - b));
}

std::string LowerAscii(std::string_view s)
{
  std::string out;
  out.reserve(s.size());
  for (const unsigned char c : s) {
    out.push_back(static_cast<char>(std::tolower(c)));
  }
  return out;
}

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b)
{
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); i++) {
    if (std::towlower(a[i]) != std::towlower(b[i])) {
      return false;
    }
  }
  return true;
}

bool IsUnknownModuleField(std::string_view modulePlusOffset)
{
  const std::string lower = LowerAscii(TrimAscii(modulePlusOffset));
  return lower.empty() || lower == "unknown" || lower == "<unknown>" || lower == "n/a" || lower == "none";
}

std::string WideToUtf8(std::wstring_view s)
{
  if (s.empty()) {
    return {};
  }
  const int needed = WideCharToMultiByte(
    CP_UTF8,
    0,
    s.data(),
    static_cast<int>(s.size()),
    nullptr,
    0,
    nullptr,
    nullptr);
  if (needed <= 0) {
    return {};
  }
  std::string out(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(
    CP_UTF8,
    0,
    s.data(),
    static_cast<int>(s.size()),
    out.data(),
    needed,
    nullptr,
    nullptr);
  return out;
}

std::filesystem::path SummaryPathForDump(const std::wstring& dumpPath, const std::filesystem::path& outBase)
{
  const std::filesystem::path dumpFs(dumpPath);
  const std::wstring stem = dumpFs.stem().wstring();
  return outBase / (stem + L"_SkyrimDiagSummary.json");
}

bool TryLoadCrashSummaryInfo(const std::filesystem::path& summaryPath, CrashSummaryInfo* out, std::wstring* err)
{
  if (out) {
    *out = CrashSummaryInfo{};
  }
  std::string txt;
  if (!ReadTextFileUtf8(summaryPath, &txt)) {
    if (err) *err = L"summary not found/readable: " + summaryPath.wstring();
    return false;
  }
  const auto root = nlohmann::json::parse(txt, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    if (err) *err = L"summary json parse failed: " + summaryPath.wstring();
    return false;
  }

  CrashSummaryInfo info{};
  if (root.contains("schema") && root["schema"].is_object()) {
    const auto& schema = root["schema"];
    info.schemaVersion = JsonUintOr(schema, "version", info.schemaVersion);
  }
  if (info.schemaVersion < kMinSupportedSummarySchemaVersion || info.schemaVersion > kMaxSupportedSummarySchemaVersion) {
    if (err) {
      *err = L"unsupported summary schema version: " + std::to_wstring(info.schemaVersion);
    }
    return false;
  }

  info.bucketKey = TrimAscii(JsonStringOr(root, "crash_bucket_key"));
  if (root.contains("analysis") && root["analysis"].is_object()) {
    const auto& analysis = root["analysis"];
    info.forcedClose = analysis.contains("is_forced_close") && analysis["is_forced_close"].is_boolean() &&
      analysis["is_forced_close"].get<bool>();
  }

  std::optional<bool> unknownFromField;
  if (root.contains("exception") && root["exception"].is_object()) {
    const auto& exceptionObj = root["exception"];
    if (exceptionObj.contains("fault_module_unknown") && exceptionObj["fault_module_unknown"].is_boolean()) {
      unknownFromField = exceptionObj["fault_module_unknown"].get<bool>();
    }
    if (!unknownFromField.has_value()) {
      const std::string modulePlusOffset = JsonStringOr(exceptionObj, "module_plus_offset");
      unknownFromField = IsUnknownModuleField(modulePlusOffset);
    }
  }
  info.unknownFaultModule = unknownFromField.value_or(true);

  if (root.contains("actionable_candidates") && root["actionable_candidates"].is_array() &&
      !root["actionable_candidates"].empty()) {
    const auto& first = root["actionable_candidates"].front();
    if (first.is_object()) {
      const auto statusId = TrimAscii(JsonStringOr(first, "status_id"));
      info.candidateConflict = (statusId == "conflicting");
      info.referenceClueOnly = (statusId == "reference_clue");
    }
  }

  if (root.contains("symbolization") && root["symbolization"].is_object()) {
    const auto& symbolization = root["symbolization"];
    if (symbolization.contains("runtime_degraded") && symbolization["runtime_degraded"].is_boolean()) {
      info.symbolRuntimeDegraded = symbolization["runtime_degraded"].get<bool>();
    }
  }

  if (root.contains("diagnostics")) {
    info.stackwalkDegraded = JsonArrayContainsString(
      root["diagnostics"],
      "[Stackwalk] DbgHelp stackwalk failed");
  }

  const auto parseFirstChanceContext = [&root, &info](const nlohmann::json& firstChance) {
    const auto repeatedSignatureCount = JsonUintOr(firstChance, "repeated_signature_count", 0u);
    const auto loadingWindowCount = JsonUintOr(firstChance, "loading_window_count", 0u);
    const bool hasStrongFirstChance = repeatedSignatureCount > 0u || loadingWindowCount > 0u;
    bool candidateWeak = true;
    if (root.contains("actionable_candidates") && root["actionable_candidates"].is_array() &&
        !root["actionable_candidates"].empty()) {
      const auto& first = root["actionable_candidates"].front();
      if (first.is_object()) {
        const auto statusId = TrimAscii(JsonStringOr(first, "status_id"));
        candidateWeak = (statusId == "related" || statusId == "reference_clue");
      }
    }
    info.firstChanceCandidateWeak = hasStrongFirstChance && candidateWeak;
  };

  if (root.contains("first_chance_context") && root["first_chance_context"].is_object()) {
    parseFirstChanceContext(root["first_chance_context"]);
  } else if (root.contains("freeze_analysis") && root["freeze_analysis"].is_object()) {
    const auto& freeze = root["freeze_analysis"];
    if (freeze.contains("first_chance_context") && freeze["first_chance_context"].is_object()) {
      parseFirstChanceContext(freeze["first_chance_context"]);
    }
  }

  if (out) {
    *out = std::move(info);
  }
  if (err) {
    err->clear();
  }
  return true;
}

bool UpdateCrashBucketStats(
  const std::filesystem::path& outBase,
  const CrashSummaryInfo& info,
  std::uint32_t* outUnknownStreak,
  std::uint32_t* outBucketSeenCount,
  std::wstring* err)
{
  if (outUnknownStreak) {
    *outUnknownStreak = 0;
  }
  if (outBucketSeenCount) {
    *outBucketSeenCount = 0;
  }
  if (info.bucketKey.empty()) {
    if (err) {
      *err = L"missing crash bucket key";
    }
    return false;
  }

  const auto path = CrashBucketStatsPath(outBase);

  nlohmann::json root = nlohmann::json::object();
  std::error_code existsEc;
  const bool statsExist = ExistsForIo(path, existsEc);
  if (existsEc) {
    if (err) {
      *err = L"crash bucket stats existence check failed: "
        + std::to_wstring(existsEc.value());
    }
    return false;
  }
  if (statsExist) {
    std::string txt;
    if (!ReadTextFileUtf8(path, &txt)) {
      if (err) {
        *err = L"crash bucket stats read failed";
      }
      return false;
    }
    const auto parsed = nlohmann::json::parse(txt, nullptr, false);
    if (!parsed.is_discarded() && parsed.is_object()) {
      root = parsed;
    } else {
      auto quarantinePath = path;
      quarantinePath += L".corrupt." + Timestamp();
      std::error_code quarantineEc;
      std::filesystem::rename(path, quarantinePath, quarantineEc);
      if (quarantineEc) {
        if (err) {
          *err = L"crash bucket stats parse failed and corrupt file quarantine failed: "
            + std::to_wstring(quarantineEc.value());
        }
        return false;
      }
    }
  }
  if (!root.contains("version")) {
    root["version"] = 1;
  }
  if (!root.contains("buckets") || !root["buckets"].is_object()) {
    root["buckets"] = nlohmann::json::object();
  }

  auto& bucket = root["buckets"][info.bucketKey];
  if (!bucket.is_object()) {
    bucket = nlohmann::json::object();
  }

  std::uint32_t seenTotal = JsonUintOr(bucket, "seen_total", 0u);
  std::uint32_t unknownTotal = JsonUintOr(bucket, "unknown_total", 0u);
  std::uint32_t unknownStreak = JsonUintOr(bucket, "unknown_streak", 0u);

  seenTotal++;
  if (info.unknownFaultModule) {
    unknownTotal++;
    unknownStreak++;
  } else {
    unknownStreak = 0;
  }

  bucket["seen_total"] = seenTotal;
  bucket["unknown_total"] = unknownTotal;
  bucket["unknown_streak"] = unknownStreak;
  bucket["last_unknown_fault_module"] = info.unknownFaultModule;
  bucket["updated_at_epoch"] = static_cast<std::int64_t>(std::time(nullptr));

  // One entry per distinct crash ever seen grew the file without end; the
  // least recently updated go past kMaxCrashBucketStats (never this one).
  constexpr std::size_t kMaxCrashBucketStats = 200;
  auto& buckets = root["buckets"];
  if (buckets.size() > kMaxCrashBucketStats) {
    std::vector<std::pair<std::int64_t, std::string>> byAge;
    byAge.reserve(buckets.size());
    for (auto it = buckets.begin(); it != buckets.end(); ++it) {
      std::int64_t updated = 0;
      if (it->is_object()) {
        const auto time = it->find("updated_at_epoch");
        if (time != it->end() && time->is_number_integer()) {
          updated = time->get<std::int64_t>();
        }
      }
      byAge.emplace_back(updated, it.key());
    }
    std::sort(byAge.begin(), byAge.end());
    std::size_t excess = buckets.size() - kMaxCrashBucketStats;
    for (const auto& [updated, key] : byAge) {
      if (excess == 0u) {
        break;
      }
      if (key != info.bucketKey) {
        buckets.erase(key);
        --excess;
      }
    }
  }

  // A bucket key from a summary is not checked for UTF-8; dump() would throw.
  if (!WriteTextFileUtf8(path, root.dump(2, ' ', false, nlohmann::json::error_handler_t::replace))) {
    if (err) {
      *err = L"crash bucket stats atomic write failed";
    }
    return false;
  }
  if (outUnknownStreak) {
    *outUnknownStreak = unknownStreak;
  }
  if (outBucketSeenCount) {
    *outBucketSeenCount = seenTotal;
  }
  if (err) {
    err->clear();
  }
  return true;
}

}  // namespace skydiag::helper::internal
