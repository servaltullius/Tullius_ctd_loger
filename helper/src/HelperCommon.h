#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace skydiag::helper {
struct HelperConfig;
}

namespace skydiag::helper::internal {

std::wstring Timestamp();
// Creates the folder and checks that a file can be written in it; err says why not.
bool PrepareWritableOutputDir(const std::filesystem::path& dir, std::wstring* err);
// cfg.outputDir, or the first writable cfg.outputDirFallbacks entry when it
// cannot be written (logged as the first line of the helper log session).
// The configured OutputDir as the helper uses it, without checking it
// (MakeOutputBase checks it and may pick a fallback).
std::filesystem::path ConfiguredOutputBase(const skydiag::helper::HelperConfig& cfg);
std::filesystem::path MakeOutputBase(const skydiag::helper::HelperConfig& cfg);

bool WriteTextFileUtf8(const std::filesystem::path& path, const std::string& s);
// j as indented text for a file, invalid UTF-8 replaced: dump()'s default
// throws on it, which would cost the capture the file it writes.
std::string DumpJsonForFile(const nlohmann::json& j);
bool ReadTextFileUtf8(const std::filesystem::path& path, std::string* out);

std::string TrimAscii(std::string_view s);
std::string LowerAscii(std::string_view s);

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b);
bool IsUnknownModuleField(std::string_view modulePlusOffset);

std::string WideToUtf8(std::wstring_view s);

// True when the helper runs under Wine/Proton (ntdll exports wine_get_version).
bool IsRunningUnderWine();

struct CrashSummaryInfo
{
  std::uint32_t schemaVersion = 1;
  std::string bucketKey;
  bool unknownFaultModule = true;
  bool candidateConflict = false;
  bool referenceClueOnly = false;
  bool stackwalkDegraded = false;
  bool symbolRuntimeDegraded = false;
  bool firstChanceCandidateWeak = false;
  // ADR-0022: an error while the game was being closed, not a CTD.
  bool forcedClose = false;
};

std::filesystem::path SummaryPathForDump(const std::wstring& dumpPath, const std::filesystem::path& outBase);

bool TryLoadCrashSummaryInfo(const std::filesystem::path& summaryPath, CrashSummaryInfo* out, std::wstring* err);

bool UpdateCrashBucketStats(
  const std::filesystem::path& outBase,
  const CrashSummaryInfo& info,
  std::uint32_t* outUnknownStreak,
  std::uint32_t* outBucketSeenCount,
  std::wstring* err);

}  // namespace skydiag::helper::internal
