#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace skydiag::helper {
struct HelperConfig;
}

namespace skydiag::helper::internal {

std::wstring Timestamp();
std::filesystem::path MakeOutputBase(const skydiag::helper::HelperConfig& cfg);

void WriteTextFileUtf8(const std::filesystem::path& path, const std::string& s);
bool ReadTextFileUtf8(const std::filesystem::path& path, std::string* out);

std::string TrimAscii(std::string_view s);
std::string LowerAscii(std::string_view s);

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b);
bool IsUnknownModuleField(std::string_view modulePlusOffset);

std::string WideToUtf8(std::wstring_view s);

struct CrashSummaryInfo
{
  std::uint32_t schemaVersion = 1;
  std::string bucketKey;
  bool unknownFaultModule = true;
};

std::filesystem::path SummaryPathForDump(const std::wstring& dumpPath, const std::filesystem::path& outBase);

bool TryLoadCrashSummaryInfo(const std::filesystem::path& summaryPath, CrashSummaryInfo* out, std::wstring* err);

bool UpdateCrashBucketStats(
  const std::filesystem::path& outBase,
  const CrashSummaryInfo& info,
  std::uint32_t* outUnknownStreak,
  std::wstring* err);

}  // namespace skydiag::helper::internal
