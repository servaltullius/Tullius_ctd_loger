#include "HelperCommon.h"

#include <Windows.h>

#include <algorithm>
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

void WriteTextFileUtf8(const std::filesystem::path& path, const std::string& s)
{
  std::ofstream f(path, std::ios::binary);
  f.write(s.data(), static_cast<std::streamsize>(s.size()));
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
  std::wstring* err)
{
  if (outUnknownStreak) {
    *outUnknownStreak = 0;
  }
  if (info.bucketKey.empty()) {
    if (err) {
      *err = L"missing crash bucket key";
    }
    return false;
  }

  const auto path = CrashBucketStatsPath(outBase);

  nlohmann::json root = nlohmann::json::object();
  if (std::filesystem::exists(path)) {
    std::string txt;
    if (ReadTextFileUtf8(path, &txt)) {
      const auto parsed = nlohmann::json::parse(txt, nullptr, false);
      if (!parsed.is_discarded() && parsed.is_object()) {
        root = parsed;
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

  if (outUnknownStreak) {
    *outUnknownStreak = unknownStreak;
  }

  WriteTextFileUtf8(path, root.dump(2));
  if (err) {
    err->clear();
  }
  return true;
}

}  // namespace skydiag::helper::internal
