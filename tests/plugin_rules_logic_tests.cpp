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
using skydiag::dump_tool::AnyPluginHeaderVersionGte;
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
  const bool parsed = ParsePluginScanJson(kScanJson, &scan);
  assert(parsed);
  assert(scan.plugins.size() == 3);
  assert(scan.plugins[1].header_version >= 1.70f);
  assert(scan.plugins[1].is_esl);
  const auto missing = ComputeMissingMasters(scan);
  assert(missing.size() == 1);
  assert(missing[0] == L"MissingMaster.esm");
  // Scans made before the helper read skse64.log have no section.
  assert(scan.skse_log.status.empty());
  assert(scan.skse_log.issues.empty());
}

void TestParseSkseLogSection()
{
  const char* scanJson = R"JSON(
{
  "game_exe_version": "1.6.1170.0",
  "plugins": [],
  "skse_log": {
    "status": "matched",
    "skse_version": "2.2.6",
    "checked_count": 322,
    "loaded_count": 320,
    "issue_count": 1,
    "issues": [
      { "dll": "NpcGhostFix.dll", "name": "", "status": "no version data", "code": 0 },
      { "dll": "Missing Dep.dll", "name": "Missing Dep", "status": "couldn't load plugin", "code": 126 },
      { "dll": "", "status": "no version data" },
      "not an object"
    ]
  }
}
)JSON";
  ParsedPluginScan scan{};
  assert(ParsePluginScanJson(scanJson, &scan));
  const auto& log = scan.skse_log;
  assert(log.status == "matched");
  assert(log.skse_version == "2.2.6");
  assert(log.checked_count == 322u);
  assert(log.loaded_count == 320u);
  assert(log.issues.size() == 2u);
  // A count below the listed issues is raised to match them.
  assert(log.issue_count == 2u);
  assert(log.issues[1].plugin_name == "Missing Dep");
  assert(log.issues[1].error_code == 126);

  ParsedPluginScan unmatched{};
  assert(ParsePluginScanJson(R"JSON({ "plugins": [], "skse_log": { "status": "no_matching_log" } })JSON", &unmatched));
  assert(unmatched.skse_log.status == "no_matching_log");
  assert(unmatched.skse_log.issues.empty());
}

void TestDescribeSkseLoadStatus()
{
  using skydiag::dump_tool::DescribeSkseLoadStatus;
  using skydiag::dump_tool::SummarizeSkseLogIssues;
  assert(DescribeSkseLoadStatus("disabled, incompatible with current version of the game", 0, true) ==
         L"made for a different game version");
  assert(DescribeSkseLoadStatus("disabled, incompatible with current version of the game", 0, false) == L"다른 게임 버전용");
  assert(DescribeSkseLoadStatus("couldn't load plugin", 126, true).find(L"a DLL it needs is missing") != std::wstring::npos);
  assert(DescribeSkseLoadStatus("couldn't load plugin", 5, true) == L"could not be loaded (error 5)");
  assert(DescribeSkseLoadStatus("no version data", 0, false).find(L"버전 정보 없음") == 0);
  // Unknown texts (newer SKSE builds) are shown as SKSE wrote them.
  assert(DescribeSkseLoadStatus("disabled, something new", 0, true) == L"disabled, something new");

  skydiag::dump_tool::SkseLogScanInfo log{};
  log.issue_count = 3;
  log.issues.push_back({ "A.dll", "", "no version data", 0 });
  log.issues.push_back({ "B.dll", "B", "disabled, requires newer script extender", 0 });
  assert(SummarizeSkseLogIssues(log, true, 1, L", ") ==
         L"A.dll: no version data (a helper DLL that is not an SKSE plugin, or an old plugin without AE version data) (+2 more)");
  assert(SummarizeSkseLogIssues(log, false, 6, L" | ").find(L" | B.dll: 더 새로운 SKSE 필요 (외 1개)") != std::wstring::npos);
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
  const bool parsed = ParsePluginScanJson(inactiveOnlyJson, &scan);
  assert(parsed);
  const auto missing = ComputeMissingMasters(scan);
  assert(missing.empty());
}

void TestHeaderVersionRuleIgnoresInactivePlugins()
{
  const char* inactiveHeaderJson = R"JSON(
{
  "plugins": [
    {
      "filename": "Disabled171.esp",
      "header_version": 1.71,
      "is_esl": true,
      "is_active": false,
      "masters": []
    },
    {
      "filename": "ActiveLegacy.esp",
      "header_version": 1.0,
      "is_esl": false,
      "is_active": true,
      "masters": []
    }
  ]
}
)JSON";
  ParsedPluginScan scan{};
  const bool parsed = ParsePluginScanJson(inactiveHeaderJson, &scan);
  assert(parsed);
  assert(!AnyPluginHeaderVersionGte(scan, 1.71));
}

void TestMissingMastersIgnoreImplicitRuntimeMasters()
{
  const char* implicitMastersJson = R"JSON(
{
  "plugins": [
    {
      "filename": "MyPatch.esp",
      "header_version": 1.0,
      "is_esl": false,
      "is_active": true,
      "masters": [
        "Skyrim.esm",
        "Update.esm",
        "Dawnguard.esm",
        "HearthFires.esm",
        "Dragonborn.esm",
        "ccbgssse001-fish.esm",
        "CCQDRSSE001-SURVIVALMODE.ESL",
        "ccbgssse037-curios.esl",
        "ccbgssse025-advdsgs.esm",
        "_ResourcePack.esl",
        "ResourcePack.esl",
        "ActuallyMissing.esm"
      ]
    }
  ]
}
)JSON";
  ParsedPluginScan scan{};
  const bool parsed = ParsePluginScanJson(implicitMastersJson, &scan);
  assert(parsed);
  const auto missing = ComputeMissingMasters(scan);
  assert(missing.size() == 1);
  assert(missing[0] == L"ActuallyMissing.esm");
}

// Field case (v0.2.59-rc3): an MO2 profile without the Anniversary Edition
// CC files in plugins.txt made every CC master look missing.
void TestMissingMastersTrustCreationClubMastersOnlyFromImplicitAwareScans()
{
  const char* legacyJson = R"JSON(
{
  "plugins": [
    {
      "filename": "MyPatch.esp",
      "is_active": true,
      "masters": ["ccasvsse001-almsivi.esm", "ccBGSSSE002-ExoticArrows.esl", "ActuallyMissing.esm"]
    }
  ]
}
)JSON";
  ParsedPluginScan legacy{};
  assert(ParsePluginScanJson(legacyJson, &legacy));
  assert(!legacy.implicit_plugins_included);
  const auto legacyMissing = ComputeMissingMasters(legacy);
  assert(legacyMissing.size() == 1);
  assert(legacyMissing[0] == L"ActuallyMissing.esm");

  const char* implicitAwareJson = R"JSON(
{
  "implicit_plugins_included": true,
  "plugins": [
    { "filename": "ccasvsse001-almsivi.esm", "is_active": true, "masters": ["Skyrim.esm"] },
    {
      "filename": "MyPatch.esp",
      "is_active": true,
      "masters": ["ccasvsse001-almsivi.esm", "ccBGSSSE002-ExoticArrows.esl"]
    }
  ]
}
)JSON";
  ParsedPluginScan aware{};
  assert(ParsePluginScanJson(implicitAwareJson, &aware));
  assert(aware.implicit_plugins_included);
  const auto awareMissing = ComputeMissingMasters(aware);
  assert(awareMissing.size() == 1);
  assert(awareMissing[0] == L"ccBGSSSE002-ExoticArrows.esl");
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
  const bool parsed = ParsePluginScanJson(kScanJson, &scan);
  assert(parsed);

  PluginRules rules;
  const bool loaded = rules.LoadFromJson(tmp);
  assert(loaded);

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

void TestFullPluginSlotRuleRequiresActiveFullThreshold()
{
  const auto tmp = std::filesystem::temp_directory_path() / "skydiag_plugin_rules_full_slot_test.json";
  const std::string rulesJson = R"JSON(
{
  "version": 1,
  "rules": [
    {
      "id": "ESP_FULL_SLOT_NEAR_LIMIT",
      "condition": {
        "full_plugin_count_gte": 3
      },
      "diagnosis": {
        "cause_ko": "ko",
        "cause_en": "near full slot",
        "confidence": "high",
        "recommendations_ko": ["ko"],
        "recommendations_en": ["en"]
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

  const char* belowThresholdJson = R"JSON(
{
  "plugins": [
    { "filename": "A.esm", "is_esl": false, "is_active": true, "masters": [] },
    { "filename": "B.esp", "is_esl": false, "is_active": true, "masters": [] },
    { "filename": "Light.esl", "is_esl": true, "is_active": true, "masters": [] },
    { "filename": "Disabled.esp", "is_esl": false, "is_active": false, "masters": [] }
  ]
}
)JSON";

  ParsedPluginScan below{};
  const bool belowParsed = ParsePluginScanJson(belowThresholdJson, &below);
  assert(belowParsed);

  PluginRules rules;
  const bool loaded = rules.LoadFromJson(tmp);
  assert(loaded);

  PluginRulesContext ctx{};
  ctx.scan = &below;
  const auto belowDiags = rules.Evaluate(ctx);
  assert(belowDiags.empty());

  const char* atThresholdJson = R"JSON(
{
  "plugins": [
    { "filename": "A.esm", "is_esl": false, "is_active": true, "masters": [] },
    { "filename": "B.esp", "is_esl": false, "is_active": true, "masters": [] },
    { "filename": "C.esp", "is_esl": false, "is_active": true, "masters": [] },
    { "filename": "Light.esl", "is_esl": true, "is_active": true, "masters": [] }
  ]
}
)JSON";

  ParsedPluginScan at{};
  const bool atParsed = ParsePluginScanJson(atThresholdJson, &at);
  assert(atParsed);
  ctx.scan = &at;
  const auto atDiags = rules.Evaluate(ctx);
  assert(atDiags.size() == 1);
  assert(atDiags[0].rule_id == "ESP_FULL_SLOT_NEAR_LIMIT");

  std::error_code ec;
  std::filesystem::remove(tmp, ec);
}

void TestEslSlotRuleCountsOnlyActivePlugins()
{
  const auto tmp = std::filesystem::temp_directory_path() / "skydiag_plugin_rules_esl_slot_test.json";
  const std::string rulesJson = R"JSON(
{
  "version": 1,
  "rules": [
    {
      "id": "ESL_SLOT_NEAR_LIMIT",
      "condition": {
        "esl_count_gte": 2
      },
      "diagnosis": {
        "cause_ko": "ko",
        "cause_en": "near esl slot",
        "confidence": "low",
        "recommendations_ko": ["ko"],
        "recommendations_en": ["en"]
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

  const char* scanJson = R"JSON(
{
  "plugins": [
    { "filename": "Active.esl", "is_esl": true, "is_active": true, "masters": [] },
    { "filename": "Inactive.esl", "is_esl": true, "is_active": false, "masters": [] }
  ]
}
)JSON";

  ParsedPluginScan scan{};
  const bool parsed = ParsePluginScanJson(scanJson, &scan);
  assert(parsed);

  PluginRules rules;
  const bool loaded = rules.LoadFromJson(tmp);
  assert(loaded);

  PluginRulesContext ctx{};
  ctx.scan = &scan;
  const auto diags = rules.Evaluate(ctx);
  assert(diags.empty());

  std::error_code ec;
  std::filesystem::remove(tmp, ec);
}

}  // namespace

int main()
{
  TestVersionCompare();
  TestParseAndMissingMasters();
  TestParseSkseLogSection();
  TestDescribeSkseLoadStatus();
  TestMissingMastersIgnoreInactivePlugins();
  TestHeaderVersionRuleIgnoresInactivePlugins();
  TestMissingMastersIgnoreImplicitRuntimeMasters();
  TestMissingMastersTrustCreationClubMastersOnlyFromImplicitAwareScans();
  TestRulesEvaluateFromJson();
  TestFullPluginSlotRuleRequiresActiveFullThreshold();
  TestEslSlotRuleCountsOnlyActivePlugins();
  return 0;
}
