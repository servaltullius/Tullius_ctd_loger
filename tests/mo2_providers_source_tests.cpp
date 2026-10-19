// Audit F05: MO2 providers named in a report must come from the profile the
// game ran with. A dump analyzed later used whatever profile MO2 had on disk by
// then; the helper now captures the profile with the dump, and providers read
// from disk at analysis time are labelled as candidates with low confidence.

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "Analyzer.h"
#include "AnalyzerPipeline.h"
#include "EvidenceBuilder.h"

namespace {

using skydiag::dump_tool::AnalysisResult;
using skydiag::dump_tool::BuildEvidenceAndSummary;
using skydiag::dump_tool::BuildMo2IndexForDump;
using skydiag::dump_tool::EvidenceItem;
using skydiag::dump_tool::ResourceRow;
namespace i18n = skydiag::dump_tool::i18n;

void Require(bool condition, const char* message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void WriteText(const std::filesystem::path& path, const std::string& text)
{
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << text;
  Require(static_cast<bool>(out), "failed to write a test file");
}

std::string U8(const std::filesystem::path& path)
{
  const auto s = path.u8string();
  return std::string(s.begin(), s.end());
}

const EvidenceItem* FindEvidence(const AnalysisResult& r, std::wstring_view title)
{
  for (const auto& e : r.evidence) {
    if (e.title == title) {
      return &e;
    }
  }
  return nullptr;
}

AnalysisResult MakeCrashWithConflict(std::string source)
{
  AnalysisResult r{};
  r.dump_path = L"C:\\Logs\\SkyrimDiag_Crash_20261014_120000_000.dmp";
  r.exc_code = 0xC0000005u;
  r.fault_module_filename = L"SkyrimSE.exe";
  ResourceRow rr{};
  rr.t_ms = 1000.0;
  rr.kind = L"mesh";
  rr.path = L"meshes\\armor\\test.nif";
  rr.providers = { L"Mod A", L"Mod B" };
  rr.is_conflict = true;
  r.resources.push_back(rr);
  r.mo2_providers_source = std::move(source);
  r.mo2_profile_name = L"Profile X";
  return r;
}

void TestAnalysisTimeProvidersAreLabelledAndCapped()
{
  for (const auto lang : { i18n::Language::kEnglish, i18n::Language::kKorean }) {
    const bool en = lang == i18n::Language::kEnglish;
    const std::wstring_view conflictTitle =
      en ? L"Same file provided by multiple mods (possible conflict)" : L"동일 파일을 여러 모드가 제공(충돌 가능)";

    auto later = MakeCrashWithConflict("analysis_time");
    BuildEvidenceAndSummary(later, lang);
    const auto* laterConflict = FindEvidence(later, conflictTitle);
    Require(laterConflict != nullptr, "the conflict evidence is still shown");
    Require(laterConflict->confidence_level == i18n::ConfidenceLevel::kLow,
      "providers read at analysis time are not trusted beyond low confidence");
    Require(laterConflict->details.find(en ? L"at analysis time" : L"분석 시점") != std::wstring::npos,
      "the details say the providers come from the installation at analysis time");
    Require(laterConflict->details.find(L"Profile X") != std::wstring::npos, "the profile read at analysis is named");

    auto captured = MakeCrashWithConflict("crash_time");
    BuildEvidenceAndSummary(captured, lang);
    const auto* capturedConflict = FindEvidence(captured, conflictTitle);
    Require(capturedConflict != nullptr, "the conflict evidence is shown with a capture");
    Require(capturedConflict->confidence_level == i18n::ConfidenceLevel::kMedium,
      "providers from the crash-time profile keep their confidence");
    Require(capturedConflict->details.find(en ? L"at analysis time" : L"분석 시점") == std::wstring::npos,
      "no caveat when the providers were captured with the dump");
  }
}

bool CandidateHasResourceSupport(const AnalysisResult& r, std::wstring_view name)
{
  for (const auto& c : r.actionable_candidates) {
    if (c.mod_name != name) {
      continue;
    }
    for (const auto& family : c.supporting_families) {
      if (family == "resource_provider") {
        return true;
      }
    }
  }
  return false;
}

bool HasCandidate(const AnalysisResult& r, std::wstring_view name)
{
  for (const auto& c : r.actionable_candidates) {
    if (c.mod_name == name) {
      return true;
    }
  }
  return false;
}

// A stack candidate whose mod also provided the resource opened just before
// the crash. Only a crash-time provider may count as support for it.
AnalysisResult MakeStackCandidateWithNearbyResource(std::string source)
{
  AnalysisResult r{};
  r.dump_path = L"C:\\Logs\\SkyrimDiag_Crash_20261014_120000_000.dmp";
  r.exc_code = 0xC0000005u;
  r.fault_module_filename = L"SkyrimSE.exe";
  r.suspects_from_stackwalk = true;
  skydiag::dump_tool::SuspectItem suspect{};
  suspect.module_filename = L"ModA.dll";
  suspect.module_path = L"C:\\MO2\\mods\\Mod A\\SKSE\\Plugins\\ModA.dll";
  suspect.inferred_mod_name = L"Mod A";
  suspect.confidence_level = i18n::ConfidenceLevel::kMedium;
  suspect.score = 10;
  r.suspects.push_back(suspect);
  ResourceRow rr{};
  rr.t_ms = 1000.0;
  rr.kind = L"nif";
  rr.path = L"meshes\\moda\\thing.nif";
  rr.providers = { L"Mod A" };
  r.resources.push_back(rr);
  r.resources.push_back(rr);
  r.mo2_providers_source = std::move(source);
  return r;
}

void TestAnalysisTimeProvidersDoNotSupportCandidates()
{
  auto captured = MakeStackCandidateWithNearbyResource("crash_time");
  BuildEvidenceAndSummary(captured, i18n::Language::kEnglish);
  Require(HasCandidate(captured, L"Mod A"), "the stack suspect becomes a candidate");
  Require(CandidateHasResourceSupport(captured, L"Mod A"),
    "a crash-time provider of a nearby resource supports the candidate");

  auto later = MakeStackCandidateWithNearbyResource("analysis_time");
  BuildEvidenceAndSummary(later, i18n::Language::kEnglish);
  Require(HasCandidate(later, L"Mod A"), "the stack candidate stays");
  Require(!CandidateHasResourceSupport(later, L"Mod A"),
    "a provider read at analysis time may belong to another profile and does not support it");
}

void TestCaptureInTheScanWinsOverTheDisk()
{
  const auto base = std::filesystem::temp_directory_path() / "skydiag_mo2_providers_source_test";
  std::error_code ec;
  std::filesystem::remove_all(base, ec);
  WriteText(base / "ModOrganizer.ini", "[General]\nselected_profile=@ByteArray(Now)\n");
  WriteText(base / "profiles" / "Now" / "modlist.txt", "+Now Mod\n");
  WriteText(base / "mods" / "Now Mod" / "x.txt", "x");
  const std::vector<std::wstring> modulePaths = { base.wstring() + L"\\mods\\Now Mod\\x.dll" };

  AnalysisResult withCapture{};
  withCapture.plugin_scan_json_utf8 = nlohmann::json{
    { "mo2",
      { { "status", "captured" },
        { "base_dir", U8(base) },
        { "mods_dir", U8(base / "mods") },
        { "profiles_dir", U8(base / "profiles") },
        { "overwrite_dir", U8(base / "overwrite") },
        { "profile", "At Crash" },
        { "enabled_mods", { "Crash Mod" } } } },
  }.dump();
  const auto captured = BuildMo2IndexForDump(modulePaths, withCapture);

  AnalysisResult oldDump{};  // a scan from before the capture existed
  oldDump.plugin_scan_json_utf8 = R"({"plugins_source":"mo2_profile","plugins":[]})";
  const auto fromDisk = BuildMo2IndexForDump(modulePaths, oldDump);
  std::filesystem::remove_all(base, ec);

  Require(captured.has_value() && captured->profileName == L"At Crash", "the captured profile is used");
  Require(withCapture.mo2_providers_source == "crash_time" && withCapture.mo2_profile_name == L"At Crash",
    "the result records a crash-time source");
  Require(fromDisk.has_value() && fromDisk->profileName == L"Now", "without a capture the disk is read");
  Require(oldDump.mo2_providers_source == "analysis_time", "and the result says the providers are from analysis time");
}

// Review M16 follow-up: the MO2 folders come from the dump (module paths, the
// helper's plugin scan). One that is not on a local fixed disk is not read,
// so a crafted dump cannot make the analyzer list a UNC share; a "\\?\"
// path to a real local folder stands in for it here. A captured mod or
// profile name is one folder name, never a path of its own.
void TestFoldersOffALocalFixedDiskAreNotRead()
{
  const auto base = std::filesystem::temp_directory_path() / "skydiag_mo2_local_folder_test";
  std::error_code ec;
  std::filesystem::remove_all(base, ec);
  WriteText(base / "ModOrganizer.ini", "[General]\nselected_profile=@ByteArray(Now)\n");
  WriteText(base / "profiles" / "Now" / "modlist.txt", "+Now Mod\n");
  WriteText(base / "mods" / "Now Mod" / "x.txt", "x");
  const std::filesystem::path other(L"\\\\?\\" + base.wstring());
  const std::vector<std::wstring> otherModulePaths = { other.wstring() + L"\\mods\\Now Mod\\x.dll" };

  AnalysisResult fromModules{};
  const auto modulesIdx = BuildMo2IndexForDump(otherModulePaths, fromModules);

  AnalysisResult fromCapture{};
  fromCapture.plugin_scan_json_utf8 = nlohmann::json{
    { "mo2",
      { { "status", "captured" },
        { "base_dir", U8(other) },
        { "mods_dir", U8(other / "mods") },
        { "profiles_dir", U8(other / "profiles") },
        { "overwrite_dir", U8(other / "overwrite") },
        { "profile", "At Crash" },
        { "enabled_mods", { "Crash Mod" } } } },
  }.dump();
  const auto captureIdx = BuildMo2IndexForDump(otherModulePaths, fromCapture);

  AnalysisResult names{};
  names.plugin_scan_json_utf8 = nlohmann::json{
    { "mo2",
      { { "status", "captured" },
        { "base_dir", U8(base) },
        { "mods_dir", U8(base / "mods") },
        { "profiles_dir", U8(base / "profiles") },
        { "overwrite_dir", U8(base / "overwrite") },
        { "profile", "..\\..\\Elsewhere" },
        { "enabled_mods", { "Good Mod", "\\\\server\\share", "..", "..\\Up", "C:\\Abs", "D:Rel", "Also/Bad" } } } },
  }.dump();
  const auto namesIdx = BuildMo2IndexForDump({}, names);
  std::filesystem::remove_all(base, ec);

  Require(!modulesIdx.has_value() && fromModules.mo2_providers_source.empty(),
    "an MO2 folder off a local fixed disk named by module paths is not read");
  Require(!captureIdx.has_value() && fromCapture.mo2_providers_source.empty(),
    "captured MO2 folders off a local fixed disk are not read");
  Require(namesIdx.has_value() && namesIdx->modNames == std::vector<std::wstring>{ L"Good Mod" } &&
            namesIdx->modDirs.size() == 1u && namesIdx->modDirs[0] == base / "mods" / "Good Mod",
    "a captured mod name that is not one plain folder name is dropped");
  Require(namesIdx->profileDir.empty() && namesIdx->modlistPath.empty(),
    "a captured profile name that is not one plain folder name names no folder");
}

}  // namespace

int main()
{
  try {
    TestAnalysisTimeProvidersAreLabelledAndCapped();
    TestAnalysisTimeProvidersDoNotSupportCandidates();
    TestCaptureInTheScanWinsOverTheDisk();
    TestFoldersOffALocalFixedDiskAreNotRead();
    std::puts("mo2 providers source tests passed");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
