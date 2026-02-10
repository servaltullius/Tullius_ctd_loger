#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace skydiag::dump_tool::internal::output_writer {

std::wstring JoinList(const std::vector<std::wstring>& items, std::size_t maxN, std::wstring_view sep);

bool WriteTextUtf8(const std::filesystem::path& path, const std::string& content, std::wstring* err);

void LoadExistingSummaryTriage(const std::filesystem::path& summaryPath, nlohmann::json* triage);

bool IsUnknownModuleField(std::wstring_view modulePlusOffset);

std::wstring MaybeRedactPath(std::wstring_view path, bool redactPaths);

std::wstring ReplaceAll(std::wstring text, std::wstring_view from, std::wstring_view to);

std::filesystem::path DefaultOutDirForDump(const std::filesystem::path& dumpPath);

std::filesystem::path FindIncidentManifestForDump(const std::filesystem::path& outBase, std::wstring_view dumpStem);

bool TryLoadIncidentManifestJson(const std::filesystem::path& path, nlohmann::json* out);

}  // namespace skydiag::dump_tool::internal::output_writer

