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

struct CrashLoggerFrameSignals {
  std::string direct_fault_module;
  std::string first_actionable_probable_module;
  std::string probable_streak_module;
  std::uint32_t probable_streak_length = 0;
  std::vector<std::string> probable_modules_in_order;
  // Plugins whose probable frames only wrap a game thread's entry
  // (ThreadEntryHooks.h); never the first actionable module or a top module.
  std::vector<std::string> thread_entry_hook_modules;
  std::vector<bool> probable_frame_is_thread_entry_hook;
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

enum class CrashLoggerArtifactKind {
  kUnknown,
  kCrash,
  kThreadDump,
};

inline constexpr std::uint64_t kCrashLoggerFileTimeTicksPerSecond = 10'000'000ull;
inline constexpr std::uint64_t kCrashLoggerPairingMaxDiff =
  120ull * kCrashLoggerFileTimeTicksPerSecond;
inline constexpr std::uint64_t kCrashLoggerPairingAmbiguityWindow =
  2ull * kCrashLoggerFileTimeTicksPerSecond;

constexpr bool IsCrashLoggerPairingDiffAllowed(std::uint64_t diff) noexcept
{
  return diff <= kCrashLoggerPairingMaxDiff;
}

constexpr bool IsCrashLoggerPairingAmbiguous(
  std::uint64_t bestDiff,
  std::uint64_t competitorDiff) noexcept
{
  return competitorDiff >= bestDiff &&
    (competitorDiff - bestDiff) <= kCrashLoggerPairingAmbiguityWindow;
}

bool IsBetterCrashLoggerPairCandidate(
  bool hasBest,
  std::uint64_t candidateDiff,
  CrashLoggerArtifactKind candidateKind,
  bool candidateSameDirectory,
  std::wstring_view candidateStablePath,
  std::uint64_t bestDiff,
  CrashLoggerArtifactKind bestKind,
  bool bestSameDirectory,
  std::wstring_view bestStablePath);

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
CrashLoggerArtifactKind ClassifyCrashLoggerArtifactNameAscii(std::string_view filename);
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

// The exception Crash Logger reports, e.g.
// Unhandled exception "EXCEPTION_BREAKPOINT" at 0x7FFE68312B24 ucrtbase.dll+00C2B24<TAB>int3
struct CrashLoggerExceptionLine {
  std::string type;         // "EXCEPTION_BREAKPOINT"
  std::string location;     // "ucrtbase.dll+00C2B24"
  std::string instruction;  // "int3"
};
std::optional<CrashLoggerExceptionLine> ParseCrashLoggerExceptionLineAscii(std::string_view logUtf8);
// The log's exception location ("KERNELBASE.dll+00C483A", the module may be
// path-qualified) names a different module or offset than the dump's fault:
// the time pairing most likely picked another crash's log. False when either
// side has no module+offset, or when the dump faulted inside Crash Logger
// itself (a secondary fault while it reported the original exception, which
// the log then names).
bool CrashLoggerLocationContradictsDumpFault(
  std::wstring_view location,
  std::wstring_view faultModuleFilename,
  std::uint64_t faultModuleOffset);
CrashLoggerFrameSignals ParseCrashLoggerFrameSignalsAscii(std::string_view logUtf8);
std::vector<std::string> ParseCrashLoggerFrameTopModulesAsciiLower(std::string_view logUtf8);
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
// Every object type Crash Logger names under POSSIBLE RELEVANT OBJECTS and
// REGISTERS, and the thrown type under C++ EXCEPTION (std::bad_alloc),
// without the trailing '*', first occurrence order, unique. Unlike
// the refs above this keeps types with no plugin or name (InstalledContent).
std::vector<std::string> ParseCrashLoggerObjectTypesAscii(std::string_view logUtf8);
std::vector<CrashLoggerObjectRef> AggregateCrashLoggerObjectRefs(const std::vector<CrashLoggerObjectRef>& refs);
// POSSIBLE RELEVANT OBJECTS entries with a name or FormID but no plugin (forms
// created at runtime, such as spawned actors), most relevant first, at most 6.
// esp_name is empty.
std::vector<CrashLoggerObjectRef> ParseCrashLoggerUnattributedObjectsAscii(std::string_view logUtf8);
// Cells and worldspaces tell where the player was, not what was being processed.
bool IsLocationObjectTypeAscii(std::string_view objType);

}  // namespace crashlogger_core

using crashlogger_core::LooksLikeCrashLoggerLogTextCore;
using crashlogger_core::ParseCrashLoggerVersionAscii;
using crashlogger_core::ParseCrashLoggerCppExceptionDetailsAscii;
using crashlogger_core::ParseCrashLoggerTopModulesAsciiLower;

}  // namespace skydiag::dump_tool
