#include "PluginRules.h"
#include "SkyrimDiagStringUtil.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "Utf.h"

namespace skydiag::dump_tool {
namespace {

using skydiag::WideLower;
using i18n::AsciiLower;

bool IsImplicitRuntimeMaster(std::string_view masterLower)
{
  return masterLower == "skyrim.esm" ||
         masterLower == "update.esm" ||
         masterLower == "dawnguard.esm" ||
         masterLower == "hearthfires.esm" ||
         masterLower == "dragonborn.esm" ||
         masterLower == "ccbgssse001-fish.esm" ||
         masterLower == "ccqdrsse001-survivalmode.esl" ||
         masterLower == "ccbgssse037-curios.esl" ||
         masterLower == "ccbgssse025-advdsgs.esm" ||
         masterLower == "_resourcepack.esl" ||
         masterLower == "resourcepack.esl";
}

// Creation Club plugin names (ccXXXsse###-name.esm/.esl). The game loads the
// ones listed in Skyrim.ccc without a plugins.txt entry.
bool IsCreationClubPluginName(std::string_view nameLower)
{
  const bool masterOrLight = nameLower.size() > 4u &&
    (nameLower.substr(nameLower.size() - 4u) == ".esm" || nameLower.substr(nameLower.size() - 4u) == ".esl");
  return masterOrLight && nameLower.rfind("cc", 0) == 0;
}

i18n::ConfidenceLevel ParseConfidenceLevel(std::string_view s)
{
  const std::string lower = AsciiLower(s);
  if (lower == "high") {
    return i18n::ConfidenceLevel::kHigh;
  }
  if (lower == "medium") {
    return i18n::ConfidenceLevel::kMedium;
  }
  if (lower == "low") {
    return i18n::ConfidenceLevel::kLow;
  }
  return i18n::ConfidenceLevel::kUnknown;
}

std::vector<int> ParseVersionSegments(std::string_view s)
{
  std::vector<int> out;
  std::size_t i = 0;
  while (i < s.size()) {
    std::size_t start = i;
    while (i < s.size() && s[i] != '.') {
      ++i;
    }
    std::string_view token = s.substr(start, i - start);
    int value = 0;
    bool hasDigit = false;
    for (const char c : token) {
      if (!std::isdigit(static_cast<unsigned char>(c))) {
        break;
      }
      hasDigit = true;
      value = (value * 10) + (c - '0');
    }
    out.push_back(hasDigit ? value : 0);
    if (i < s.size() && s[i] == '.') {
      ++i;
    }
  }
  return out;
}

bool IsGamePluginList(std::string_view pluginsSource)
{
  return pluginsSource == "mo2_profile" || pluginsSource == "standard";
}

// A non-negative JSON integer as a count, or nullopt for any other value.
std::optional<std::size_t> ReadCount(const nlohmann::json& value)
{
  if (value.is_number_unsigned()) {
    return static_cast<std::size_t>(value.get<std::uint64_t>());
  }
  if (value.is_number_integer() && value.get<std::int64_t>() >= 0) {
    return static_cast<std::size_t>(value.get<std::int64_t>());
  }
  return std::nullopt;
}

struct RuleCondition
{
  std::optional<double> any_plugin_header_version_gte;
  std::optional<std::string> game_version_lt;
  std::vector<std::wstring> modules_not_loaded_lower;
  std::optional<bool> has_missing_master;
  std::optional<std::size_t> esl_count_gte;
  std::optional<std::size_t> full_plugin_count_gte;
};

// Reads a rule's conditions. Any unknown key, mistyped value or an empty
// condition rejects the rule: ignoring one would broaden it until it fires on
// every report (the same policy as crash_signatures.json).
bool ParseRuleCondition(const nlohmann::json& cond, RuleCondition* rule)
{
  if (cond.empty()) {
    return false;
  }
  for (auto it = cond.begin(); it != cond.end(); ++it) {
    const std::string& key = it.key();
    const auto& value = it.value();
    if (key == "any_plugin_header_version_gte") {
      if (!value.is_number()) {
        return false;
      }
      rule->any_plugin_header_version_gte = value.get<double>();
    } else if (key == "game_version_lt") {
      if (!value.is_string() || value.get_ref<const std::string&>().empty()) {
        return false;
      }
      rule->game_version_lt = value.get<std::string>();
    } else if (key == "module_not_loaded") {
      if (value.is_string() && !value.get_ref<const std::string&>().empty()) {
        rule->modules_not_loaded_lower.push_back(WideLower(Utf8ToWide(value.get<std::string>())));
      } else if (value.is_array() && !value.empty()) {
        for (const auto& name : value) {
          if (!name.is_string() || name.get_ref<const std::string&>().empty()) {
            return false;
          }
          rule->modules_not_loaded_lower.push_back(WideLower(Utf8ToWide(name.get<std::string>())));
        }
      } else {
        return false;
      }
    } else if (key == "has_missing_master") {
      if (!value.is_boolean()) {
        return false;
      }
      rule->has_missing_master = value.get<bool>();
    } else if (key == "esl_count_gte" || key == "full_plugin_count_gte") {
      const auto count = ReadCount(value);
      if (!count) {
        return false;
      }
      if (key == "esl_count_gte") {
        rule->esl_count_gte = count;
      } else {
        rule->full_plugin_count_gte = count;
      }
    } else {
      return false;
    }
  }
  return true;
}

}  // namespace

bool ParsePluginScanJson(std::string_view jsonUtf8, ParsedPluginScan* out)
{
  if (!out) {
    return false;
  }

  try {
    const auto j = nlohmann::json::parse(jsonUtf8, nullptr, true);
    if (!j.is_object()) {
      return false;
    }

    ParsedPluginScan parsed{};
    parsed.game_exe_version = j.value("game_exe_version", "");
    parsed.plugins_source = j.value("plugins_source", "");
    parsed.mo2_detected = j.value("mo2_detected", false);
    parsed.implicit_plugins_included = j.value("implicit_plugins_included", false);
    parsed.implicit_masters_verified = j.value("implicit_masters_verified", false);

    if (auto it = j.find("plugins"); it != j.end() && it->is_array()) {
      parsed.plugins.reserve(it->size());
      for (const auto& p : *it) {
        if (!p.is_object()) {
          continue;
        }
        PluginEntryInfo info{};
        info.filename = p.value("filename", "");
        info.header_version = p.value("header_version", 0.0f);
        info.is_esl = p.value("is_esl", false);
        info.is_active = p.value("is_active", false);
        info.slot_type_known = p.value("slot_type_known", true);
        if (auto itMasters = p.find("masters"); itMasters != p.end() && itMasters->is_array()) {
          for (const auto& m : *itMasters) {
            if (m.is_string()) {
              info.masters.push_back(m.get<std::string>());
            }
          }
        }
        parsed.plugins.push_back(std::move(info));
      }
    }

    if (auto it = j.find("skse_log"); it != j.end() && it->is_object()) {
      auto& log = parsed.skse_log;
      log.status = it->value("status", "");
      log.skse_version = it->value("skse_version", "");
      log.checked_count = it->value("checked_count", 0u);
      log.loaded_count = it->value("loaded_count", 0u);
      log.issue_count = it->value("issue_count", 0u);
      log.time_checked = it->value("time_checked", false);
      if (auto itIssues = it->find("issues"); itIssues != it->end() && itIssues->is_array()) {
        for (const auto& issue : *itIssues) {
          if (!issue.is_object()) {
            continue;
          }
          SkseLogIssueInfo info{};
          info.dll_name = issue.value("dll", "");
          info.plugin_name = issue.value("name", "");
          info.status = issue.value("status", "");
          info.error_code = issue.value("code", static_cast<std::int64_t>(0));
          if (!info.dll_name.empty()) {
            log.issues.push_back(std::move(info));
          }
        }
      }
      if (log.issue_count < log.issues.size()) {
        log.issue_count = static_cast<std::uint32_t>(log.issues.size());
      }
    }

    *out = std::move(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

std::vector<std::wstring> ComputeMissingMasters(const ParsedPluginScan& scan)
{
  std::unordered_set<std::string> active;
  active.reserve(scan.plugins.size());
  for (const auto& plugin : scan.plugins) {
    if (!plugin.is_active || plugin.filename.empty()) {
      continue;
    }
    active.insert(AsciiLower(plugin.filename));
  }

  std::unordered_set<std::string> added;
  std::vector<std::wstring> missing;
  for (const auto& plugin : scan.plugins) {
    if (!plugin.is_active) {
      continue;
    }
    for (const auto& master : plugin.masters) {
      if (master.empty()) {
        continue;
      }
      const std::string masterLower = AsciiLower(master);
      if (active.find(masterLower) != active.end()) {
        continue;
      }
      // Base game/DLC and mandatory free CC masters can be implicitly loaded even when
      // they are not listed in plugins.txt (depends on manager/runtime). Only a scan
      // that checked them where the game looks knows one is really absent.
      if (!scan.implicit_masters_verified && IsImplicitRuntimeMaster(masterLower)) {
        continue;
      }
      // A scan that predates implicit plugins cannot see which Creation Club
      // files Skyrim.ccc loaded, so a CC master absent from plugins.txt is not
      // evidence that it is missing.
      if (!scan.implicit_plugins_included && IsCreationClubPluginName(masterLower)) {
        continue;
      }
      if (added.insert(masterLower).second) {
        missing.push_back(Utf8ToWide(master));
      }
    }
  }

  return missing;
}

bool AnyPluginHeaderVersionGte(const ParsedPluginScan& scan, double threshold)
{
  for (const auto& plugin : scan.plugins) {
    if (plugin.is_active && static_cast<double>(plugin.header_version) + 1e-6 >= threshold) {
      return true;
    }
  }
  return false;
}

std::size_t CountEslPlugins(const ParsedPluginScan& scan)
{
  std::size_t count = 0;
  for (const auto& plugin : scan.plugins) {
    if (plugin.is_active && plugin.is_esl) {
      ++count;
    }
  }
  return count;
}

std::size_t CountFullPlugins(const ParsedPluginScan& scan)
{
  std::size_t count = 0;
  for (const auto& plugin : scan.plugins) {
    if (plugin.is_active && plugin.slot_type_known && !plugin.is_esl) {
      ++count;
    }
  }
  return count;
}

bool IsGameVersionLessThan(std::string_view lhs, std::string_view rhs)
{
  const auto lv = ParseVersionSegments(lhs);
  const auto rv = ParseVersionSegments(rhs);
  const std::size_t n = std::max(lv.size(), rv.size());
  for (std::size_t i = 0; i < n; ++i) {
    const int a = (i < lv.size()) ? lv[i] : 0;
    const int b = (i < rv.size()) ? rv[i] : 0;
    if (a < b) {
      return true;
    }
    if (a > b) {
      return false;
    }
  }
  return false;
}

std::wstring DescribeSkseLoadStatus(std::string_view status, std::int64_t errorCode, bool en)
{
  // Texts from SKSE64's PluginManager.cpp (2.2.x / 2.3.x).
  if (status == "no version data") {
    return en ? L"no version data (a helper DLL that is not an SKSE plugin, or an old plugin without AE version data)"
              : L"버전 정보 없음(SKSE 플러그인이 아닌 보조 DLL이거나 AE용 버전 정보가 없는 구형 플러그인)";
  }
  if (status == "disabled, incompatible with current version of the game") {
    return en ? L"made for a different game version" : L"다른 게임 버전용";
  }
  if (status == "disabled, address library needs to be updated") {
    return en ? L"Address Library for this game version is missing" : L"이 게임 버전용 Address Library 없음";
  }
  if (status == "must be recompiled for new address library") {
    return en ? L"must be rebuilt for the new Address Library format" : L"새 Address Library 형식용으로 다시 빌드해야 함";
  }
  if (status == "disabled, only compatible with versions earlier than 1.6.629") {
    return en ? L"only works on game versions before 1.6.629" : L"1.6.629 이전 게임 전용";
  }
  if (status == "disabled, requires newer script extender") {
    return en ? L"needs a newer SKSE" : L"더 새로운 SKSE 필요";
  }
  if (status == "disabled, bad version data" || status == "disabled, no name specified" ||
      status == "disabled, unsupported version independence method") {
    return en ? L"invalid version data" : L"잘못된 버전 정보";
  }
  if (status == "couldn't load plugin") {
    if (errorCode == 126) {
      return en ? L"could not be loaded: a DLL it needs is missing (error 126)"
                : L"불러오지 못함: 필요한 다른 DLL이 없음(오류 126)";
    }
    return en ? (L"could not be loaded (error " + std::to_wstring(errorCode) + L")")
              : (L"불러오지 못함(오류 " + std::to_wstring(errorCode) + L")");
  }
  if (status == "does not appear to be an SKSE plugin") {
    return en ? L"not an SKSE plugin" : L"SKSE 플러그인이 아님";
  }
  if (status == "LE plugin cannot be used with SE") {
    return en ? L"32-bit (LE) plugin" : L"32비트(LE)용 플러그인";
  }
  if (status == "reported as incompatible during load") {
    return en ? L"the plugin reported itself incompatible while loading" : L"로드 중 플러그인 스스로 비호환을 알림";
  }
  if (status == "crashed during postload") {
    return en ? L"crashed during post-load setup" : L"로드 후 초기화 중 충돌";
  }
  if (status == "disabled, fatal error occurred while loading plugin" ||
      status == "disabled, fatal error occurred while checking plugin compatibility") {
    return en ? L"an error occurred while SKSE loaded or checked it" : L"SKSE가 로드하거나 검사하는 중 오류 발생";
  }
  return Utf8ToWide(status);
}

std::wstring SummarizeSkseLogIssues(
  const SkseLogScanInfo& log,
  bool en,
  std::size_t maxItems,
  std::wstring_view separator)
{
  // DLLs with the same meaning share one explanation, in first-seen order:
  // "a.dll, b.dll: <meaning>" instead of repeating a long text per DLL.
  struct Group
  {
    std::wstring meaning;
    std::wstring dlls;
  };
  std::vector<Group> groups;
  std::size_t shown = 0;
  for (const auto& issue : log.issues) {
    if (shown == maxItems) {
      break;
    }
    auto meaning = DescribeSkseLoadStatus(issue.status, issue.error_code, en);
    auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.meaning == meaning; });
    if (it == groups.end()) {
      groups.push_back(Group{ std::move(meaning), {} });
      it = std::prev(groups.end());
    } else {
      it->dlls += L", ";
    }
    it->dlls += Utf8ToWide(issue.dll_name);
    ++shown;
  }
  std::wstring out;
  for (const auto& group : groups) {
    if (!out.empty()) {
      out += separator;
    }
    out += group.dlls + L": " + group.meaning;
  }
  const std::size_t total = std::max<std::size_t>(log.issue_count, log.issues.size());
  if (total > shown) {
    out += en ? (L" (+" + std::to_wstring(total - shown) + L" more)")
              : (L" (외 " + std::to_wstring(total - shown) + L"개)");
  }
  return out;
}

bool PluginRules::LoadFromJson(const std::filesystem::path& jsonPath)
{
  try {
    std::ifstream f(jsonPath);
    if (!f.is_open()) {
      return false;
    }
    const auto j = nlohmann::json::parse(f, nullptr, true);
    if (!j.is_object() || !j.contains("rules") || !j["rules"].is_array()) {
      return false;
    }
    // Only schema 1 is understood; a newer file may carry conditions this
    // build would read too broadly.
    if (!j.contains("version") || !j["version"].is_number_unsigned() || j["version"].get<std::uint64_t>() != 1u) {
      return false;
    }

    std::vector<Rule> rules;
    rules.reserve(j["rules"].size());
    for (const auto& item : j["rules"]) {
      if (!item.is_object()) {
        continue;
      }
      const auto itId = item.find("id");
      const auto itCondition = item.find("condition");
      const auto itDiagnosis = item.find("diagnosis");
      if (itId == item.end() || !itId->is_string() ||
          itCondition == item.end() || !itCondition->is_object() ||
          itDiagnosis == item.end() || !itDiagnosis->is_object()) {
        continue;
      }

      RuleCondition cond{};
      if (!ParseRuleCondition(*itCondition, &cond)) {
        continue;
      }
      Rule rule{};
      rule.id = itId->get<std::string>();
      rule.any_plugin_header_version_gte = cond.any_plugin_header_version_gte;
      rule.game_version_lt = std::move(cond.game_version_lt);
      rule.modules_not_loaded_lower = std::move(cond.modules_not_loaded_lower);
      rule.has_missing_master = cond.has_missing_master;
      rule.esl_count_gte = cond.esl_count_gte;
      rule.full_plugin_count_gte = cond.full_plugin_count_gte;

      const auto& diag = *itDiagnosis;
      rule.cause_ko = Utf8ToWide(diag.value("cause_ko", ""));
      rule.cause_en = Utf8ToWide(diag.value("cause_en", ""));
      rule.confidence = AsciiLower(diag.value("confidence", ""));

      if (auto it = diag.find("recommendations_ko"); it != diag.end() && it->is_array()) {
        rule.recommendations_ko.reserve(it->size());
        for (const auto& rec : *it) {
          if (rec.is_string()) {
            rule.recommendations_ko.push_back(Utf8ToWide(rec.get<std::string>()));
          }
        }
      }
      if (auto it = diag.find("recommendations_en"); it != diag.end() && it->is_array()) {
        rule.recommendations_en.reserve(it->size());
        for (const auto& rec : *it) {
          if (rec.is_string()) {
            rule.recommendations_en.push_back(Utf8ToWide(rec.get<std::string>()));
          }
        }
      }

      if (!rule.id.empty()) {
        rules.push_back(std::move(rule));
      }
    }

    m_rules = std::move(rules);
    return !m_rules.empty();
  } catch (...) {
    return false;
  }
}

std::vector<PluginRuleDiagnosis> PluginRules::Evaluate(const PluginRulesContext& ctx) const
{
  std::vector<PluginRuleDiagnosis> out;
  if (!ctx.scan || m_rules.empty()) {
    return out;
  }

  const ParsedPluginScan& scan = *ctx.scan;
  const std::vector<std::wstring> missingMasters = ctx.missing_masters.empty()
    ? ComputeMissingMasters(scan)
    : ctx.missing_masters;
  const std::size_t eslCount = CountEslPlugins(scan);
  const std::size_t fullPluginCount = CountFullPlugins(scan);

  std::unordered_set<std::wstring> loadedModulesLower;
  loadedModulesLower.reserve(ctx.loaded_module_filenames.size());
  for (const auto& mod : ctx.loaded_module_filenames) {
    loadedModulesLower.insert(WideLower(mod));
  }

  out.reserve(m_rules.size());
  for (const auto& rule : m_rules) {
    if (rule.any_plugin_header_version_gte.has_value() &&
        !AnyPluginHeaderVersionGte(scan, *rule.any_plugin_header_version_gte)) {
      continue;
    }

    if (rule.game_version_lt.has_value()) {
      if (ctx.game_version.empty() || !IsGameVersionLessThan(ctx.game_version, *rule.game_version_lt)) {
        continue;
      }
    }

    if (std::any_of(rule.modules_not_loaded_lower.begin(), rule.modules_not_loaded_lower.end(),
          [&](const std::wstring& name) { return loadedModulesLower.contains(name); })) {
      continue;
    }

    if (rule.has_missing_master.has_value()) {
      const bool hasMissing = !missingMasters.empty();
      if (hasMissing != *rule.has_missing_master) {
        continue;
      }
    }

    if (rule.esl_count_gte.has_value() && eslCount < *rule.esl_count_gte) {
      continue;
    }

    if (rule.full_plugin_count_gte.has_value() && fullPluginCount < *rule.full_plugin_count_gte) {
      continue;
    }

    PluginRuleDiagnosis diag{};
    diag.rule_id = rule.id;
    diag.confidence_level = ParseConfidenceLevel(rule.confidence);
    // A missing master is only as certain as the list it was read from. Only
    // the game's own list (the MO2 profile's or the standard plugins.txt) is
    // trusted beyond Low; a plugins.txt beside the exe may be stale.
    if (rule.has_missing_master.value_or(false) && !IsGamePluginList(scan.plugins_source) &&
        (diag.confidence_level == i18n::ConfidenceLevel::kHigh ||
         diag.confidence_level == i18n::ConfidenceLevel::kMedium)) {
      diag.confidence_level = i18n::ConfidenceLevel::kLow;
    }
    diag.confidence = std::wstring(i18n::ConfidenceLabel(
      ctx.use_korean ? i18n::Language::kKorean : i18n::Language::kEnglish,
      diag.confidence_level));
    diag.cause = ctx.use_korean ? rule.cause_ko : rule.cause_en;
    diag.recommendations = ctx.use_korean ? rule.recommendations_ko : rule.recommendations_en;
    out.push_back(std::move(diag));
  }

  return out;
}

std::size_t PluginRules::RuleCount() const
{
  return m_rules.size();
}

}  // namespace skydiag::dump_tool
