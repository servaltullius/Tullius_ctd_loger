#include "GraphicsInjectionDiag.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <string_view>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "Utf.h"

namespace skydiag::dump_tool {
namespace {

std::wstring WideLower(std::wstring_view s)
{
  std::wstring out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return out;
}

std::string AsciiLower(std::string_view s)
{
  std::string out;
  out.reserve(s.size());
  std::transform(s.begin(), s.end(), std::back_inserter(out), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  return out;
}

std::vector<std::wstring> ParseStringArrayAsWideLower(const nlohmann::json& j)
{
  std::vector<std::wstring> out;
  if (!j.is_array()) {
    return out;
  }
  out.reserve(j.size());
  for (const auto& item : j) {
    if (!item.is_string()) {
      continue;
    }
    out.push_back(WideLower(Utf8ToWide(item.get<std::string>())));
  }
  return out;
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

bool HasAny(const std::unordered_set<std::wstring>& present, const std::vector<std::wstring>& requiredAny)
{
  if (requiredAny.empty()) {
    return true;
  }
  for (const auto& token : requiredAny) {
    if (present.find(token) != present.end()) {
      return true;
    }
  }
  return false;
}

bool HasAll(const std::unordered_set<std::wstring>& present, const std::vector<std::wstring>& requiredAll)
{
  for (const auto& token : requiredAll) {
    if (present.find(token) == present.end()) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool GraphicsInjectionDiag::LoadRules(const std::filesystem::path& jsonPath)
{
  try {
    std::ifstream f(jsonPath);
    if (!f.is_open()) {
      return false;
    }
    const auto j = nlohmann::json::parse(f, nullptr, true);
    if (!j.is_object()) {
      return false;
    }

    std::vector<DetectionGroup> groups;
    if (auto it = j.find("detection_modules"); it != j.end() && it->is_object()) {
      for (const auto& [nameUtf8, dllsJson] : it->items()) {
        DetectionGroup g{};
        g.name = WideLower(Utf8ToWide(nameUtf8));
        g.dlls = ParseStringArrayAsWideLower(dllsJson);
        if (!g.name.empty() && !g.dlls.empty()) {
          groups.push_back(std::move(g));
        }
      }
    }

    if (!j.contains("rules") || !j["rules"].is_array()) {
      return false;
    }

    std::vector<Rule> rules;
    rules.reserve(j["rules"].size());
    for (const auto& entry : j["rules"]) {
      if (!entry.is_object()) {
        continue;
      }
      const auto itId = entry.find("id");
      const auto itDetect = entry.find("detect");
      const auto itDiag = entry.find("diagnosis");
      if (itId == entry.end() || !itId->is_string() ||
          itDetect == entry.end() || !itDetect->is_object() ||
          itDiag == entry.end() || !itDiag->is_object()) {
        continue;
      }

      Rule r{};
      r.id = itId->get<std::string>();
      r.modules_any = ParseStringArrayAsWideLower(itDetect->value("modules_any", nlohmann::json::array()));
      r.modules_all = ParseStringArrayAsWideLower(itDetect->value("modules_all", nlohmann::json::array()));
      r.fault_module_any = ParseStringArrayAsWideLower(itDetect->value("fault_module_any", nlohmann::json::array()));
      r.cause_ko = Utf8ToWide(itDiag->value("cause_ko", ""));
      r.cause_en = Utf8ToWide(itDiag->value("cause_en", ""));
      r.confidence = AsciiLower(itDiag->value("confidence", ""));

      if (auto it = itDiag->find("recommendations_ko"); it != itDiag->end() && it->is_array()) {
        r.recommendations_ko.reserve(it->size());
        for (const auto& rec : *it) {
          if (rec.is_string()) {
            r.recommendations_ko.push_back(Utf8ToWide(rec.get<std::string>()));
          }
        }
      }
      if (auto it = itDiag->find("recommendations_en"); it != itDiag->end() && it->is_array()) {
        r.recommendations_en.reserve(it->size());
        for (const auto& rec : *it) {
          if (rec.is_string()) {
            r.recommendations_en.push_back(Utf8ToWide(rec.get<std::string>()));
          }
        }
      }

      if (!r.id.empty()) {
        rules.push_back(std::move(r));
      }
    }

    m_groups = std::move(groups);
    m_rules = std::move(rules);
    return !m_rules.empty();
  } catch (...) {
    return false;
  }
}

GraphicsEnvironment GraphicsInjectionDiag::DetectEnvironment(const std::vector<std::wstring>& moduleFilenames) const
{
  GraphicsEnvironment env{};
  if (moduleFilenames.empty() || m_groups.empty()) {
    return env;
  }

  std::unordered_set<std::wstring> moduleLowerSet;
  moduleLowerSet.reserve(moduleFilenames.size());
  for (const auto& mod : moduleFilenames) {
    moduleLowerSet.insert(WideLower(mod));
  }

  std::unordered_set<std::wstring> added;
  for (const auto& group : m_groups) {
    if (!HasAny(moduleLowerSet, group.dlls)) {
      continue;
    }

    if (group.name == L"enb") {
      env.enb_detected = true;
    } else if (group.name == L"reshade") {
      env.reshade_detected = true;
    } else if (group.name == L"dxvk") {
      env.dxvk_detected = true;
    }

    for (const auto& mod : moduleFilenames) {
      const std::wstring modLower = WideLower(mod);
      if (std::find(group.dlls.begin(), group.dlls.end(), modLower) == group.dlls.end()) {
        continue;
      }
      if (added.insert(modLower).second) {
        env.injection_modules.push_back(mod);
      }
    }
  }

  return env;
}

std::optional<GraphicsDiagResult> GraphicsInjectionDiag::Diagnose(
  const std::vector<std::wstring>& moduleFilenames,
  const std::wstring& faultModuleFilename,
  bool useKorean) const
{
  if (m_rules.empty() || moduleFilenames.empty()) {
    return std::nullopt;
  }

  std::unordered_set<std::wstring> moduleLowerSet;
  moduleLowerSet.reserve(moduleFilenames.size());
  for (const auto& mod : moduleFilenames) {
    moduleLowerSet.insert(WideLower(mod));
  }
  const std::wstring faultLower = WideLower(faultModuleFilename);

  for (const auto& rule : m_rules) {
    if (!HasAny(moduleLowerSet, rule.modules_any)) {
      continue;
    }
    if (!HasAll(moduleLowerSet, rule.modules_all)) {
      continue;
    }
    if (!rule.fault_module_any.empty() &&
        std::find(rule.fault_module_any.begin(), rule.fault_module_any.end(), faultLower) == rule.fault_module_any.end()) {
      continue;
    }

    GraphicsDiagResult out{};
    out.rule_id = rule.id;
    out.cause = useKorean ? rule.cause_ko : rule.cause_en;
    out.recommendations = useKorean ? rule.recommendations_ko : rule.recommendations_en;
    out.confidence_level = ParseConfidenceLevel(rule.confidence);
    out.confidence = std::wstring(i18n::ConfidenceLabel(
      useKorean ? i18n::Language::kKorean : i18n::Language::kEnglish,
      out.confidence_level));
    return out;
  }

  return std::nullopt;
}

std::size_t GraphicsInjectionDiag::RuleCount() const
{
  return m_rules.size();
}

}  // namespace skydiag::dump_tool
