// The summary and report the writer builds (BuildSummaryJson /
// BuildReportText), not a hand-written fixture:
// - they keep the SkyrimDiagSummary v2 schema for a minimal and a full result;
// - with path redaction on, no user folder survives anywhere in them (the MO2
//   folders in the plugin scan, a symbol path from _NT_SYMBOL_PATH, paths
//   quoted in Crash Logger's exception text or an error dialog, ...);
// - an unparsable plugin scan preview stays valid UTF-8 and is left out of a
//   redacted summary;
// - the incident manifest is found for the helper's millisecond timestamps.

#include <Windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <nlohmann/json.hpp>

#include "Analyzer.h"
#include "OutputWriterInternals.h"
#include "OutputWriterPipeline.h"
#include "SummarySchemaV2Checks.h"
#include "Utf.h"

namespace {

using skydiag::dump_tool::AnalysisResult;
using skydiag::dump_tool::BuildReportText;
using skydiag::dump_tool::BuildSummaryJson;
using skydiag::dump_tool::ResourceRow;
using skydiag::dump_tool::SuspectItem;
using skydiag::dump_tool::internal::output_writer::FindIncidentManifestForDump;
using skydiag::dump_tool::internal::output_writer::MaybeRedactPath;
using skydiag::dump_tool::internal::output_writer::RedactAbsolutePathStrings;
using skydiag::dump_tool::internal::output_writer::RedactPathsInText;
using skydiag::dump_tool::internal::output_writer::RedactSymbolSearchPath;
using skydiag::tests::summary_schema::ValidateSummarySchemaV2;

constexpr std::string_view kProbeUser = "SkydiagProbeUser";
constexpr std::string_view kProbeUserKo = "홍길동프로브";

void Require(bool condition, const char* message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::filesystem::path MakeTempDir(std::wstring_view suffix)
{
  const auto root = std::filesystem::temp_directory_path() /
    (L"skydiag_summary_writer_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::wstring(suffix));
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  Require(!ec, "temp dir");
  return root;
}

void WriteText(const std::filesystem::path& path, std::string_view text)
{
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  Require(static_cast<bool>(out), "write fixture file");
}

std::string Lower(std::string s)
{
  for (auto& c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return s;
}

// Every string in the summary, so a path is matched unescaped.
void CollectStrings(const nlohmann::json& value, std::string* out)
{
  if (value.is_string()) {
    *out += value.get_ref<const std::string&>();
    *out += '\n';
  } else if (value.is_array() || value.is_object()) {
    for (const auto& child : value) {
      CollectStrings(child, out);
    }
  }
}

AnalysisResult MakeMinimalResult()
{
  AnalysisResult r{};
  r.dump_path = L"SkyrimDiag_Crash_20261019_120000_123.dmp";
  r.summary_sentence = L"minimal";
  r.crash_bucket_key = L"CTD3-0";
  return r;
}

// Every field the writer prints as a path, under a probe user's profile; the
// MO2 folders as the helper's plugin scan writes them; the symbol path as
// DbgHelp reports it when _NT_SYMBOL_PATH set it (no cache path of our own).
AnalysisResult MakeFullResult(const std::filesystem::path& outBase)
{
  const std::wstring home = L"C:\\Users\\SkydiagProbeUser";
  AnalysisResult r{};
  r.dump_path = (outBase / L"SkyrimDiag_Crash_20261019_120000_123.dmp").wstring();
  r.out_dir = outBase.wstring();
  r.summary_sentence = L"full";
  r.crash_bucket_key = L"CTD3-0123";
  r.exc_code = 0xC0000005u;
  r.exc_tid = 4242;
  r.is_crash_like = true;
  r.fault_module_filename = L"BadPlugin.dll";
  r.fault_module_path = home + L"\\AppData\\Local\\ModOrganizer\\Skyrim\\mods\\Bad\\SKSE\\Plugins\\BadPlugin.dll";
  r.fault_module_plus_offset = L"BadPlugin.dll+0x1234";
  r.crash_logger_log_path = home + L"\\Documents\\My Games\\Skyrim Special Edition\\SKSE\\crash-2026-10-19-12-00-00.log";
  r.crash_logger_version = L"v1.25.0";

  SuspectItem s{};
  s.confidence = L"High";
  s.module_filename = L"BadPlugin.dll";
  s.module_path = r.fault_module_path;
  s.inferred_mod_name = L"Bad";
  s.score = 9;
  s.reason = L"stack";
  r.suspects.push_back(s);

  ResourceRow rr{};
  rr.t_ms = 10.0;
  rr.path = home + L"\\Desktop\\loose\\meshes\\a.nif";
  rr.kind = L"nif";
  r.resources.push_back(rr);

  r.symbol_search_path = home + L"\\LocalSyms;srv*" + home + L"\\Symbols*https://msdl.microsoft.com/download/symbols";
  r.dbghelp_path = home + L"\\Tools\\dbghelp.dll";
  r.msdia_path = home + L"\\Tools\\msdia140.dll";

  r.has_plugin_scan = true;
  r.plugin_scan_json_utf8 = R"({"game_exe_version":"1.6.1170.0","plugins_source":"mo2_profile","mo2_detected":true,
    "plugins":[{"filename":"Skyrim.esm","is_active":true,"is_esl":false,"masters":[]}],
    "mo2":{"status":"captured",
      "ini_path":"C:\\Users\\SkydiagProbeUser\\AppData\\Local\\ModOrganizer\\Skyrim\\ModOrganizer.ini",
      "base_dir":"C:\\Users\\SkydiagProbeUser\\AppData\\Local\\ModOrganizer\\Skyrim",
      "mods_dir":"C:\\Users\\SkydiagProbeUser\\AppData\\Local\\ModOrganizer\\Skyrim\\mods",
      "profiles_dir":"C:\\Users\\SkydiagProbeUser\\AppData\\Local\\ModOrganizer\\Skyrim\\profiles",
      "overwrite_dir":"C:\\Users\\SkydiagProbeUser\\AppData\\Local\\ModOrganizer\\Skyrim\\overwrite",
      "profile":"Default","enabled_mods":["Bad"],"enabled_mods_truncated":false}})";

  r.modal_dialog_wait.caller_module_path = home + L"\\mods\\Dialog.dll";
  r.main_thread_wait.blocked_module_path = home + L"\\mods\\Blocked.dll";
  r.hook_stub_race.target_module_path = home + L"\\mods\\Hook.dll";

  // Free text that quotes paths, under the probe user and a Korean one:
  // Crash Logger's exception text, an error dialog (as the report prints it
  // in a freeze), and evidence, advice, steps and diagnostics repeating them.
  const std::wstring koHome = L"C:\\Users\\" + skydiag::dump_tool::Utf8ToWide(std::string(kProbeUserKo));
  r.crash_logger_cpp_exception_type = L"std::filesystem::filesystem_error";
  r.crash_logger_cpp_exception_info = L"filesystem error: cannot open: [" + home + L"\\AppData\\Local\\x.json]";
  r.crash_logger_cpp_exception_throw_location = koHome + L"\\src\\Plugin.cpp:120";
  r.crash_logger_exception_type = L"EXCEPTION_ACCESS_VIOLATION";
  r.crash_logger_exception_location = L"BadPlugin.dll+0x1234";
  r.freeze_analysis.has_analysis = true;
  r.modal_dialog_wait.detected = true;
  r.modal_dialog_wait.dialog_title = L"Bad.dll";
  r.modal_dialog_wait.dialog_text = L"Failed to load \\\\?\\" + home +
    L"\\Documents\\My Games\\Skyrim Special Edition\\Bad.ini / See \\\\nas\\share\\" +
    skydiag::dump_tool::Utf8ToWide(std::string(kProbeUserKo)) + L"\\log.txt";
  skydiag::dump_tool::EvidenceItem e{};
  e.confidence = L"High";
  e.title = L"dialog";
  e.details = L"text: " + r.modal_dialog_wait.dialog_text;
  r.evidence.push_back(e);
  r.recommendations.push_back(L"[Check] Remove " + koHome + L"\\Desktop\\Bad Mod\\Bad.esp, then see https://www.nexusmods.com/skyrimspecialedition/mods/32444");
  r.troubleshooting_title = L"steps";
  r.troubleshooting_steps.push_back(L"Open C:/Users/SkydiagProbeUser/Documents/My Games/Skyrim.ini");
  r.diagnostics.push_back(L"[symbols] search path " + home + L"\\Syms");
  r.stackwalk_primary_frames.push_back(L"BadPlugin.dll!Foo+0x12 (" + home + L"\\src\\foo.cpp:42)");
  return r;
}

void TestWriterOutputKeepsSchema()
{
  const auto outBase = MakeTempDir(L"schema");
  for (const bool redact : { false, true }) {
    const auto minimal = BuildSummaryJson(MakeMinimalResult(), outBase, L"SkyrimDiag_Crash_20261019_120000_123", redact);
    ValidateSummarySchemaV2(minimal);
    const auto full = MakeFullResult(outBase);
    const auto summary = BuildSummaryJson(full, outBase, L"SkyrimDiag_Crash_20261019_120000_123", redact);
    ValidateSummarySchemaV2(summary);
    Require(!summary["suspects"].empty(), "full result keeps its suspect");
    // ADR-0018: one class per recommendation, in the same order.
    Require(minimal["recommendation_classes"].is_array() &&
              minimal["recommendation_classes"].size() == minimal["recommendations"].size(),
      "recommendation_classes follows recommendations");
    Require(summary["recommendation_classes"].size() == summary["recommendations"].size() &&
              summary["recommendation_classes"][0]["group"] == "immediate" &&
              summary["recommendation_classes"][0]["kind"] == "check",
      "a [Check] line is an immediate action");
    Require(summary["privacy"]["path_redaction_applied"].get<bool>() == redact, "privacy flag");
    Require(!BuildReportText(full, summary, redact).empty(), "report text");
  }
  std::filesystem::remove_all(outBase);
}

void TestRedactedOutputsHoldNoUserFolder()
{
  const auto outBase = MakeTempDir(L"privacy");
  const std::wstring stem = L"SkyrimDiag_Crash_20261019_120000_123";
  // The manifest sits in the real temp folder, under the real profile.
  WriteText(outBase / L"SkyrimDiag_Incident_Crash_20261019_120000_123.json",
    R"({"capture_kind":"crash","incident_id":"crash_20261019_120000_123_pid1","artifacts":{"dump":"x.dmp"}})");
  const auto r = MakeFullResult(outBase);

  // Without redaction the probe user is everywhere: the fixture reaches the
  // fields the redacted run must clean.
  {
    const auto summary = BuildSummaryJson(r, outBase, stem, false);
    std::string strings;
    CollectStrings(summary, &strings);
    Require(strings.find(kProbeUser) != std::string::npos, "fixture puts the probe user into the summary");
    Require(strings.find(kProbeUserKo) != std::string::npos, "fixture puts the Korean probe user into the summary");
    Require(summary["plugin_scan"]["mo2"]["base_dir"].get<std::string>().find(kProbeUser) != std::string::npos,
      "unredacted MO2 base_dir");
    Require(summary.contains("incident"), "manifest found");
    const std::string report = BuildReportText(r, summary, false);
    Require(report.find(kProbeUser) != std::string::npos && report.find(kProbeUserKo) != std::string::npos,
      "fixture puts both probe users into the report");
  }

  const auto summary = BuildSummaryJson(r, outBase, stem, true);
  const std::string report = BuildReportText(r, summary, true);
  std::string strings;
  CollectStrings(summary, &strings);
  Require(strings.find(kProbeUser) == std::string::npos, "redacted summary names no user folder");
  Require(report.find(kProbeUser) == std::string::npos, "redacted report names no user folder");
  Require(strings.find(kProbeUserKo) == std::string::npos, "redacted summary names no Korean user folder");
  Require(report.find(kProbeUserKo) == std::string::npos, "redacted report names no Korean user folder");
  Require(report.find(":\\Users") == std::string::npos && report.find(":/Users") == std::string::npos &&
            strings.find(":\\Users") == std::string::npos && strings.find(":/Users") == std::string::npos,
    "no profile folder is left in the redacted outputs");

  // The paths inside text keep their last element; the text around them stays.
  Require(summary["crash_logger"]["cpp_exception"]["info"] == "filesystem error: cannot open: [<redacted>\\x.json]",
    "a path in Crash Logger's exception text");
  Require(summary["crash_logger"]["cpp_exception"]["throw_location"] == "<redacted>\\Plugin.cpp:120",
    "a throw location keeps its file and line");
  Require(summary["freeze_analysis"]["modal_dialog_wait"]["dialog_text"] ==
            "Failed to load <redacted>\\Bad.ini / See <redacted>\\log.txt",
    "paths in the middle of a dialog's text");
  Require(report.find("modal_dialog_text=Failed to load <redacted>\\Bad.ini / See <redacted>\\log.txt\n") !=
            std::string::npos,
    "the report's dialog line is masked the same way");
  Require(summary["recommendations"][0] ==
            "[Check] Remove <redacted>\\Bad.esp, then see https://www.nexusmods.com/skyrimspecialedition/mods/32444",
    "a path in advice text; the URL stays");
  Require(summary["callstack"]["frames"][0] == "BadPlugin.dll!Foo+0x12 (<redacted>\\foo.cpp:42)",
    "a source path in a frame; the module and offset stay");
  Require(summary["exception"]["module_plus_offset"] == "BadPlugin.dll+0x1234" &&
            summary["crash_logger"]["exception"]["location"] == "BadPlugin.dll+0x1234",
    "module names are not paths");
  Require(report.find("Module+Offset: BadPlugin.dll+0x1234\n") != std::string::npos, "report module+offset kept");

  // The temp folder (the manifest, the output base) is under the real profile.
  wchar_t profile[MAX_PATH]{};
  const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    const std::string profileLower = Lower(skydiag::dump_tool::WideToUtf8(profile));
    Require(Lower(strings).find(profileLower) == std::string::npos, "redacted summary holds no real profile path");
    Require(Lower(report).find(profileLower) == std::string::npos, "redacted report holds no real profile path");
  }

  const auto& mo2 = summary["plugin_scan"]["mo2"];
  Require(mo2["base_dir"] == "<redacted>\\Skyrim", "MO2 base_dir keeps only its last folder");
  Require(mo2["ini_path"] == "<redacted>\\ModOrganizer.ini", "MO2 ini_path keeps only its file name");
  Require(mo2["profile"] == "Default" && mo2["status"] == "captured", "non-path MO2 fields stay");
  Require(summary["symbolization"]["search_path"] ==
            "<redacted>\\LocalSyms;srv*<redacted>\\Symbols*https://msdl.microsoft.com/download/symbols",
    "every directory of an env-provided symbol path is redacted, the URL stays");
  Require(summary["incident"]["capture_kind"] == "crash", "incident kept");
  Require(summary["incident"]["manifest_path"] == "<redacted>\\SkyrimDiag_Incident_Crash_20261019_120000_123.json",
    "manifest path redacted");
  std::filesystem::remove_all(outBase);
}

// A path field that is the profile folder itself keeps no account name.
void TestProfileFolderPathField()
{
  const std::wstring ko = skydiag::dump_tool::Utf8ToWide(std::string(kProbeUserKo));
  Require(MaybeRedactPath(L"C:\\Users\\" + ko, true) == L"<redacted>\\<user>", "a Korean profile folder");
  Require(MaybeRedactPath(L"C:/users/SkydiagProbeUser", true) == L"<redacted>\\<user>", "forward slashes, any case");
  Require(MaybeRedactPath(L"\\\\?\\C:\\Users\\" + ko + L"\\", true) == L"<redacted>", "a trailing separator");
  Require(MaybeRedactPath(L"C:\\Users\\" + ko + L"\\Symbols", true) == L"<redacted>\\Symbols",
    "a folder under the profile keeps its own name");
  Require(MaybeRedactPath(L"C:\\Users", true) == L"<redacted>\\Users", "the Users folder names nobody");
  Require(MaybeRedactPath(L"C:\\Users\\" + ko, false) == L"C:\\Users\\" + ko, "redaction off");
  Require(RedactSymbolSearchPath(L"C:\\Users\\" + ko + L";srv*D:\\Users\\Kim*https://msdl.microsoft.com/download/symbols",
            true) == L"<redacted>\\<user>;srv*<redacted>\\<user>*https://msdl.microsoft.com/download/symbols",
    "profile folders in a symbol path");
}

void TestSymbolSearchPathRedaction()
{
  Require(RedactSymbolSearchPath(L"C:\\Users\\A\\Sym", false) == L"C:\\Users\\A\\Sym", "redaction off");
  Require(RedactSymbolSearchPath(L"", true).empty(), "empty");
  Require(RedactSymbolSearchPath(L"cache*C:\\Users\\A\\Cache;\\\\server\\share\\syms;relative", true) ==
            L"cache*<redacted>\\Cache;<redacted>\\syms;relative",
    "cache chains and UNC shares are redacted; relative elements stay");
  Require(RedactSymbolSearchPath(L" D:\\Sym ; srv*https://msdl.microsoft.com/download/symbols", true) ==
            L" <redacted>\\Sym ; srv*https://msdl.microsoft.com/download/symbols",
    "blanks around elements are kept and do not hide a path");
}

void TestPathsInTextRedaction()
{
  const auto check = [](std::string_view in, std::string_view want, const char* message) {
    const std::string got = RedactPathsInText(in);
    if (got != want) {
      throw std::runtime_error(std::string(message) + ": got \"" + got + "\"");
    }
    Require(RedactPathsInText(got) == got, message);  // masking twice changes nothing
  };
  check("Failed to read C:\\Users\\홍길동\\Documents\\My Games\\Skyrim Special Edition\\SKSE\\x.ini / See D:\\Mods\\a b\\c.dll.",
    "Failed to read <redacted>\\x.ini / See <redacted>\\c.dll.", "drive paths with blanks and Korean folders");
  check("C:/Users/<user>/git/SkyrimSE-SmoothCam/REL/ID.h(223): Unsupported address library format: 2 / This means",
    "<redacted>\\ID.h(223): Unsupported address library format: 2 / This means",
    "a forward-slash path ends at its ':'; the dialog's message stays");
  check("open \\\\?\\C:\\Users\\kim\\AppData\\x.dll and \\\\?\\UNC\\nas\\share\\y.esp",
    "open <redacted>\\x.dll and <redacted>\\y.esp", "\\\\?\\ prefixed paths");
  check("load \\\\nas\\share\\홍길동\\file.esp failed", "load <redacted>\\file.esp failed", "a UNC share");
  check("C:\\Users\\김철수 is not writable", "<redacted>\\<user> is not writable", "a profile folder keeps no name");
  check("C:\\Users\\Kim\\", "<redacted>", "nothing after the last separator");
  check("See /users/other/log.txt and \\Users\\Name\\x", "See /users/<user>/log.txt and \\Users\\<user>\\x",
    "a profile folder without its drive");
  check("[C:\\Program Files (x86)\\Steam\\steamapps\\common\\Skyrim Special Edition\\SkyrimSE.exe] crashed",
    "[<redacted>\\SkyrimSE.exe] crashed", "Program Files (x86)");
  check("srv*C:\\Users\\A\\Symbols*https://msdl.microsoft.com/download/symbols",
    "srv*<redacted>\\Symbols*https://msdl.microsoft.com/download/symbols", "a symbol path chain");
  check("file:///C:/Users/A/report.txt", "file:///<redacted>\\report.txt", "a file URL is a local path");
  check("Loaded C:\\Mods\\a.dll then D:\\x\\y.dll", "Loaded <redacted>\\a.dll then <redacted>\\y.dll",
    "two paths in one sentence");

  // Not paths: module names, FormIDs, URLs, relative data paths, words before ':'.
  for (const std::string_view kept : {
         std::string_view("SkyrimSE.exe+0x1A2B3C"),
         std::string_view("BadPlugin.dll!Foo+0x12"),
         std::string_view("FormID 0xFF000800 [FE:001] Skyrim.esm:0x00012EB7 (Skyrim.esm|0x0001A2B3)"),
         std::string_view("https://www.nexusmods.com/skyrimspecialedition/mods/32444 and https://github.com/users/x/y"),
         std::string_view("Data/SKSE/Plugins/version-1-6-1170-0.bin"),
         std::string_view("meshes\\actors\\character\\a.nif"),
         std::string_view("Exception:\\n code: 0xC0000005"),
         std::string_view("// comment and a://b and \\\\ alone"),
         std::string_view("<redacted>\\x.dll"),
         std::string_view("홍길동 모드 C: 드라이브"),
       }) {
    check(kept, kept, "text without an absolute path stays as it is");
  }
}

// The WCT companion is masked string by string: a dialog text that starts
// with a path keeps its message, and the other fields stay.
void TestWctCompanionDialogText()
{
  auto wct = nlohmann::json::parse(R"({"capture":{"kind":"hang"},"threads":[{"tid":7,"dialogs":[{"title":"SmoothCam.dll",
    "text":"C:/Users/SkydiagProbeUser/git/SmoothCam/REL/ID.h(223): Unsupported address library format: 2\r\nThis means this plugin is incompatible."}]}]})");
  RedactAbsolutePathStrings(&wct);
  const auto& dialog = wct["threads"][0]["dialogs"][0];
  Require(dialog["text"] ==
            "<redacted>\\ID.h(223): Unsupported address library format: 2\r\nThis means this plugin is incompatible.",
    "the dialog text keeps its message");
  Require(dialog["title"] == "SmoothCam.dll" && wct["capture"]["kind"] == "hang" && wct["threads"][0]["tid"] == 7,
    "the other fields stay");
}

void TestUnparsablePluginScanPreview()
{
  AnalysisResult r = MakeMinimalResult();
  r.has_plugin_scan = true;
  // 4095 ASCII bytes, then a Korean syllable (3 UTF-8 bytes) across the
  // 4096-byte preview limit.
  r.plugin_scan_json_utf8 = "{\"mo2\":{\"base_dir\":\"C:\\\\Users\\\\SkydiagProbeUser\\\\";
  r.plugin_scan_json_utf8.append(4095 - r.plugin_scan_json_utf8.size(), 'x');
  r.plugin_scan_json_utf8 += "\xED\x95\x9C";
  r.plugin_scan_json_utf8.append(100, 'y');

  const auto summary = BuildSummaryJson(r, std::filesystem::temp_directory_path(), L"stem", false);
  Require(summary["plugin_scan"].is_null(), "unparsable scan");
  const auto& raw = summary["plugin_scan_raw"].get_ref<const std::string&>();
  Require(raw.size() == 4095u, "preview cut before the split code point");
  // The default (strict) handler throws on invalid UTF-8.
  Require(!summary.dump().empty(), "summary with a preview serializes");

  const auto redacted = BuildSummaryJson(r, std::filesystem::temp_directory_path(), L"stem", true);
  Require(redacted["plugin_scan"].is_null() && !redacted.contains("plugin_scan_raw"),
    "a redacted summary leaves the unredactable preview out");
}

void TestIncidentManifestMatchesHelperTimestamps()
{
  const auto outBase = MakeTempDir(L"manifest");
  // helper Timestamp(): YYYYMMDD_HHMMSS_mmm (HelperCommon.cpp).
  WriteText(outBase / L"SkyrimDiag_Incident_Crash_20261007_123742_440.json", "{}");
  WriteText(outBase / L"SkyrimDiag_Incident_Hang_20261018_190444_914.json", "{}");
  WriteText(outBase / L"SkyrimDiag_Incident_Manual_20261018_190409_880.json", "{}");
  WriteText(outBase / L"SkyrimDiag_Incident_CrashRecapture_20261019_120500_456.json", "{}");
  // Helpers before 2026-02 wrote whole seconds.
  WriteText(outBase / L"SkyrimDiag_Incident_Crash_20260101_101010.json", "{}");
  // Another capture in the same second.
  WriteText(outBase / L"SkyrimDiag_Incident_Crash_20261007_123742_999.json", "{}");

  const auto name = [&](std::wstring_view stem) {
    return FindIncidentManifestForDump(outBase, stem).filename().wstring();
  };
  Require(name(L"SkyrimDiag_Crash_20261007_123742_440") == L"SkyrimDiag_Incident_Crash_20261007_123742_440.json",
    "crash");
  Require(name(L"SkyrimDiag_Hang_20261018_190444_914") == L"SkyrimDiag_Incident_Hang_20261018_190444_914.json",
    "hang");
  Require(name(L"SkyrimDiag_Manual_20261018_190409_880") == L"SkyrimDiag_Incident_Manual_20261018_190409_880.json",
    "manual");
  Require(name(L"SkyrimDiag_Crash_20261019_120500_456_Full") ==
            L"SkyrimDiag_Incident_CrashRecapture_20261019_120500_456.json",
    "recapture dump with a profile suffix");
  Require(name(L"SkyrimDiag_Crash_20260101_101010") == L"SkyrimDiag_Incident_Crash_20260101_101010.json",
    "second-precision names still match");
  Require(name(L"SkyrimDiag_Crash_20261007_123742_441").empty(), "no manifest for another millisecond");
  Require(name(L"SkyrimDiag_Crash").empty(), "no timestamp, no manifest");
  std::filesystem::remove_all(outBase);
}

}  // namespace

int main()
{
  try {
    TestWriterOutputKeepsSchema();
    TestRedactedOutputsHoldNoUserFolder();
    TestSymbolSearchPathRedaction();
    TestProfileFolderPathField();
    TestPathsInTextRedaction();
    TestWctCompanionDialogText();
    TestUnparsablePluginScanPreview();
    TestIncidentManifestMatchesHelperTimestamps();
  } catch (const std::exception& ex) {
    std::cerr << "summary_writer_runtime_tests failed: " << ex.what() << "\n";
    return 1;
  }
  std::cout << "summary_writer_runtime_tests passed\n";
  return 0;
}
