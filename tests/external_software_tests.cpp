// GPU vendor tools and overlays (NVIDIA's NvTelemetryBridge64.dll, the Steam
// overlay, ...) load into the game but are not mods: a field report had one
// named as a crash suspect like a mod DLL (Nexus, 2026-10-17).

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Analyzer.h"
#include "CrashLogger.h"
#include "EvidenceBuilder.h"
#include "MinidumpUtil.h"

namespace {

using skydiag::dump_tool::AnalysisResult;
using skydiag::dump_tool::BuildEvidenceAndSummary;
using skydiag::dump_tool::ParseCrashLoggerTopModules;
using skydiag::dump_tool::SuspectItem;
using skydiag::dump_tool::minidump::ExternalSoftwareName;
namespace i18n = skydiag::dump_tool::i18n;

void Require(bool condition, const char* message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

bool Contains(const std::wstring& text, std::wstring_view part)
{
  return text.find(part) != std::wstring::npos;
}

bool AnyStartsWith(const std::vector<std::wstring>& lines, std::wstring_view prefix)
{
  for (const auto& line : lines) {
    if (line.rfind(prefix, 0) == 0) {
      return true;
    }
  }
  return false;
}

void TestNames()
{
  Require(ExternalSoftwareName(L"NvTelemetryBridge64.dll") == L"NVIDIA", "by name, as Crash Logger lists it");
  Require(ExternalSoftwareName(L"NVTELEMETRYBRIDGE64.DLL") == L"NVIDIA", "any case");
  Require(
    ExternalSoftwareName(
      L"NvTelemetryBridge64.dll", L"C:\\Program Files\\NVIDIA Corporation\\NvTelemetry\\NvTelemetryBridge64.dll") ==
      L"NVIDIA",
    "by name with its path");
  Require(
    ExternalSoftwareName(L"NvCamera64.dll", L"C:\\Program Files\\NVIDIA Corporation\\Ansel\\NvCamera64.dll") == L"NVIDIA",
    "by the vendor's install folder");
  Require(
    ExternalSoftwareName(L"GameOverlayRenderer64.dll", L"C:\\Program Files (x86)\\Steam\\GameOverlayRenderer64.dll") ==
      L"Steam overlay",
    "Steam overlay");
  Require(ExternalSoftwareName(L"RTSSHooks64.dll") == L"RivaTuner Statistics Server", "RTSS");

  Require(ExternalSoftwareName(L"nvspcap64.dll", L"C:\\Windows\\System32\\nvspcap64.dll").empty(),
    "a copy under Windows stays a system module");
  Require(ExternalSoftwareName(L"po3_Tweaks.dll", L"G:\\MO2\\mods\\po3 Tweaks\\SKSE\\Plugins\\po3_Tweaks.dll").empty(),
    "a mod DLL");
  Require(
    ExternalSoftwareName(
      L"ExampleMod.dll",
      L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Skyrim Special Edition\\Data\\SKSE\\Plugins\\ExampleMod.dll")
      .empty(),
    "a mod in a game installed under Program Files");
}

void TestCrashLoggerStackSkipsIt()
{
  const std::string log =
    "CrashLoggerSSE v1.20.0 Feb 10 2026 04:25:35\n"
    "Unhandled exception \"EXCEPTION_ACCESS_VIOLATION\" at 0x7FF612345678 NvTelemetryBridge64.dll+00001234\n"
    "PROBABLE CALL STACK:\n"
    "\t[ 0] 0x00007FF612340001 NvTelemetryBridge64.dll+00000001\n"
    "\t[ 1] 0x00007FF612340002 NvTelemetryBridge64.dll+00000002\n"
    "\t[ 2] 0x00007FF612340003 ExampleMod.dll+00000003\n"
    "\t[ 3] 0x00007FF612340004 SkyrimSE.exe+00000004\n"
    "\n"
    "REGISTERS:\n";
  const auto modules = ParseCrashLoggerTopModules(log, std::unordered_map<std::wstring, std::wstring>{});
  Require(!modules.empty() && modules.front() == L"ExampleMod.dll", "the mod DLL leads");
  for (const auto& module : modules) {
    Require(module != L"NvTelemetryBridge64.dll", "the NVIDIA DLL is no suspect");
  }
}

AnalysisResult MakeCrashInNvidiaDll()
{
  AnalysisResult r{};
  r.dump_path = L"C:\\Logs\\SkyrimDiag_Crash_20261017_212700_000.dmp";
  r.exc_code = 0xC0000005u;
  r.exc_addr = 0x7FF612345678ull;
  r.fault_module_filename = L"NvTelemetryBridge64.dll";
  r.fault_module_path = L"C:\\Program Files\\NVIDIA Corporation\\NvTelemetry\\NvTelemetryBridge64.dll";
  r.fault_module_plus_offset = L"NvTelemetryBridge64.dll+0x1234";
  r.fault_module_external_software = ExternalSoftwareName(r.fault_module_filename, r.fault_module_path);
  return r;
}

void TestFaultInsideIt()
{
  for (const auto lang : { i18n::Language::kEnglish, i18n::Language::kKorean }) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = MakeCrashInNvidiaDll();
    BuildEvidenceAndSummary(r, lang);

    Require(Contains(r.summary_sentence,
              en ? L"Crash is reported in external software (NVIDIA, NvTelemetryBridge64.dll), a program outside the game"
                 : L"크래시가 외부 프로그램(NVIDIA, NvTelemetryBridge64.dll)에서 보고되었습니다."),
      "the summary names the program, not a mod");
    Require(!Contains(r.summary_sentence, en ? L"Windows system DLL" : L"Windows 시스템 DLL"), "not a Windows DLL");

    bool hasEvidence = false;
    for (const auto& e : r.evidence) {
      hasEvidence = hasEvidence ||
        Contains(e.title, en ? L"Exception reported in external software (NVIDIA)" : L"크래시가 외부 프로그램(NVIDIA)에서 보고됨");
      Require(!Contains(e.title, en ? L"inside a specific DLL" : L"특정 DLL 내부"), "not classed as a mod DLL");
    }
    Require(hasEvidence, "evidence item for the external program");

    Require(AnyStartsWith(r.recommendations, en ? L"[External software]" : L"[외부 프로그램]"), "update or turn it off");
    Require(!AnyStartsWith(r.recommendations, en ? L"[DLL guidance]" : L"[DLL 점검]"), "no mod prerequisites advice");
    Require(!AnyStartsWith(r.recommendations, en ? L"[Top suspect]" : L"[유력 후보]"), "not a top suspect");
    for (const auto& c : r.actionable_candidates) {
      Require(c.module_filename != L"NvTelemetryBridge64.dll", "not an actionable candidate");
    }
  }

  // A mod on the stack still leads; the fault place is named as the program.
  auto r = MakeCrashInNvidiaDll();
  SuspectItem mod{};
  mod.module_filename = L"ExampleMod.dll";
  mod.module_path = L"G:\\MO2\\mods\\Example\\SKSE\\Plugins\\ExampleMod.dll";
  mod.inferred_mod_name = L"Example";
  mod.confidence_level = i18n::ConfidenceLevel::kMedium;
  mod.confidence = i18n::ConfidenceText(i18n::Language::kEnglish, mod.confidence_level);
  mod.score = 12;
  r.suspects.push_back(mod);
  r.suspects_from_stackwalk = true;
  BuildEvidenceAndSummary(r, i18n::Language::kEnglish);
  Require(Contains(r.summary_sentence, L"Crash is reported in external software (NVIDIA, NvTelemetryBridge64.dll), but"),
    "the fault place");
  Require(Contains(r.summary_sentence, L"ExampleMod.dll"), "the mod on the stack");
}

}  // namespace

int main()
{
  try {
    TestNames();
    TestCrashLoggerStackSkipsIt();
    TestFaultInsideIt();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "external_software_tests failed: %s\n", e.what());
    return 1;
  }
  return 0;
}
