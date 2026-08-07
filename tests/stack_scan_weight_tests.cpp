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
  assert(in && "Failed to open source file");
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static void TestStackScanUsesProximityWeight()
{
  const auto src = ReadFile("dump_tool/src/AnalyzerInternalsStackScan.cpp");
  assert(src.find("StackScanSlotWeight") != std::string::npos);
  assert(src.find("exceptionScoreByModule") != std::string::npos);
  assert(src.find("usedExceptionThreadScores = !rows.empty()") != std::string::npos);
  assert(src.find("rows = buildActionableRows(scoreByModule)") != std::string::npos);

  const auto capture = ReadFile("dump_tool/src/Analyzer.CaptureInputs.cpp");
  assert(capture.find("std::vector<std::uint32_t>{ *mainTid }") != std::string::npos);
  assert(capture.find("main-thread pointer scan only; weak freeze clue") != std::string::npos);

  const auto candidates = ReadFile("dump_tool/src/EvidenceBuilderCandidates.cpp");
  assert(candidates.find("weakHangPointerScan ? 1u : kMaxActionableStackSignals") != std::string::npos);
  assert(candidates.find("topHangMainThreadOwner") != std::string::npos);
}

int main()
{
  TestStackScanUsesProximityWeight();
  return 0;
}
