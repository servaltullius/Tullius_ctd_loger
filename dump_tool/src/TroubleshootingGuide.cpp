#include "TroubleshootingGuide.h"
#include "Utf.h"

#include <cstdio>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

namespace skydiag::dump_tool {
namespace {

constexpr int kTroubleshootingGuideSchemaVersion = 1;

bool TryReadOptionalString(const nlohmann::json& obj, const char* key, std::string* out)
{
  if (!out) {
    return false;
  }
  out->clear();
  if (!obj.contains(key)) {
    return true;
  }
  const auto& value = obj[key];
  if (!value.is_string()) {
    return false;
  }
  *out = value.get<std::string>();
  return true;
}

bool TryReadStepsArray(const nlohmann::json& guide, const char* key, std::vector<std::string>* out)
{
  if (!out) {
    return false;
  }
  out->clear();
  if (!guide.contains(key)) {
    return true;
  }
  const auto& value = guide[key];
  if (!value.is_array()) {
    return false;
  }
  for (const auto& step : value) {
    if (!step.is_string()) {
      return false;
    }
    out->push_back(step.get<std::string>());
  }
  return true;
}

bool IsSupportedStateFlag(const std::string& stateFlag)
{
  return stateFlag.empty() || stateFlag == "hang" || stateFlag == "loading" || stateFlag == "snapshot";
}

}  // namespace

struct TroubleshootingGuideDatabase::Guide
{
  std::string match_exc_code;       // e.g. "0xC0000005"
  std::string match_signature_id;
  std::string match_state_flags;    // "hang", "loading", "snapshot"
  std::string title_en;
  std::string title_ko;
  std::vector<std::string> steps_en;
  std::vector<std::string> steps_ko;
};

TroubleshootingGuideDatabase::TroubleshootingGuideDatabase() = default;
TroubleshootingGuideDatabase::~TroubleshootingGuideDatabase() = default;
TroubleshootingGuideDatabase::TroubleshootingGuideDatabase(TroubleshootingGuideDatabase&&) noexcept = default;
TroubleshootingGuideDatabase& TroubleshootingGuideDatabase::operator=(TroubleshootingGuideDatabase&&) noexcept = default;

bool TroubleshootingGuideDatabase::LoadFromJson(const std::filesystem::path& jsonPath)
{
  m_guides.clear();

  try {
    std::ifstream f(jsonPath);
    if (!f.is_open()) {
      return false;
    }

    const auto j = nlohmann::json::parse(f, nullptr, /*allow_exceptions=*/true);
    if (!j.is_object() ||
        !j.contains("version") ||
        !j["version"].is_number_integer() ||
        j["version"].get<int>() != kTroubleshootingGuideSchemaVersion ||
        !j.contains("guides") ||
        !j["guides"].is_array()) {
      return false;
    }

    for (const auto& guide : j["guides"]) {
      if (!guide.is_object() || !guide.contains("match") || !guide["match"].is_object()) {
        continue;
      }

      Guide g{};
      const auto& match = guide["match"];
      if (!TryReadOptionalString(match, "exc_code", &g.match_exc_code) ||
          !TryReadOptionalString(match, "signature_id", &g.match_signature_id) ||
          !TryReadOptionalString(match, "state_flags_contains", &g.match_state_flags) ||
          !TryReadOptionalString(guide, "title_en", &g.title_en) ||
          !TryReadOptionalString(guide, "title_ko", &g.title_ko) ||
          !TryReadStepsArray(guide, "steps_en", &g.steps_en) ||
          !TryReadStepsArray(guide, "steps_ko", &g.steps_ko)) {
        continue;
      }

      if ((g.match_exc_code.empty() && g.match_signature_id.empty() && g.match_state_flags.empty()) ||
          !IsSupportedStateFlag(g.match_state_flags) ||
          (g.title_en.empty() && g.title_ko.empty()) ||
          (g.steps_en.empty() && g.steps_ko.empty())) {
        continue;
      }

      m_guides.push_back(std::move(g));
    }

    return !m_guides.empty();
  } catch (...) {
    return false;
  }
}

std::optional<TroubleshootingResult> TroubleshootingGuideDatabase::Match(
  const TroubleshootingMatchInput& input, i18n::Language lang) const
{
  const bool en = (lang == i18n::Language::kEnglish);

  std::string excHex;
  if (input.exc_code != 0) {
    char buf[32]{};
    snprintf(buf, sizeof(buf), "0x%08X", input.exc_code);
    excHex = buf;
  }

  for (const auto& g : m_guides) {
    bool matched = true;

    if (!g.match_exc_code.empty()) {
      if (g.match_exc_code != excHex) { matched = false; }
    }
    if (matched && !g.match_signature_id.empty()) {
      if (g.match_signature_id != input.signature_id) { matched = false; }
    }
    if (matched && !g.match_state_flags.empty()) {
      if (g.match_state_flags == "hang" && !input.is_hang) { matched = false; }
      if (g.match_state_flags == "loading" && !input.is_loading) { matched = false; }
      if (g.match_state_flags == "snapshot" && !input.is_snapshot) { matched = false; }
    }

    if (matched) {
      TroubleshootingResult result{};
      result.title = Utf8ToWide(en ? (g.title_en.empty() ? g.title_ko : g.title_en)
                                   : (g.title_ko.empty() ? g.title_en : g.title_ko));
      const auto& steps = en ? (g.steps_en.empty() ? g.steps_ko : g.steps_en)
                              : (g.steps_ko.empty() ? g.steps_en : g.steps_ko);
      result.steps.reserve(steps.size());
      for (const auto& s : steps) {
        result.steps.push_back(Utf8ToWide(s));
      }
      return result;
    }
  }

  return std::nullopt;
}

std::size_t TroubleshootingGuideDatabase::Size() const
{
  return m_guides.size();
}

}  // namespace skydiag::dump_tool
