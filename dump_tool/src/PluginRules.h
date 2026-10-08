#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "I18nCore.h"

namespace skydiag::dump_tool {

struct PluginEntryInfo
{
  std::string filename;
  float header_version = 0.0f;
  bool is_esl = false;
  bool is_active = false;
  bool slot_type_known = true;
  std::vector<std::string> masters;
};

// A DLL that SKSE did not load in the captured session, as skse64.log said.
struct SkseLogIssueInfo
{
  std::string dll_name;
  std::string plugin_name;
  std::string status;  // SKSE's own text
  std::int64_t error_code = 0;
};

struct SkseLogScanInfo
{
  // matched / no_matching_log / not_found / no_image_base; empty for scans
  // made before the helper read skse64.log
  std::string status;
  std::string skse_version;
  std::uint32_t checked_count = 0;
  std::uint32_t loaded_count = 0;
  std::uint32_t issue_count = 0;  // may exceed issues.size() when the helper capped the list
  std::vector<SkseLogIssueInfo> issues;
};

struct ParsedPluginScan
{
  std::string game_exe_version;
  std::string plugins_source;
  bool mo2_detected = false;
  // Set by scans that list the base masters and Skyrim.ccc plugins the game
  // loads on its own; older scans listed only plugins.txt.
  bool implicit_plugins_included = false;
  std::vector<PluginEntryInfo> plugins;
  SkseLogScanInfo skse_log;
};

struct PluginRuleDiagnosis
{
  std::string rule_id;
  i18n::ConfidenceLevel confidence_level = i18n::ConfidenceLevel::kUnknown;
  std::wstring confidence;
  std::wstring cause;
  std::vector<std::wstring> recommendations;
};

struct PluginRulesContext
{
  const ParsedPluginScan* scan = nullptr;
  std::vector<std::wstring> loaded_module_filenames;
  std::string game_version;
  std::vector<std::wstring> missing_masters;
  bool use_korean = false;
};

bool ParsePluginScanJson(std::string_view jsonUtf8, ParsedPluginScan* out);
std::vector<std::wstring> ComputeMissingMasters(const ParsedPluginScan& scan);
bool AnyPluginHeaderVersionGte(const ParsedPluginScan& scan, double threshold);
std::size_t CountEslPlugins(const ParsedPluginScan& scan);
bool IsGameVersionLessThan(std::string_view lhs, std::string_view rhs);
// Short reader-facing meaning of an SKSE load status from skse64.log.
std::wstring DescribeSkseLoadStatus(std::string_view status, std::int64_t errorCode, bool en);
// "<dll>: <meaning>" for the first maxItems DLLs SKSE did not load, then "+N more".
std::wstring SummarizeSkseLogIssues(
  const SkseLogScanInfo& log,
  bool en,
  std::size_t maxItems,
  std::wstring_view separator);

class PluginRules
{
public:
  bool LoadFromJson(const std::filesystem::path& jsonPath);
  std::vector<PluginRuleDiagnosis> Evaluate(const PluginRulesContext& ctx) const;
  std::size_t RuleCount() const;

private:
  struct Rule
  {
    std::string id;

    std::optional<double> any_plugin_header_version_gte;
    std::optional<std::string> game_version_lt;
    std::optional<std::wstring> module_not_loaded_lower;
    std::optional<bool> has_missing_master;
    std::optional<std::size_t> esl_count_gte;
    std::optional<std::size_t> full_plugin_count_gte;

    std::wstring cause_ko;
    std::wstring cause_en;
    std::string confidence;
    std::vector<std::wstring> recommendations_ko;
    std::vector<std::wstring> recommendations_en;
  };
  std::vector<Rule> m_rules;
};

}  // namespace skydiag::dump_tool
