#pragma once

#include <cstddef>
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
  std::vector<std::string> masters;
};

struct ParsedPluginScan
{
  std::string game_exe_version;
  std::string plugins_source;
  bool mo2_detected = false;
  std::vector<PluginEntryInfo> plugins;
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

class PluginRules
{
public:
  bool LoadFromJson(const std::filesystem::path& jsonPath);
  std::vector<PluginRuleDiagnosis> Evaluate(const PluginRulesContext& ctx) const;
  std::size_t RuleCount() const;

private:
  struct Rule;
  std::vector<Rule> m_rules;
};

}  // namespace skydiag::dump_tool
