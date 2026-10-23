#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace skydiag::dump_tool {
struct DumpIdentity;
}

namespace skydiag::dump_tool::internal::output_writer {

std::wstring JoinList(const std::vector<std::wstring>& items, std::size_t maxN, std::wstring_view sep);

bool ReadTextFileUtf8(const std::filesystem::path& path, std::string* out);

bool WriteTextUtf8(const std::filesystem::path& path, const std::string& content, std::wstring* err);

void LoadExistingSummaryTriage(
  const std::filesystem::path& summaryPath,
  const std::filesystem::path& outBase,
  const DumpIdentity& identity,
  nlohmann::json* triage);

nlohmann::json DumpIdentityJson(const DumpIdentity& identity);

bool DumpIdentityMatchesJson(const nlohmann::json& value, const DumpIdentity& identity);

std::filesystem::path TriageStatePath(
  const std::filesystem::path& outBase,
  const DumpIdentity& identity);

std::filesystem::path IdentityArtifactDirectory(
  const std::filesystem::path& outBase,
  const DumpIdentity& identity);

std::filesystem::path OutputFamilyLockPath(
  const std::filesystem::path& outBase,
  std::wstring_view dumpStem);

bool TriageHasReviewContent(const nlohmann::json& triage);

bool WriteTriageState(
  const std::filesystem::path& outBase,
  const DumpIdentity& identity,
  const nlohmann::json& triage,
  std::wstring* err);

bool IsUnknownModuleField(std::wstring_view modulePlusOffset);

std::wstring MaybeRedactPath(std::wstring_view path, bool redactPaths);

// Redacts every absolute directory in a DbgHelp symbol search path
// ("C:\Sym;srv*C:\Cache*https://..."), keeping separators and URLs.
std::wstring RedactSymbolSearchPath(std::wstring_view searchPath, bool redactPaths);

// Masks every absolute path inside UTF-8 text the way MaybeRedactPath masks
// a path field: "C:\...", "\\?\C:\...", "\\server\share\..." and their
// forward-slash spellings keep only their last element ("<redacted>\x.dll"),
// a user's profile folder becomes "<user>" (also without a drive:
// "/users/<user>/"). URLs, module names and FormIDs are left as they are.
std::string RedactPathsInText(std::string_view text);

// RedactPathsInText over every string in `value` (recursively): a string that
// is a path, and the paths a message or dialog text quotes.
void RedactAbsolutePathStrings(nlohmann::json* value);

// At most maxBytes of UTF-8 text, cut on a code point boundary.
std::string TruncateUtf8(std::string_view text, std::size_t maxBytes);

std::filesystem::path DefaultOutDirForDump(const std::filesystem::path& dumpPath);

std::filesystem::path FindIncidentManifestForDump(const std::filesystem::path& outBase, std::wstring_view dumpStem);

bool TryLoadIncidentManifestJson(const std::filesystem::path& path, nlohmann::json* out);

}  // namespace skydiag::dump_tool::internal::output_writer
