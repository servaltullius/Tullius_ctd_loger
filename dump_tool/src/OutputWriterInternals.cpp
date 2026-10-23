#include "OutputWriterInternals.h"
#include "DumpIdentity.h"
#include "Utf.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

namespace skydiag::dump_tool::internal::output_writer {

bool ReadTextFileUtf8(const std::filesystem::path& path, std::string* out)
{
  if (out) {
    out->clear();
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  if (out) {
    *out = ss.str();
  }
  return true;
}

namespace {

std::atomic<std::uint64_t> g_atomicWriteCounter{ 0u };

nlohmann::json DefaultTriageFields()
{
  return {
    { "review_status", "unreviewed" },
    { "reviewed", false },
    { "verdict", "" },
    { "actual_cause", "" },
    { "ground_truth_cause", "" },
    { "ground_truth_mod", "" },
    { "signature_matched", false },
    { "reviewer", "" },
    { "reviewed_at_utc", "" },
    { "notes", "" },
  };
}

bool LooksLikeAbsolutePath(std::wstring_view path)
{
  if (path.size() >= 3 &&
      ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
      path[1] == L':' &&
      (path[2] == L'\\' || path[2] == L'/')) {
    return true;
  }
  if (path.size() >= 2 &&
      ((path[0] == L'\\' && path[1] == L'\\') || (path[0] == L'/' && path[1] == L'/'))) {
    return true;
  }
  return false;
}

std::wstring RedactPathValue(std::wstring_view path)
{
  const std::filesystem::path p(path);
  const std::wstring filename = p.filename().wstring();
  if (filename.empty()) {
    return L"<redacted>";
  }
  // "C:\Users\<name>" ends with the account name itself.
  if (_wcsicmp(p.parent_path().filename().c_str(), L"Users") == 0) {
    return L"<redacted>\\<user>";
  }
  return L"<redacted>\\" + filename;
}

// --- Paths quoted inside text (RedactPathsInText) ---------------------------
// The text is UTF-8, but every byte the rules look at is ASCII, and the bytes
// of a non-ASCII character are never ASCII.

bool IsAsciiAlpha(char c)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool IsAsciiWordChar(char c)
{
  return IsAsciiAlpha(c) || (c >= '0' && c <= '9') || c == '_';
}

bool IsPathSeparator(char c)
{
  return c == '\\' || c == '/';
}

// Characters a file or folder name cannot hold (':' only after the drive),
// and the ends of a line.
bool EndsPathInText(char c)
{
  return c == '"' || c == '|' || c == '*' || c == '?' || c == ':' || c == '\r' || c == '\n' || c == '\t' ||
    c == '\0';
}

char CharAt(std::string_view text, std::size_t pos)
{
  return pos < text.size() ? text[pos] : '\0';
}

bool EndsWithUsersFolder(std::string_view dir)
{
  constexpr std::string_view kUsers = "users";
  if (dir.size() < kUsers.size() + 2u || !IsPathSeparator(dir.back())) {
    return false;
  }
  const std::size_t start = dir.size() - 1u - kUsers.size();
  if (!IsPathSeparator(dir[start - 1u])) {
    return false;
  }
  for (std::size_t i = 0; i < kUsers.size(); ++i) {
    const char c = dir[start + i];
    if (static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c) != kUsers[i]) {
      return false;
    }
  }
  return true;
}

// End of a "scheme://..." URL starting at `pos` (left as it is), or `pos`.
// file:// URLs name local paths, so they are not skipped.
std::size_t UrlEndAt(std::string_view text, std::size_t pos)
{
  if (!IsAsciiAlpha(text[pos]) || (pos > 0 && IsAsciiWordChar(text[pos - 1]))) {
    return pos;
  }
  std::size_t i = pos;
  while (i < text.size() && (IsAsciiWordChar(text[i]) || text[i] == '+' || text[i] == '-' || text[i] == '.')) {
    ++i;
  }
  if (i - pos < 2u || text.compare(i, 3, "://") != 0) {
    return pos;
  }
  std::string scheme(text.substr(pos, i - pos));
  for (auto& c : scheme) {
    c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  }
  if (scheme == "file") {
    return pos;
  }
  while (i < text.size() && text[i] != ' ' && text[i] != '"' && text[i] != '\r' && text[i] != '\n' &&
         text[i] != '\t') {
    ++i;
  }
  return i;
}

// Length of the root of an absolute path starting at `pos` ("C:\",
// "\\?\C:\", "\\?\", "\\server"), or 0.
std::size_t AbsolutePathRootAt(std::string_view text, std::size_t pos)
{
  const char prev = pos > 0 ? text[pos - 1] : ' ';
  const auto at = [&](std::size_t i) { return CharAt(text, pos + i); };
  if (IsAsciiAlpha(at(0)) && at(1) == ':' && IsPathSeparator(at(2))) {
    // "Exception:\" ends a word, and "x://" is a URL.
    if (IsAsciiWordChar(prev) || (at(2) == '/' && at(3) == '/')) {
      return 0;
    }
    return 3;
  }
  const bool backslashes = at(0) == '\\' && at(1) == '\\';
  const bool slashes = at(0) == '/' && at(1) == '/';
  if (!backslashes && !slashes) {
    return 0;
  }
  // "scheme://" is a URL; "a\\b" is not a share.
  if (prev == ':' || IsPathSeparator(prev) || IsAsciiWordChar(prev)) {
    return 0;
  }
  if (backslashes && (at(2) == '?' || at(2) == '.') && at(3) == '\\') {
    if (IsAsciiAlpha(at(4)) && at(5) == ':' && IsPathSeparator(at(6))) {
      return 7;
    }
    return 4;
  }
  const char first = at(2);
  if (first == ' ' || IsPathSeparator(first) || EndsPathInText(first)) {
    return 0;
  }
  return 2;
}

// End of the absolute path whose root ends at `pos + root`; `*nameStart`
// is where its last element begins. A folder name may hold blanks ("Program
// Files (x86)", "My Games"): the path goes on over a blank only when a few
// words reach another separator, and a separator after a blank (" / ",
// flattened dialog lines) starts new text.
std::size_t AbsolutePathEnd(std::string_view text, std::size_t pos, std::size_t root, std::size_t* nameStart)
{
  constexpr std::size_t kMaxBlanksInFolder = 3;
  constexpr std::size_t kMaxBlankRun = 64;
  std::size_t i = pos + root;
  *nameStart = i;
  while (i < text.size()) {
    const char c = text[i];
    if (EndsPathInText(c)) {
      break;
    }
    if (IsPathSeparator(c)) {
      if (text[i - 1] == ' ') {
        break;
      }
      ++i;
      *nameStart = i;
      continue;
    }
    if (c == ' ') {
      std::size_t k = i;
      std::size_t blanks = 0;
      while (k < text.size() && !IsPathSeparator(text[k]) && !EndsPathInText(text[k])) {
        blanks += text[k] == ' ' ? 1u : 0u;
        ++k;
      }
      if (k < text.size() && IsPathSeparator(text[k]) && text[k - 1] != ' ' && blanks <= kMaxBlanksInFolder &&
          k - i <= kMaxBlankRun) {
        i = k;
        continue;
      }
      break;
    }
    ++i;
  }
  return i;
}

std::optional<std::wstring> TryExtractTimestampTokenW(std::wstring_view s)
{
  // Search for pattern: YYYYMMDD_HHMMSS (15 chars), plus the precision suffix
  // the helper has written since 2026-02 (YYYYMMDD_HHMMSS_mmm): the same
  // token rule as the helper's retention (Retention.h TryExtractTimestampToken).
  auto is_digits = [](std::wstring_view v) {
    for (const wchar_t c : v) {
      if (!std::iswdigit(c)) {
        return false;
      }
    }
    return true;
  };

  std::optional<std::wstring> best;
  for (std::size_t i = 0; i + 15 <= s.size(); i++) {
    const std::wstring_view date = s.substr(i, 8);
    if (!is_digits(date)) {
      continue;
    }
    if (s[i + 8] != L'_') {
      continue;
    }
    const std::wstring_view time = s.substr(i + 9, 6);
    if (!is_digits(time)) {
      continue;
    }
    std::size_t tokenLen = 15;
    const std::size_t suffixSep = i + tokenLen;
    if (suffixSep < s.size() && s[suffixSep] == L'_') {
      std::size_t suffixEnd = suffixSep + 1;
      while (suffixEnd < s.size() && std::iswdigit(s[suffixEnd])) {
        suffixEnd++;
      }
      if (suffixEnd > suffixSep + 1) {
        tokenLen = suffixEnd - i;
      }
    }
    best = std::wstring(s.substr(i, tokenLen));
  }
  return best;
}

}  // namespace

std::wstring JoinList(const std::vector<std::wstring>& items, std::size_t maxN, std::wstring_view sep)
{
  if (items.empty() || maxN == 0) {
    return {};
  }
  const std::size_t n = std::min<std::size_t>(items.size(), maxN);
  std::wstring out;
  for (std::size_t i = 0; i < n; i++) {
    if (i > 0) {
      out += sep;
    }
    out += items[i];
  }
  if (items.size() > n) {
    out += sep;
    out += L"...";
  }
  return out;
}

bool WriteTextUtf8(const std::filesystem::path& path, const std::string& content, std::wstring* err)
{
  std::error_code ec;
  const auto parent = path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
  }
  if (ec) {
    if (err) *err = L"Failed to create output directory: " + parent.wstring();
    return false;
  }

  std::filesystem::path tempPath;
  HANDLE file = INVALID_HANDLE_VALUE;
  constexpr int kMaxTempCreateAttempts = 8;
  for (int attempt = 0; attempt < kMaxTempCreateAttempts; ++attempt) {
    tempPath = path;
    tempPath += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
      std::to_wstring(GetTickCount64()) + L"." +
      std::to_wstring(g_atomicWriteCounter.fetch_add(1u, std::memory_order_relaxed));
    file = CreateFileW(
      tempPath.c_str(),
      GENERIC_WRITE,
      0,
      nullptr,
      CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
      nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      break;
    }
    const DWORD createError = GetLastError();
    if (createError != ERROR_FILE_EXISTS && createError != ERROR_ALREADY_EXISTS) {
      break;
    }
  }
  if (file == INVALID_HANDLE_VALUE) {
    if (err) *err = L"Failed to create temporary output: " + tempPath.wstring();
    return false;
  }

  bool ok = true;
  std::size_t offset = 0;
  while (offset < content.size()) {
    const auto remaining = content.size() - offset;
    const DWORD take = static_cast<DWORD>((std::min<std::size_t>)(remaining, 16ull * 1024ull * 1024ull));
    DWORD written = 0;
    if (!WriteFile(file, content.data() + offset, take, &written, nullptr) || written != take) {
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
  if (ok && !MoveFileExW(
      tempPath.c_str(),
      path.c_str(),
      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    ok = false;
  }
  if (!ok) {
    DeleteFileW(tempPath.c_str());
    if (err) *err = L"Failed to atomically write output: " + path.wstring();
    return false;
  }

  if (err) err->clear();
  return true;
}

nlohmann::json DumpIdentityJson(const DumpIdentity& identity)
{
  return {
    { "schema", "skydiag.dump_identity.v1" },
    { "sha256", identity.sha256 },
    { "size_bytes", identity.size_bytes },
    { "last_write_time_utc_100ns", identity.last_write_time_utc_100ns },
  };
}

bool DumpIdentityMatchesJson(const nlohmann::json& value, const DumpIdentity& identity)
{
  if (!identity.IsValid() || !value.is_object()) {
    return false;
  }
  const auto schema = value.find("schema");
  const auto sha = value.find("sha256");
  const auto size = value.find("size_bytes");
  const auto modified = value.find("last_write_time_utc_100ns");
  return schema != value.end() && schema->is_string() &&
    schema->get_ref<const std::string&>() == "skydiag.dump_identity.v1" &&
    sha != value.end() && sha->is_string() &&
    sha->get_ref<const std::string&>() == identity.sha256 &&
    size != value.end() && size->is_number_unsigned() &&
    size->get<std::uint64_t>() == identity.size_bytes &&
    modified != value.end() && modified->is_number_unsigned() &&
    modified->get<std::uint64_t>() == identity.last_write_time_utc_100ns;
}

std::filesystem::path TriageStatePath(
  const std::filesystem::path& outBase,
  const DumpIdentity& identity)
{
  if (!identity.IsValid()) {
    return {};
  }
  return outBase
    / L".skydiag-triage"
    / Utf8ToWide(identity.sha256)
    / (Utf8ToWide(identity.StorageMetadataKey()) + L".json");
}

std::filesystem::path IdentityArtifactDirectory(
  const std::filesystem::path& outBase,
  const DumpIdentity& identity)
{
  if (!identity.IsValid()) {
    return {};
  }
  return outBase
    / L".skydiag-analysis"
    / Utf8ToWide(identity.sha256)
    / Utf8ToWide(identity.StorageMetadataKey());
}

std::filesystem::path OutputFamilyLockPath(
  const std::filesystem::path& outBase,
  std::wstring_view dumpStem)
{
  return outBase / L".skydiag-locks" /
    (std::wstring(dumpStem) + L".lock");
}

bool TriageHasReviewContent(const nlohmann::json& triage)
{
  if (!triage.is_object()) {
    return false;
  }
  const auto reviewed = triage.find("reviewed");
  if (reviewed != triage.end() && reviewed->is_boolean() && reviewed->get<bool>()) {
    return true;
  }
  const auto status = triage.find("review_status");
  if (status != triage.end() && status->is_string() &&
      status->get_ref<const std::string&>() != "unreviewed") {
    return true;
  }
  constexpr std::string_view fields[] = {
    "verdict",
    "actual_cause",
    "ground_truth_cause",
    "ground_truth_mod",
    "reviewer",
    "notes",
  };
  for (const auto field : fields) {
    const auto it = triage.find(std::string(field));
    if (it != triage.end() && it->is_string() && !it->get_ref<const std::string&>().empty()) {
      return true;
    }
  }
  return false;
}

namespace {

bool MergeIdentityBoundTriage(
  const nlohmann::json& root,
  const DumpIdentity& identity,
  nlohmann::json* triage)
{
  if (!root.is_object()) {
    return false;
  }
  const auto identityIt = root.find("dump_identity");
  const auto triageIt = root.find("triage");
  if (identityIt == root.end() ||
      triageIt == root.end() ||
      !DumpIdentityMatchesJson(*identityIt, identity) ||
      !triageIt->is_object()) {
    return false;
  }
  for (const auto& [key, value] : triageIt->items()) {
    (*triage)[key] = value;
  }
  return true;
}

}  // namespace

void LoadExistingSummaryTriage(
  const std::filesystem::path& summaryPath,
  const std::filesystem::path& outBase,
  const DumpIdentity& identity,
  nlohmann::json* triage)
{
  if (!triage) {
    return;
  }

  *triage = DefaultTriageFields();
  std::string existingText;
  const auto statePath = TriageStatePath(outBase, identity);
  if (!statePath.empty() && ReadTextFileUtf8(statePath, &existingText)) {
    const auto state = nlohmann::json::parse(existingText, nullptr, false);
    if (!state.is_discarded() && MergeIdentityBoundTriage(state, identity, triage)) {
      return;
    }
  }

  existingText.clear();
  if (!ReadTextFileUtf8(summaryPath, &existingText)) {
    return;
  }
  const auto existing = nlohmann::json::parse(existingText, nullptr, false);
  if (existing.is_discarded()) {
    return;
  }
  MergeIdentityBoundTriage(existing, identity, triage);
}

bool WriteTriageState(
  const std::filesystem::path& outBase,
  const DumpIdentity& identity,
  const nlohmann::json& triage,
  std::wstring* err)
{
  const auto path = TriageStatePath(outBase, identity);
  if (path.empty() || !triage.is_object()) {
    if (err) {
      *err = L"Cannot write triage state without a valid dump identity";
    }
    return false;
  }
  const nlohmann::json state = {
    { "schema", "skydiag.triage_state.v1" },
    { "dump_identity", DumpIdentityJson(identity) },
    { "triage", triage },
  };
  return WriteTextUtf8(path, state.dump(2) + "\n", err);
}

bool IsUnknownModuleField(std::wstring_view modulePlusOffset)
{
  if (modulePlusOffset.empty()) {
    return true;
  }
  std::wstring normalized;
  normalized.reserve(modulePlusOffset.size());
  for (const wchar_t ch : modulePlusOffset) {
    normalized.push_back(static_cast<wchar_t>(std::towlower(ch)));
  }
  auto trimPred = [](wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
  };
  while (!normalized.empty() && trimPred(normalized.front())) {
    normalized.erase(normalized.begin());
  }
  while (!normalized.empty() && trimPred(normalized.back())) {
    normalized.pop_back();
  }
  return normalized.empty() ||
    normalized == L"unknown" ||
    normalized == L"<unknown>" ||
    normalized == L"n/a" ||
    normalized == L"none";
}

std::wstring MaybeRedactPath(std::wstring_view path, bool redactPaths)
{
  if (!redactPaths || path.empty() || !LooksLikeAbsolutePath(path)) {
    return std::wstring(path);
  }
  return RedactPathValue(path);
}

std::wstring RedactSymbolSearchPath(std::wstring_view searchPath, bool redactPaths)
{
  if (!redactPaths || searchPath.empty()) {
    return std::wstring(searchPath);
  }
  // A DbgHelp search path is ';'-separated elements, each a directory or a
  // "srv*<cache>*<url>" / "cache*<dir>" chain. It may come from
  // _NT_SYMBOL_PATH or SKYRIMDIAG_SYMBOL_PATH, so redact every absolute
  // directory in it, not only the cache directory this tool picked.
  std::wstring out;
  out.reserve(searchPath.size());
  std::size_t start = 0;
  for (;;) {
    const std::size_t sep = searchPath.find_first_of(L";*", start);
    const std::size_t stop = sep == std::wstring_view::npos ? searchPath.size() : sep;
    const std::wstring_view element = searchPath.substr(start, stop - start);
    std::size_t lead = 0;
    while (lead < element.size() && (element[lead] == L' ' || element[lead] == L'\t')) {
      ++lead;
    }
    out.append(element.substr(0, lead));
    out += MaybeRedactPath(element.substr(lead), true);
    if (sep == std::wstring_view::npos) {
      break;
    }
    out.push_back(searchPath[sep]);
    start = sep + 1;
  }
  return out;
}

std::string RedactPathsInText(std::string_view text)
{
  constexpr std::string_view kRedacted = "<redacted>";
  constexpr std::string_view kUser = "<user>";
  std::string out;
  out.reserve(text.size());
  std::size_t pos = 0;
  while (pos < text.size()) {
    if (const std::size_t urlEnd = UrlEndAt(text, pos); urlEnd > pos) {
      out.append(text.substr(pos, urlEnd - pos));
      pos = urlEnd;
      continue;
    }
    if (const std::size_t root = AbsolutePathRootAt(text, pos); root > 0) {
      std::size_t nameStart = 0;
      const std::size_t end = AbsolutePathEnd(text, pos, root, &nameStart);
      // As MaybeRedactPath: only the last element stays, unless it is the
      // account's own folder ("C:\Users\<name>").
      out.append(kRedacted);
      if (nameStart < end) {
        out.push_back('\\');
        if (EndsWithUsersFolder(text.substr(pos, nameStart - pos))) {
          out.append(kUser);
        } else {
          out.append(text.substr(nameStart, end - nameStart));
        }
      }
      pos = end;
      continue;
    }
    // A profile folder without its drive ("/users/<name>/log.txt").
    if (IsPathSeparator(text[pos]) && EndsWithUsersFolder(text.substr(pos, std::min<std::size_t>(7u, text.size() - pos)))) {
      out.append(text.substr(pos, 7));
      std::size_t end = pos + 7u;
      while (end < text.size() && !IsPathSeparator(text[end]) && !EndsPathInText(text[end]) && text[end] != ' ') {
        ++end;
      }
      if (end > pos + 7u) {
        out.append(kUser);
      }
      pos = end;
      continue;
    }
    out.push_back(text[pos]);
    ++pos;
  }
  return out;
}

void RedactAbsolutePathStrings(nlohmann::json* value)
{
  if (!value) {
    return;
  }
  if (value->is_string()) {
    *value = RedactPathsInText(value->get_ref<const std::string&>());
    return;
  }
  if (value->is_array() || value->is_object()) {
    for (auto& child : *value) {
      RedactAbsolutePathStrings(&child);
    }
  }
}

std::string TruncateUtf8(std::string_view text, std::size_t maxBytes)
{
  if (text.size() <= maxBytes) {
    return std::string(text);
  }
  std::size_t cut = maxBytes;
  // Back up over continuation bytes (10xxxxxx) so the cut never splits a
  // code point.
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) {
    --cut;
  }
  return std::string(text.substr(0, cut));
}

std::filesystem::path DefaultOutDirForDump(const std::filesystem::path& dumpPath)
{
  if (dumpPath.has_parent_path()) {
    return dumpPath.parent_path();
  }
  return std::filesystem::current_path();
}

std::filesystem::path FindIncidentManifestForDump(const std::filesystem::path& outBase, std::wstring_view dumpStem)
{
  const auto tsOpt = TryExtractTimestampTokenW(dumpStem);
  if (!tsOpt) {
    return {};
  }
  const std::wstring& ts = *tsOpt;

  const std::wstring candidates[] = {
    L"SkyrimDiag_Incident_Crash_" + ts + L".json",
    L"SkyrimDiag_Incident_CrashRecapture_" + ts + L".json",
    L"SkyrimDiag_Incident_Hang_" + ts + L".json",
    L"SkyrimDiag_Incident_Manual_" + ts + L".json",
  };

  std::error_code ec;
  for (const auto& name : candidates) {
    const auto p = outBase / name;
    if (std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec)) {
      return p;
    }
  }

  return {};
}

bool TryLoadIncidentManifestJson(const std::filesystem::path& path, nlohmann::json* out)
{
  if (out) {
    *out = nlohmann::json();
  }
  std::string txt;
  if (!ReadTextFileUtf8(path, &txt)) {
    return false;
  }
  const auto j = nlohmann::json::parse(txt, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    return false;
  }
  if (out) {
    *out = j;
  }
  return true;
}

}  // namespace skydiag::dump_tool::internal::output_writer
