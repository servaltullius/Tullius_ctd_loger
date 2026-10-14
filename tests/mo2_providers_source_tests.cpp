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

}  // namespace

int main()
{
  try {
    TestAnalysisTimeProvidersAreLabelledAndCapped();
    TestCaptureInTheScanWinsOverTheDisk();
    std::puts("mo2 providers source tests passed");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
