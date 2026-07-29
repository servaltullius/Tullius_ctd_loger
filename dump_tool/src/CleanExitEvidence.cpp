#include "CleanExitEvidence.h"

#include "Analyzer.h"
#include "OutputWriterInternals.h"
#include "Utf.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace skydiag::dump_tool {
namespace {

constexpr std::uintmax_t kMaxEvidenceBytes = 128ull * 1024ull;

std::wstring WideLower(std::wstring value)
{
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return value;
}

bool HasEvidenceFilename(const std::filesystem::path& path)
{
  const std::wstring name = WideLower(path.filename().wstring());
  constexpr std::wstring_view kPrefix = L"skyrimdiag_cleanexitevidence_";
  return name.size() > kPrefix.size() + 5u &&
         name.starts_with(kPrefix) &&
         name.ends_with(L".json");
}

bool ReadUnsigned(const nlohmann::json& root, std::string_view key, std::uint64_t* out)
{
  const auto it = root.find(std::string(key));
  if (it == root.end() || !it->is_number_unsigned()) {
    return false;
  }
  if (out) {
    *out = it->get<std::uint64_t>();
  }
  return true;
}

bool IsExactMatch(
  const nlohmann::json& root,
  const std::filesystem::path& dumpPath,
  const AnalysisResult& result,
  std::string* dumpState)
{
  if (!root.is_object()) {
    return false;
  }

  const auto schemaIt = root.find("schema");
  const auto preservedIt = root.find("dump_preserved");
  if (schemaIt == root.end() ||
      !schemaIt->is_string() ||
      schemaIt->get_ref<const std::string&>() != "skydiag.clean_exit_evidence.v1" ||
      preservedIt == root.end() ||
      !preservedIt->is_boolean() ||
      !preservedIt->get<bool>()) {
    return false;
  }

  const auto stateIt = root.find("dump_state");
  if (stateIt == root.end() || !stateIt->is_string()) {
    return false;
  }
  const std::string& state = stateIt->get_ref<const std::string&>();
  if (state != "preserved" && state != "delete_failed") {
    return false;
  }
  const auto filenameIt = root.find("dump_filename");
  if (filenameIt == root.end() || !filenameIt->is_string()) {
    return false;
  }
  const std::wstring recordedFilename = Utf8ToWide(filenameIt->get<std::string>());
  if (WideLower(recordedFilename) != WideLower(dumpPath.filename().wstring())) {
    return false;
  }

  std::uint64_t dumpSize = 0;
  std::uint64_t dumpLastWrite = 0;
  std::uint64_t exceptionCode = 0;
  std::uint64_t exceptionAddress = 0;
  std::uint64_t faultingTid = 0;
  std::uint64_t stateFlags = 0;
  std::uint64_t crashSeq = 0;
  if (!ReadUnsigned(root, "dump_size_bytes", &dumpSize) ||
      !ReadUnsigned(root, "dump_last_write_time_utc_100ns", &dumpLastWrite) ||
      !ReadUnsigned(root, "exception_code", &exceptionCode) ||
      !ReadUnsigned(root, "exception_addr", &exceptionAddress) ||
      !ReadUnsigned(root, "faulting_tid", &faultingTid) ||
      !ReadUnsigned(root, "state_flags", &stateFlags) ||
      !ReadUnsigned(root, "crash_seq", &crashSeq)) {
    return false;
  }

  const bool committedCrash = crashSeq != 0u && (crashSeq & 1u) == 0u;
  const bool identityMatches =
    result.dump_identity.IsValid() &&
    dumpSize == result.dump_identity.size_bytes &&
    dumpLastWrite == result.dump_identity.last_write_time_utc_100ns;
  const bool minidumpMatches =
    exceptionCode == result.exc_code &&
    exceptionAddress == result.exc_addr &&
    faultingTid == result.exc_tid;
  const bool blackboxMatches =
    result.has_blackbox &&
    crashSeq == result.blackbox_crash_seq &&
    exceptionCode == result.blackbox_exception_code &&
    exceptionAddress == result.blackbox_exception_addr &&
    faultingTid == result.blackbox_faulting_tid &&
    stateFlags == result.state_flags;
  if (!committedCrash || !identityMatches || !minidumpMatches || !blackboxMatches) {
    return false;
  }

  if (dumpState) {
    *dumpState = state;
  }
  return true;
}

}  // namespace

bool TryConsumeCleanExitEvidence(
  const std::wstring& dumpPathValue,
  AnalysisResult& result)
{
  try {
    const std::filesystem::path dumpPath(dumpPathValue);
    const std::filesystem::path parent = dumpPath.has_parent_path()
      ? dumpPath.parent_path()
      : std::filesystem::current_path();

    std::error_code ec;
    std::filesystem::directory_iterator it(parent, ec);
    if (ec) {
      return false;
    }

    std::filesystem::path matchedPath;
    std::string matchedState;
    std::size_t matchCount = 0;
    for (const auto& entry : it) {
      if (!entry.is_regular_file(ec) || ec || !HasEvidenceFilename(entry.path())) {
        ec.clear();
        continue;
      }
      const auto size = entry.file_size(ec);
      if (ec || size == 0u || size > kMaxEvidenceBytes) {
        ec.clear();
        continue;
      }

      std::string text;
      if (!internal::output_writer::ReadTextFileUtf8(entry.path(), &text)) {
        continue;
      }
      const auto root = nlohmann::json::parse(text, nullptr, false);
      std::string state;
      if (!root.is_discarded() && IsExactMatch(root, dumpPath, result, &state)) {
        ++matchCount;
        matchedPath = entry.path();
        matchedState = std::move(state);
      }
    }

    if (matchCount != 1u) {
      return false;
    }

    result.clean_exit_dump_state = std::move(matchedState);
    result.clean_exit_evidence_filename = matchedPath.filename().wstring();
    result.is_filtered_clean_exit = true;
    return true;
  } catch (...) {
    // Clean-exit evidence is optional and untrusted. Malformed JSON types,
    // filesystem races, encoding failures, or allocation errors must never
    // terminate dump analysis or turn an unvalidated sidecar into authority.
    return false;
  }
}

}  // namespace skydiag::dump_tool
