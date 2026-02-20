#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "PluginRules.h"

namespace skydiag::dump_tool {

// Test-local UTF bridge to keep this Linux test independent from Win32 Utf.cpp.
std::wstring Utf8ToWide(std::string_view s)
{
  std::wstring out;
  out.reserve(s.size());
  for (const unsigned char c : s) {
    out.push_back(static_cast<wchar_t>(c));
  }
  return out;
}

std::string WideToUtf8(std::wstring_view w)
{
  std::string out;
  out.reserve(w.size());
  for (const wchar_t c : w) {
    out.push_back(static_cast<char>(c & 0xFF));
  }
  return out;
}

}  // namespace skydiag::dump_tool

namespace {

using skydiag::dump_tool::ComputeMissingMasters;
using skydiag::dump_tool::IsGameVersionLessThan;
using skydiag::dump_tool::ParsePluginScanJson;
using skydiag::dump_tool::ParsedPluginScan;
using skydiag::dump_tool::PluginRules;
using skydiag::dump_tool::PluginRulesContext;

const char* kScanJson = R"JSON(
{
  "game_exe_version": "1.6.640.0",
  "plugins_source": "mo2_profile",
  "mo2_detected": true,
  "plugins": [
    {
      "filename": "A.esm",
      "header_version": 1.0,
      "is_esl": false,
      "is_active": true,
      "masters": []
    },
    {
      "filename": "B.esp",
      "header_version": 1.71,
      "is_esl": true,
      "is_active": true,
      "masters": ["A.esm", "MissingMaster.esm"]
    },
    {
      "filename": "DisabledPatch.esp",
      "header_version": 1.0,
      "is_esl": false,
      "is_active": false,
      "masters": ["InactiveOnlyMissing.esm"]
    }
  ]
}
)JSON";

void TestVersionCompare()
{
  assert(IsGameVersionLessThan("1.6.640", "1.6.1130"));
  assert(!IsGameVersionLessThan("1.6.1130", "1.6.640"));
  assert(!IsGameVersionLessThan("1.6.1130", "1.6.1130"));
  assert(IsGameVersionLessThan("1.6.1130.9", "1.6.1131"));
}

void TestParseAndMissingMasters()
{
  ParsedPluginScan scan{};
  assert(ParsePluginScanJson(kScanJson, &scan));
  assert(scan.plugins.size() == 3);
  assert(scan.plugins[1].header_version >= 1.70f);
  assert(scan.plugins[1].is_esl);
  const auto missing = ComputeMissingMasters(scan);
  assert(missing.size() == 1);
  assert(missing[0] == L"MissingMaster.esm");
}

void TestMissingMastersIgnoreInactivePlugins()
{
  const char* inactiveOnlyJson = R"JSON(
{
  "plugins": [
    {
      "filename": "OnlyInactive.esp",
      "header_version": 1.0,
      "is_esl": false,
      "is_active": false,
      "masters": ["ShouldNotCount.esm"]
    }
  ]
}
)JSON";
  ParsedPluginScan scan{};
  assert(ParsePluginScanJson(inactiveOnlyJson, &scan));
  const auto missing = ComputeMissingMasters(scan);
  assert(missing.empty());
}

void TestRulesEvaluateFromJson()
{
  const auto tmp = std::filesystem::temp_directory_path() / "skydiag_plugin_rules_logic_test.json";
  const std::string rulesJson = R"JSON(
{
  "version": 1,
  "rules": [
    {
      "id": "HEADER_171_WITHOUT_BEES",
      "condition": {
        "any_plugin_header_version_gte": 1.71,
        "game_version_lt": "1.6.1130",
        "module_not_loaded": "bees.dll"
      },
      "diagnosis": {
        "cause_ko": "ko",
        "cause_en": "bees missing",
        "confidence": "high",
        "recommendations_ko": ["ko1"],
        "recommendations_en": ["en1"]
      }
    },
    {
      "id": "MISSING_MASTER",
      "condition": {
        "has_missing_master": true
      },
      "diagnosis": {
        "cause_ko": "ko",
        "cause_en": "missing master",
        "confidence": "high",
        "recommendations_ko": ["ko2"],
        "recommendations_en": ["en2"]
      }
    }
  ]
}
)JSON";

  {
    std::ofstream f(tmp);
    assert(f.is_open());
    f << rulesJson;
  }

  ParsedPluginScan scan{};
  assert(ParsePluginScanJson(kScanJson, &scan));

  PluginRules rules;
  assert(rules.LoadFromJson(tmp));

  PluginRulesContext ctx{};
  ctx.scan = &scan;
  ctx.game_version = "1.6.640";
  ctx.use_korean = false;
  ctx.missing_masters = ComputeMissingMasters(scan);
  ctx.loaded_module_filenames = {L"skyrimse.exe", L"d3d11.dll"};

  const auto diags = rules.Evaluate(ctx);
  assert(diags.size() == 2);
  std::unordered_set<std::string> ids;
  for (const auto& d : diags) {
    ids.insert(d.rule_id);
  }
  assert(ids.count("HEADER_171_WITHOUT_BEES") == 1);
  assert(ids.count("MISSING_MASTER") == 1);

  PluginRulesContext ctxWithBees = ctx;
  ctxWithBees.loaded_module_filenames.push_back(L"bees.dll");
  const auto diagsWithBees = rules.Evaluate(ctxWithBees);
  assert(diagsWithBees.size() == 1);
  assert(diagsWithBees[0].rule_id == "MISSING_MASTER");

  std::error_code ec;
  std::filesystem::remove(tmp, ec);
}

}  // namespace

int main()
{
  TestVersionCompare();
  TestParseAndMissingMasters();
  TestMissingMastersIgnoreInactivePlugins();
  TestRulesEvaluateFromJson();
  return 0;
}
