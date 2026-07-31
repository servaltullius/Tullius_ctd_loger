#include "HelperCommon.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "SkyrimDiagHelper/Config.h"

namespace skydiag::helper::internal {
namespace {

constexpr std::uint32_t kMinSupportedSummarySchemaVersion = 1;
constexpr std::uint32_t kMaxSupportedSummarySchemaVersion = 2;

std::filesystem::path CrashBucketStatsPath(const std::filesystem::path& outBase)
{
  return outBase / L"SkyrimDiag_CrashBucketStats.json";
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

std::filesystem::path MakeOutputBase(const skydiag::helper::HelperConfig& cfg)
{
  std::filesystem::path out(cfg.outputDir);
  std::error_code ec;
  std::filesystem::create_directories(out, ec);
  return out;
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
    DeleteFileW(tempPath.c_str());
  }
  return ok;
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
    info.schemaVersion = schema.value("version", info.schemaVersion);
  }
  if (info.schemaVersion < kMinSupportedSummarySchemaVersion || info.schemaVersion > kMaxSupportedSummarySchemaVersion) {
    if (err) {
      *err = L"unsupported summary schema version: " + std::to_wstring(info.schemaVersion);
    }
    return false;
  }

  info.bucketKey = TrimAscii(root.value("crash_bucket_key", std::string{}));

  std::optional<bool> unknownFromField;
  if (root.contains("exception") && root["exception"].is_object()) {
    const auto& exceptionObj = root["exception"];
    if (exceptionObj.contains("fault_module_unknown") && exceptionObj["fault_module_unknown"].is_boolean()) {
      unknownFromField = exceptionObj["fault_module_unknown"].get<bool>();
    }
    if (!unknownFromField.has_value()) {
      const std::string modulePlusOffset = exceptionObj.value("module_plus_offset", std::string{});
      unknownFromField = IsUnknownModuleField(modulePlusOffset);
    }
  }
  info.unknownFaultModule = unknownFromField.value_or(true);

  if (root.contains("actionable_candidates") && root["actionable_candidates"].is_array() &&
      !root["actionable_candidates"].empty()) {
    const auto& first = root["actionable_candidates"].front();
    if (first.is_object()) {
      const auto statusId = TrimAscii(first.value("status_id", std::string{}));
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
    const auto repeatedSignatureCount = firstChance.value("repeated_signature_count", 0u);
    const auto loadingWindowCount = firstChance.value("loading_window_count", 0u);
    const bool hasStrongFirstChance = repeatedSignatureCount > 0u || loadingWindowCount > 0u;
    bool candidateWeak = true;
    if (root.contains("actionable_candidates") && root["actionable_candidates"].is_array() &&
        !root["actionable_candidates"].empty()) {
      const auto& first = root["actionable_candidates"].front();
      if (first.is_object()) {
        const auto statusId = TrimAscii(first.value("status_id", std::string{}));
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
  const bool statsExist = std::filesystem::exists(path, existsEc);
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

  std::uint32_t seenTotal = bucket.value("seen_total", 0u);
  std::uint32_t unknownTotal = bucket.value("unknown_total", 0u);
  std::uint32_t unknownStreak = bucket.value("unknown_streak", 0u);

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

  if (!WriteTextFileUtf8(path, root.dump(2))) {
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
