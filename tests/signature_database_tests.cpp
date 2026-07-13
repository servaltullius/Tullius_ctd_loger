#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

static std::string ReadFile(const char* relPath)
{
  const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
  assert(root && "SKYDIAG_PROJECT_ROOT must be set");
  const std::filesystem::path p = std::filesystem::path(root) / relPath;
  std::ifstream in(p, std::ios::in | std::ios::binary);
  assert(in && "Failed to open file");
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static void TestSignatureJsonExists()
{
  const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
  assert(root);
  const std::filesystem::path p = std::filesystem::path(root) / "dump_tool" / "data" / "crash_signatures.json";
  assert(std::filesystem::exists(p));

  const auto content = ReadFile("dump_tool/data/crash_signatures.json");
  assert(content.find("\"version\"") != std::string::npos);
  assert(content.find("\"signatures\"") != std::string::npos);
}

static void TestSignatureDatabaseApiExists()
{
  const auto header = ReadFile("dump_tool/src/SignatureDatabase.h");
  assert(header.find("SignatureDatabase") != std::string::npos);
  assert(header.find("LoadFromJson") != std::string::npos);
  assert(header.find("Match") != std::string::npos);
  assert(header.find("SignatureMatch") != std::string::npos);
  assert(header.find("std::string game_version") != std::string::npos);
  assert(header.find("std::optional<std::uint64_t> access_violation_address") != std::string::npos);
}

static void TestKnownSignaturePresent()
{
  const auto content = ReadFile("dump_tool/data/crash_signatures.json");
  assert(content.find("D6DDDA_1597_AV") != std::string::npos);
  assert(content.find("D6DDDA_VRAM") == std::string::npos);
  assert(content.find("\"game_version\": \"1.5.97.0\"") != std::string::npos);
  assert(content.find("\"fault_offset\": \"0xD6DDDA\"") != std::string::npos);
  assert(content.find("^[Dd]6[Dd][Dd][Dd][Aa]$") != std::string::npos);
  assert(content.find("^[Dd]6[Dd][Dd]..$") == std::string::npos);
  assert(content.find("\"scope\": \"mechanism\"") != std::string::npos);
}

static void TestBilingualFieldsPresent()
{
  const auto content = ReadFile("dump_tool/data/crash_signatures.json");
  assert(content.find("cause_ko") != std::string::npos);
  assert(content.find("cause_en") != std::string::npos);
  assert(content.find("recommendations_ko") != std::string::npos);
  assert(content.find("recommendations_en") != std::string::npos);
}

static void TestAnalyzerUsesRealCallstackForSignatureInput()
{
  const auto analyzer = ReadFile("dump_tool/src/Analyzer.cpp");
  assert(analyzer.find("out.stackwalk_primary_frames") != std::string::npos);
  assert(analyzer.find("input.callstack_modules.push_back(frame)") != std::string::npos);
}

static void TestAnalyzerUsesVersionAndAccessedAddressForSignatureInput()
{
  const auto analyzer = ReadFile("dump_tool/src/Analyzer.cpp");
  const auto signatures = ReadFile("dump_tool/data/crash_signatures.json");

  assert(analyzer.find("input.game_version = out.game_version") != std::string::npos);
  assert(analyzer.find("out.exc_info.size() >= 2u") != std::string::npos);
  assert(analyzer.find("input.access_violation_address = out.exc_info[1]") != std::string::npos);
  assert(analyzer.find("input.exc_address = out.exc_addr") == std::string::npos);
  assert(signatures.find("access_violation_address_near_zero") != std::string::npos);
  assert(signatures.find("\"exc_address_near_zero\"") != std::string::npos);
}

static void TestUnmatchedCrashLoggerCandidateIsNotConfidencePromoted()
{
  const auto analyzer = ReadFile("dump_tool/src/Analyzer.cpp");
  assert(analyzer.find("topRow.matchedRank && *topRow.matchedRank <= 1u") != std::string::npos);
  assert(analyzer.find("!topRow.matchedRank || *topRow.matchedRank <= 1u") == std::string::npos);
}

static void TestCrashLoggerPromotionRejectsWindowsSystemPaths()
{
  const auto analyzer = ReadFile("dump_tool/src/Analyzer.cpp");
  assert(analyzer.find("IsCrashLoggerFrameModuleLoadedFromSystemPath") != std::string::npos);
  assert(analyzer.find("IsLikelyWindowsSystemModulePath(loaded.path)") != std::string::npos);
  assert(analyzer.find("ApplyCrashLoggerCorroborationToSuspects(&out, allModules)") != std::string::npos);
}

static void TestMechanismMatchDoesNotHideRootCauseCandidateSummary()
{
  const auto summary = ReadFile("dump_tool/src/EvidenceBuilderSummary.cpp");
  assert(summary.find("Known crash mechanism") != std::string::npos);
  assert(summary.find("Root-cause assessment") != std::string::npos);
  assert(summary.find("if (r.signature_match.has_value())") > summary.find("std::wstring summary;"));
}

int main()
{
  TestSignatureJsonExists();
  TestSignatureDatabaseApiExists();
  TestKnownSignaturePresent();
  TestBilingualFieldsPresent();
  TestAnalyzerUsesRealCallstackForSignatureInput();
  TestAnalyzerUsesVersionAndAccessedAddressForSignatureInput();
  TestUnmatchedCrashLoggerCandidateIsNotConfidencePromoted();
  TestCrashLoggerPromotionRejectsWindowsSystemPaths();
  TestMechanismMatchDoesNotHideRootCauseCandidateSummary();
  return 0;
}
