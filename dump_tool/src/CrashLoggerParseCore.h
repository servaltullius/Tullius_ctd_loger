#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skydiag::dump_tool {
namespace crashlogger_core {

struct CrashLoggerCppExceptionDetails {
  std::string type;
  std::string info;
  std::string throw_location;
  std::string module;
};

// Parsed date/time components from a filename pattern.
struct ParsedTimestamp
{
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
};

// ── ESP/ESM object reference parsing ──

struct CrashLoggerObjectRef {
  std::string location;        // "RDI", "RSP+68"
  std::string object_type;     // "Character*", "TESObjectREFR*"
  std::string esp_name;        // "AE_StellarBlade_Doro.esp"
  std::string object_name;     // "도로롱" (UTF-8, may be empty)
  std::string form_id;         // "0xFEAD081B" or empty
  std::uint32_t relevance_score = 0;  // LocationWeight + TypeWeight
};

// Entry with ESP name and optional FormID
struct EspRefEntry {
  std::string esp_name;
  std::string form_id;  // "0xFEAD081B" or empty
};

// ── String utilities ──

std::string AsciiLower(std::string_view s);
std::string_view TrimLeftAscii(std::string_view s);
std::string_view TrimRightAscii(std::string_view s);
std::string_view TrimAscii(std::string_view s);
bool ContainsCaseInsensitiveAscii(std::string_view haystack, std::string_view needle);
bool StartsWithCaseInsensitiveAscii(std::string_view s, std::string_view prefix);
bool EqualsCaseInsensitiveAscii(std::string_view a, std::string_view b);

// ── CrashLogger log parsing ──

std::optional<std::string> ParseCrashLoggerVersionAscii(std::string_view logUtf8);
std::optional<std::string> ParseCrashLoggerIniCrashlogDirectoryAscii(std::string_view iniUtf8);
bool LooksLikeCrashLoggerLogTextCore(std::string_view utf8Prefix);
std::optional<std::string_view> TryExtractModulePlusOffsetTokenAscii(std::string_view line);
std::optional<CrashLoggerCppExceptionDetails> ParseCrashLoggerCppExceptionDetailsAscii(std::string_view logUtf8);
std::vector<std::string> ParseCrashLoggerTopModulesAsciiLower(std::string_view logUtf8);

// ── Module classification ──

bool IsSystemishModuleAsciiLower(std::string_view filenameLower);
bool IsGameExeModuleAsciiLower(std::string_view filenameLower);

// ── Timestamp extraction ──

std::optional<ParsedTimestamp> TryExtractCompactTimestampFromStem(std::wstring_view stem);
std::optional<ParsedTimestamp> TryExtractDashedTimestampFromStem(std::wstring_view stem);

// ── ESP/ESM reference extraction ──

bool IsVanillaDlcEspAsciiLower(std::string_view espLower);
std::uint32_t LocationWeight(std::string_view loc);
std::uint32_t TypeWeight(std::string_view objType);
bool LooksLikePluginExtension(std::string_view s);
std::string ExtractFormIdBefore(std::string_view line, std::size_t pos);
std::vector<EspRefEntry> ExtractEspRefsFromLine(std::string_view line);
std::vector<std::string> ExtractEspNamesFromLine(std::string_view line);
std::string ExtractObjectType(std::string_view line);
std::string ExtractObjectName(std::string_view line);
std::string ExtractLocation(std::string_view trimmed);
std::vector<CrashLoggerObjectRef> ParseCrashLoggerObjectRefsAscii(std::string_view logUtf8);
std::vector<CrashLoggerObjectRef> AggregateCrashLoggerObjectRefs(const std::vector<CrashLoggerObjectRef>& refs);

}  // namespace crashlogger_core

using crashlogger_core::LooksLikeCrashLoggerLogTextCore;
using crashlogger_core::ParseCrashLoggerVersionAscii;
using crashlogger_core::ParseCrashLoggerCppExceptionDetailsAscii;
using crashlogger_core::ParseCrashLoggerTopModulesAsciiLower;

}  // namespace skydiag::dump_tool
