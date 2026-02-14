#include "OutputWriterInternals.h"

#include <algorithm>
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
namespace {

bool ReadTextUtf8(const std::filesystem::path& path, std::string* out)
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
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    if (err) *err = L"Failed to open output: " + path.wstring();
    return false;
  }
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!out) {
    if (err) *err = L"Failed to write output: " + path.wstring();
    return false;
  }
  if (err) err->clear();
  return true;
}

void LoadExistingSummaryTriage(const std::filesystem::path& summaryPath, nlohmann::json* triage)
{
  if (!triage) {
    return;
  }

  *triage = DefaultTriageFields();
  std::string existingText;
  if (!ReadTextUtf8(summaryPath, &existingText)) {
    return;
  }
  const auto existing = nlohmann::json::parse(existingText, nullptr, false);
  if (existing.is_discarded() || !existing.is_object()) {
    return;
  }
  const auto it = existing.find("triage");
  if (it == existing.end() || !it->is_object()) {
    return;
  }

  for (const auto& [k, v] : it->items()) {
    (*triage)[k] = v;
  }
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
  if (!ReadTextUtf8(path, &txt)) {
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
