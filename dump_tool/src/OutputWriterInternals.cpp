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
  return L"<redacted>\\" + filename;
}

std::optional<std::wstring> TryExtractTimestampTokenW(std::wstring_view s)
{
  // Search for pattern: YYYYMMDD_HHMMSS (15 chars)
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
    best = std::wstring(s.substr(i, 15));
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

std::wstring ReplaceAll(std::wstring text, std::wstring_view from, std::wstring_view to)
{
  if (text.empty() || from.empty()) {
    return text;
  }
  std::size_t pos = 0;
  while ((pos = text.find(from, pos)) != std::wstring::npos) {
    text.replace(pos, from.size(), to);
    pos += to.size();
  }
  return text;
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
