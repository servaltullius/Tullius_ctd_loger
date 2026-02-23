#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

static std::string ReadAllText(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::in | std::ios::binary);
  assert(in && "Failed to open file");
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static void AssertContains(const std::string& haystack, const char* needle, const char* message)
{
  assert(haystack.find(needle) != std::string::npos && message);
}

int main()
{
  const std::filesystem::path repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  const auto preflightCpp = repoRoot / "helper" / "src" / "CompatibilityPreflight.cpp";
  const auto preflightHeader = repoRoot / "helper" / "src" / "CompatibilityPreflight.h";
  const auto helperMain = repoRoot / "helper" / "src" / "main.cpp";
  const auto helperIni = repoRoot / "dist" / "SkyrimDiagHelper.ini";
  const auto wctCapture = repoRoot / "helper" / "src" / "WctCapture.cpp";

  assert(std::filesystem::exists(preflightCpp) && "helper/src/CompatibilityPreflight.cpp not found");
  assert(std::filesystem::exists(preflightHeader) && "helper/src/CompatibilityPreflight.h not found");
  assert(std::filesystem::exists(helperMain) && "helper/src/main.cpp not found");
  assert(std::filesystem::exists(helperIni) && "dist/SkyrimDiagHelper.ini not found");
  assert(std::filesystem::exists(wctCapture) && "helper/src/WctCapture.cpp not found");

  const auto preflightCppText = ReadAllText(preflightCpp);
  const auto helperMainText = ReadAllText(helperMain);
  const auto helperIniText = ReadAllText(helperIni);
  const auto wctCaptureText = ReadAllText(wctCapture);

  AssertContains(
    preflightCppText,
    "SkyrimDiagPreflight",
    "Compatibility preflight must write schema-tagged output.");
  AssertContains(
    preflightCppText,
    "SkyrimDiag_Preflight.json",
    "Compatibility preflight must emit SkyrimDiag_Preflight.json.");
  AssertContains(
    preflightCppText,
    "CRASH_LOGGER_CONFLICT",
    "Preflight must include crash-logger conflict checks.");
  AssertContains(
    helperMainText,
    "RunCompatibilityPreflight(",
    "Helper main loop must run compatibility preflight at startup.");
  AssertContains(
    helperIniText,
    "EnableCompatibilityPreflight",
    "Helper ini must include compatibility preflight toggle.");
  AssertContains(
    wctCaptureText,
    "RegisterWaitChainCOMCallback",
    "WCT capture must register COM callback when available.");
  AssertContains(
    wctCaptureText,
    "comCallbackRegistered",
    "WCT output must include COM callback registration status.");

  AssertContains(
    preflightCppText,
    "FULL_PLUGIN_SLOT_LIMIT",
    "Preflight must warn when non-ESL plugin count approaches 254 limit.");
  AssertContains(
    preflightCppText,
    "KNOWN_INCOMPATIBLE_COMBO",
    "Preflight must check known incompatible mod combinations.");

  return 0;
}

