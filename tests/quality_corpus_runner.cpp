#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "CandidateConsensus.h"

namespace {

using skydiag::dump_tool::ActionableCandidate;
using skydiag::dump_tool::BuildCandidateConsensus;
using skydiag::dump_tool::CandidateSignal;
using skydiag::dump_tool::i18n::Language;

constexpr std::string_view kFixtureKind = "skydiag.candidate_consensus_signals.v1";

std::wstring AsciiToWide(const std::string& value, std::string_view field)
{
  std::wstring result;
  result.reserve(value.size());
  for (const unsigned char ch : value) {
    if (ch > 0x7Fu) {
      throw std::runtime_error(
        "quality fixture field '" + std::string(field) + "' must use ASCII text");
    }
    result.push_back(static_cast<wchar_t>(ch));
  }
  return result;
}

std::string WideToAscii(const std::wstring& value, std::string_view field)
{
  std::string result;
  result.reserve(value.size());
  for (const wchar_t ch : value) {
    if (ch < 0 || static_cast<std::uint32_t>(ch) > 0x7Fu) {
      throw std::runtime_error(
        "generated candidate field '" + std::string(field) + "' is not ASCII");
    }
    result.push_back(static_cast<char>(ch));
  }
  return result;
}

std::string RequiredString(const nlohmann::json& object, std::string_view field)
{
  const auto key = std::string(field);
  if (!object.contains(key) || !object.at(key).is_string()) {
    throw std::runtime_error("missing string field '" + key + "'");
  }
  return object.at(key).get<std::string>();
}

std::wstring OptionalWideString(const nlohmann::json& object, std::string_view field)
{
  const auto key = std::string(field);
  if (!object.contains(key)) {
    return {};
  }
  if (!object.at(key).is_string()) {
    throw std::runtime_error("field '" + key + "' must be a string");
  }
  return AsciiToWide(object.at(key).get<std::string>(), field);
}

CandidateSignal ParseSignal(const nlohmann::json& value)
{
  if (!value.is_object()) {
    throw std::runtime_error("each signals entry must be an object");
  }

  CandidateSignal signal{};
  signal.family_id = RequiredString(value, "family_id");
  signal.candidate_key = AsciiToWide(RequiredString(value, "candidate_key"), "candidate_key");
  signal.display_name = OptionalWideString(value, "display_name");
  signal.plugin_name = OptionalWideString(value, "plugin_name");
  signal.mod_name = OptionalWideString(value, "mod_name");
  signal.module_filename = OptionalWideString(value, "module_filename");
  signal.detail = OptionalWideString(value, "detail");
  if (!value.contains("weight") || !value.at("weight").is_number_unsigned()) {
    throw std::runtime_error("signal weight must be an unsigned integer");
  }
  signal.weight = value.at("weight").get<std::uint32_t>();
  return signal;
}

nlohmann::json CandidateJson(const ActionableCandidate& candidate)
{
  return {
    { "confidence", WideToAscii(candidate.confidence, "confidence") },
    { "status_id", candidate.status_id },
    { "display_name", WideToAscii(candidate.display_name, "display_name") },
    { "primary_identifier", WideToAscii(candidate.primary_identifier, "primary_identifier") },
    { "secondary_label", WideToAscii(candidate.secondary_label, "secondary_label") },
    { "plugin_name", WideToAscii(candidate.plugin_name, "plugin_name") },
    { "mod_name", WideToAscii(candidate.mod_name, "mod_name") },
    { "module_filename", WideToAscii(candidate.module_filename, "module_filename") },
    { "supporting_families", candidate.supporting_families },
    { "conflicting_families", candidate.conflicting_families },
    { "score", candidate.score },
    { "family_count", candidate.family_count },
    { "cross_validated", candidate.cross_validated },
    { "has_conflict", candidate.has_conflict },
  };
}

void VerifyExpectedCandidates(
  const nlohmann::json& fixture,
  const nlohmann::json& actual,
  const std::filesystem::path& path)
{
  if (!fixture.contains("expected_candidates") || !fixture.at("expected_candidates").is_array()) {
    throw std::runtime_error(path.string() + ": expected_candidates must be an array");
  }
  const auto& expected = fixture.at("expected_candidates");
  if (expected.size() != actual.size()) {
    throw std::runtime_error(
      path.string() + ": expected " + std::to_string(expected.size()) +
      " candidate(s), got " + std::to_string(actual.size()) + "\nactual=" + actual.dump(2));
  }

  for (std::size_t index = 0; index < expected.size(); ++index) {
    if (!expected.at(index).is_object()) {
      throw std::runtime_error(path.string() + ": expected candidate must be an object");
    }
    for (auto it = expected.at(index).begin(); it != expected.at(index).end(); ++it) {
      if (!actual.at(index).contains(it.key()) || actual.at(index).at(it.key()) != it.value()) {
        throw std::runtime_error(
          path.string() + ": candidate " + std::to_string(index) + " field '" + it.key() +
          "' changed\nexpected=" + it.value().dump() +
          "\nactual=" + actual.at(index).value(it.key(), nlohmann::json{}).dump());
      }
    }
  }
}

void ValidateScenarioId(std::string_view value)
{
  if (value.empty() || !std::all_of(value.begin(), value.end(), [](const char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
      })) {
    throw std::runtime_error("scenario_id must contain only lowercase ASCII letters, digits, '_' or '-'");
  }
}

void GenerateSummary(const std::filesystem::path& fixturePath, const std::filesystem::path& outputDir)
{
  std::ifstream input(fixturePath, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open fixture: " + fixturePath.string());
  }

  nlohmann::json fixture;
  input >> fixture;
  if (RequiredString(fixture, "_fixture_kind") != kFixtureKind) {
    throw std::runtime_error(fixturePath.string() + ": unsupported _fixture_kind");
  }

  const auto scenarioId = RequiredString(fixture, "scenario_id");
  ValidateScenarioId(scenarioId);
  const auto groundTruth = RequiredString(fixture, "ground_truth_mod");
  const auto intent = RequiredString(fixture, "_fixture_intent");
  if (!fixture.contains("signals") || !fixture.at("signals").is_array()) {
    throw std::runtime_error(fixturePath.string() + ": signals must be an array");
  }

  std::vector<CandidateSignal> signals;
  signals.reserve(fixture.at("signals").size());
  for (const auto& value : fixture.at("signals")) {
    signals.push_back(ParseSignal(value));
  }

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  nlohmann::json candidateRows = nlohmann::json::array();
  for (const auto& candidate : candidates) {
    candidateRows.push_back(CandidateJson(candidate));
  }
  VerifyExpectedCandidates(fixture, candidateRows, fixturePath);

  nlohmann::json summary = {
    { "_generated_by", "skydiag_quality_corpus_runner" },
    { "_fixture_intent", intent },
    { "synthetic_regression_fixture", true },
    { "schema_version", 2 },
    { "crash_bucket_key", "synthetic::" + scenarioId },
    { "dump_path", "synthetic/" + scenarioId + ".dmp" },
    { "incident", { { "incident_id", "synthetic-" + scenarioId } } },
    { "triage", {
        { "review_status", "synthetic_fixture" },
        { "ground_truth_mod", groundTruth },
        { "verdict", "behavior_contract" },
        { "notes", "Synthetic regression expectation; not a reviewed real incident." },
      } },
    { "exception", {
        { "module_plus_offset", fixture.value("fault_module_plus_offset", "<unknown>") },
      } },
    { "actionable_candidates", std::move(candidateRows) },
    { "suspects", nlohmann::json::array() },
  };

  const auto outputPath = outputDir / (scenarioId + "_SkyrimDiagSummary.json");
  std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("failed to open generated summary: " + outputPath.string());
  }
  output << summary.dump(2) << '\n';
  if (!output) {
    throw std::runtime_error("failed to write generated summary: " + outputPath.string());
  }
}

}  // namespace

int main(int argc, char** argv)
{
  try {
    if (argc != 3) {
      std::cerr << "usage: skydiag_quality_corpus_runner <fixture-dir> <output-dir>\n";
      return 2;
    }

    const std::filesystem::path fixtureDir(argv[1]);
    const std::filesystem::path outputDir(argv[2]);
    if (!std::filesystem::is_directory(fixtureDir)) {
      throw std::runtime_error("fixture directory not found: " + fixtureDir.string());
    }
    std::filesystem::create_directories(outputDir);

    std::vector<std::filesystem::path> fixtures;
    for (const auto& entry : std::filesystem::directory_iterator(fixtureDir)) {
      const auto filename = entry.path().filename().string();
      if (entry.is_regular_file() && filename.ends_with("_signals.json")) {
        fixtures.push_back(entry.path());
      }
    }
    std::sort(fixtures.begin(), fixtures.end());
    if (fixtures.empty()) {
      throw std::runtime_error("no *_signals.json fixtures found in " + fixtureDir.string());
    }

    for (const auto& fixture : fixtures) {
      GenerateSummary(fixture, outputDir);
    }
    std::cout << "generated " << fixtures.size()
              << " synthetic summaries through BuildCandidateConsensus\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "quality corpus generation failed: " << error.what() << '\n';
    return 1;
  }
}
