#include "TroubleshootingGuide.h"
#include "Utf.h"

#include <cstdio>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

namespace skydiag::dump_tool {

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
    if (!j.contains("guides") || !j["guides"].is_array()) {
      return false;
    }

    for (const auto& guide : j["guides"]) {
      if (!guide.is_object() || !guide.contains("match")) {
        continue;
      }

      Guide g{};
      const auto& match = guide["match"];
      g.match_exc_code = match.value("exc_code", "");
      g.match_signature_id = match.value("signature_id", "");
      g.match_state_flags = match.value("state_flags_contains", "");
      g.title_en = guide.value("title_en", "");
      g.title_ko = guide.value("title_ko", "");

      auto loadSteps = [&](const char* key, std::vector<std::string>& out) {
        if (guide.contains(key) && guide[key].is_array()) {
          for (const auto& step : guide[key]) {
            if (step.is_string()) {
              out.push_back(step.get<std::string>());
            }
          }
        }
      };
      loadSteps("steps_en", g.steps_en);
      loadSteps("steps_ko", g.steps_ko);

      m_guides.push_back(std::move(g));
    }

    return true;
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
