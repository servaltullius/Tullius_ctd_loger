// Memory at capture time: RAM and commit from the dump's own
// SystemMemoryInfo/ProcessVmCounters streams, VRAM from the helper's GPU
// memory stream. Numbers are the user's 2026-10-07 crash (64 GB RAM,
// RTX 4090) unless a test pushes them to a limit.

#include <Windows.h>

#include <DbgHelp.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "Analyzer.h"
#include "EvidenceBuilder.h"
#include "MemorySnapshot.h"
#include "NextAction.h"
#include "OutputWriterPipeline.h"
#include "SkyrimDiagProtocol.h"

namespace {

using skydiag::dump_tool::AnalysisResult;
using skydiag::dump_tool::BuildEvidenceAndSummary;
using skydiag::dump_tool::MemorySnapshot;
namespace i18n = skydiag::dump_tool::i18n;

constexpr std::uint64_t kGiB = 1024ull * 1024ull * 1024ull;
constexpr std::uint64_t kMiB = 1024ull * 1024ull;

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

bool AnyStartsWith(const std::vector<std::wstring>& lines, std::wstring_view prefix, std::wstring_view part = L"")
{
  for (const auto& line : lines) {
    if (line.rfind(prefix, 0) == 0 && Contains(line, part)) {
      return true;
    }
  }
  return false;
}

const skydiag::dump_tool::EvidenceItem* FindEvidence(const AnalysisResult& r, std::wstring_view title)
{
  for (const auto& e : r.evidence) {
    if (Contains(e.title, title)) {
      return &e;
    }
  }
  return nullptr;
}

struct Stream
{
  std::uint32_t type = 0;
  std::vector<std::uint8_t> bytes;
};

template <typename T>
Stream MakeStream(std::uint32_t type, const T& value)
{
  Stream s{ type, std::vector<std::uint8_t>(sizeof(T)) };
  std::memcpy(s.bytes.data(), &value, sizeof(T));
  return s;
}

std::vector<std::uint8_t> BuildDump(const std::vector<Stream>& streams)
{
  std::vector<std::uint8_t> dump(sizeof(MINIDUMP_HEADER) + streams.size() * sizeof(MINIDUMP_DIRECTORY));
  MINIDUMP_HEADER header{};
  header.Signature = MINIDUMP_SIGNATURE;
  header.Version = MINIDUMP_VERSION;
  header.NumberOfStreams = static_cast<ULONG32>(streams.size());
  header.StreamDirectoryRva = sizeof(MINIDUMP_HEADER);
  std::memcpy(dump.data(), &header, sizeof(header));
  for (std::size_t i = 0; i < streams.size(); ++i) {
    MINIDUMP_DIRECTORY dir{};
    dir.StreamType = streams[i].type;
    dir.Location.DataSize = static_cast<ULONG32>(streams[i].bytes.size());
    dir.Location.Rva = static_cast<RVA>(dump.size());
    dump.insert(dump.end(), streams[i].bytes.begin(), streams[i].bytes.end());
    std::memcpy(dump.data() + sizeof(MINIDUMP_HEADER) + i * sizeof(MINIDUMP_DIRECTORY), &dir, sizeof(dir));
  }
  return dump;
}

Stream SystemMemoryStream(std::uint64_t availablePages, std::uint64_t committedPages)
{
  MINIDUMP_SYSTEM_MEMORY_INFO_1 info{};
  info.Revision = 2;
  info.Flags = MINIDUMP_SYSMEMINFO1_BASICPERF;
  info.BasicInfo.PageSize = 4096;
  info.BasicInfo.NumberOfPhysicalPages = 16136541;  // 61.56 GB
  info.BasicPerfInfo.AvailablePages = availablePages;
  info.BasicPerfInfo.CommittedPages = committedPages;
  info.BasicPerfInfo.CommitLimit = 26622301;  // 101.56 GB
  return MakeStream(SystemMemoryInfoStream, info);
}

Stream ProcessCountersStream()
{
  MINIDUMP_PROCESS_VM_COUNTERS_2 counters{};
  counters.Revision = 2;
  counters.Flags = MINIDUMP_PROCESS_VM_COUNTERS | MINIDUMP_PROCESS_VM_COUNTERS_EX;
  counters.PrivateUsage = 25098ull * kMiB;
  counters.PagefileUsage = counters.PrivateUsage;
  counters.WorkingSetSize = 14108ull * kMiB;
  counters.PeakWorkingSetSize = 14108ull * kMiB;
  return MakeStream(ProcessVmCountersStream, counters);
}

// The shape the helper writes (helper/src/GpuMemory.cpp).
Stream GpuStream(std::uint64_t usage, std::uint64_t budget)
{
  const nlohmann::json j = {
    { "schema_version", 1 },
    { "adapters", nlohmann::json::array({
      { { "name", "AMD Radeon(TM) Graphics" }, { "local_usage", 12 * kMiB }, { "local_budget", 46 * kGiB },
        { "nonlocal_usage", 0 }, { "nonlocal_budget", 0 }, { "dedicated_bytes", 2 * kGiB } },
      { { "name", "NVIDIA GeForce RTX 4090" }, { "local_usage", usage }, { "local_budget", budget },
        { "nonlocal_usage", 100 * kMiB }, { "nonlocal_budget", 44 * kGiB }, { "dedicated_bytes", 23 * kGiB } },
    }) },
  };
  const std::string text = j.dump();
  return Stream{ skydiag::protocol::kMinidumpUserStream_GpuMemory, std::vector<std::uint8_t>(text.begin(), text.end()) };
}

MemorySnapshot ReadSnapshot(const std::vector<Stream>& streams)
{
  auto dump = BuildDump(streams);
  MemorySnapshot snapshot{};
  skydiag::dump_tool::ReadMemorySnapshot(dump.data(), dump.size(), snapshot);
  return snapshot;
}

void TestDumpStreamsAreRead()
{
  const auto m = ReadSnapshot({
    SystemMemoryStream(6378496, 14366720),
    ProcessCountersStream(),
    GpuStream(4946ull * kMiB, 23368ull * kMiB),
  });
  Require(m.has_system && m.has_process && m.has_gpu, "all three parts are read");
  Require(m.ram_total_bytes == 16136541ull * 4096ull, "RAM total comes from the physical page count");
  Require(m.ram_available_bytes == 6378496ull * 4096ull, "available RAM");
  Require(m.commit_used_bytes == 14366720ull * 4096ull && m.commit_limit_bytes == 26622301ull * 4096ull, "commit");
  Require(m.process_private_bytes == 25098ull * kMiB && m.process_working_set_bytes == 14108ull * kMiB, "process");
  Require(m.gpu_name == L"NVIDIA GeForce RTX 4090", "the adapter the game used most");
  Require(m.vram_usage_bytes == 4946ull * kMiB && m.vram_budget_bytes == 23368ull * kMiB, "VRAM usage and budget");
  Require(m.gpu_shared_usage_bytes == 100ull * kMiB, "shared GPU memory");

  const auto none = ReadSnapshot({ ProcessCountersStream() });
  Require(!none.has_system && none.has_process && !none.has_gpu, "missing streams leave their part unset");

  MemorySnapshot broken{};
  Require(!skydiag::dump_tool::ApplyGpuMemoryJson("{\"adapters\":", broken) && !broken.has_gpu, "broken JSON is ignored");
}

void TestPressureThresholds()
{
  MemorySnapshot m{};
  m.has_system = true;
  m.commit_limit_bytes = 100 * kGiB;
  m.commit_used_bytes = 89 * kGiB;
  m.has_gpu = true;
  m.vram_budget_bytes = 20 * kGiB;
  m.vram_usage_bytes = 17 * kGiB;
  auto p = skydiag::dump_tool::AssessMemoryPressure(m);
  Require(!p.Any() && p.commit_percent == 89u && p.vram_percent == 85u, "below the limits");

  m.commit_used_bytes = 90 * kGiB;
  m.vram_usage_bytes = 18 * kGiB;
  p = skydiag::dump_tool::AssessMemoryPressure(m);
  Require(p.commit_high && p.vram_high, "90% of either limit is pressure");

  m.vram_budget_bytes = 0;
  Require(!skydiag::dump_tool::AssessMemoryPressure(m).vram_high, "no budget, no VRAM verdict");

  // The raw ratio decides: 89.6% shows as 90% but is not pressure.
  m.commit_limit_bytes = 1000 * kGiB;
  m.commit_used_bytes = 896 * kGiB;
  p = skydiag::dump_tool::AssessMemoryPressure(m);
  Require(p.commit_percent == 90u && !p.commit_high, "89.6% is not rounded up into pressure");

  // A PSS snapshot's clone counts toward commit while the dump is written.
  m.commit_used_bytes = 990 * kGiB;
  m.commit_includes_snapshot_clone = true;
  Require(!skydiag::dump_tool::AssessMemoryPressure(m).commit_high, "no commit verdict for a PSS dump");
  Require(Contains(skydiag::dump_tool::DescribeSystemMemory(m, false), L"PSS 스냅샷이 복제한 게임 메모리 포함"),
    "the PSS commit is labelled");
}

void TestGpuDescription()
{
  MemorySnapshot m{};
  m.has_gpu = true;
  m.gpu_name = L"NVIDIA GeForce RTX 4090";
  m.vram_usage_bytes = 4946ull * kMiB;
  m.vram_budget_bytes = 23368ull * kMiB;
  Require(skydiag::dump_tool::DescribeGpuMemory(m, false) == L"NVIDIA GeForce RTX 4090, VRAM 4.83/22.82 GB(예산 대비 21%)",
    "Korean VRAM wording");
  m.vram_budget_bytes = 0;
  m.gpu_shared_usage_bytes = 100ull * kMiB;
  Require(skydiag::dump_tool::DescribeGpuMemory(m, true) == L"NVIDIA GeForce RTX 4090, shared 0.10 GB",
    "no VRAM figure without a budget");
  m.gpu_shared_usage_bytes = 0;
  Require(skydiag::dump_tool::DescribeGpuMemory(m, true).empty(), "nothing to show");

  // Fields of the wrong type read as absent; the analysis goes on.
  MemorySnapshot odd{};
  Require(skydiag::dump_tool::ApplyGpuMemoryJson(
            R"({"adapters":[{"name":7,"local_usage":"12","local_budget":null,"nonlocal_usage":[1]}]})", odd),
    "the adapter is still taken");
  Require(odd.has_gpu && odd.gpu_name.empty() && odd.vram_usage_bytes == 0 && odd.vram_budget_bytes == 0,
    "wrong types read as absent");
}

AnalysisResult MakeCrash(const MemorySnapshot& memory)
{
  AnalysisResult r{};
  r.dump_path = L"C:\\Logs\\SkyrimDiag_Crash_20261007_123742_440.dmp";
  r.exc_code = 0xC0000005u;
  r.fault_module_filename = L"SkyrimSE.exe";
  r.memory = memory;
  return r;
}

MemorySnapshot RealCrashMemory()
{
  return ReadSnapshot({
    SystemMemoryStream(6378496, 14366720),
    ProcessCountersStream(),
    GpuStream(4946ull * kMiB, 23368ull * kMiB),
  });
}

void TestNormalMemoryIsContextOnly()
{
  auto r = MakeCrash(RealCrashMemory());
  BuildEvidenceAndSummary(r, i18n::Language::kEnglish);
  const auto* e = FindEvidence(r, L"Memory at capture time");
  Require(e != nullptr && e->confidence_level == i18n::ConfidenceLevel::kLow, "low-confidence context item");
  Require(Contains(e->details, L"RAM 24.3/61.6 GB available, commit 54.8/101.6 GB (54%)"), "system numbers");
  Require(Contains(e->details, L"game commit 24.51 GB, working set 13.78 GB"), "game numbers");
  Require(Contains(e->details, L"NVIDIA GeForce RTX 4090, VRAM 4.83/22.82 GB of budget (21%)"),
    "the same VRAM numbers Crash Logger printed for this crash");
  Require(!AnyStartsWith(r.recommendations, L"[Memory]"), "no memory advice without pressure");

  const auto summary = skydiag::dump_tool::BuildSummaryJson(r, std::filesystem::temp_directory_path(), L"stem", false);
  Require(summary["memory"]["system"]["commit_percent"] == 54 && summary["memory"]["gpu"]["vram_percent"] == 21 &&
            summary["memory"]["commit_high"] == false && summary["memory"]["vram_high"] == false,
    "summary JSON memory block");
  const auto report = skydiag::dump_tool::BuildReportText(r, summary, false);
  Require(report.find("Memory: RAM 24.3/61.6 GB available") != std::string::npos, "report memory line");
  Require(report.find("GPU: NVIDIA GeForce RTX 4090, VRAM 4.83/22.82 GB of budget (21%)") != std::string::npos,
    "report GPU line");

  AnalysisResult old = MakeCrash(MemorySnapshot{});
  BuildEvidenceAndSummary(old, i18n::Language::kEnglish);
  Require(FindEvidence(old, L"Memory") == nullptr, "a dump without the streams has no memory item");
  const auto oldSummary = skydiag::dump_tool::BuildSummaryJson(old, std::filesystem::temp_directory_path(), L"stem", false);
  Require(!oldSummary.contains("memory"), "nor a memory block");
}

void TestPressureIsReported()
{
  auto memory = RealCrashMemory();
  memory.vram_usage_bytes = 22000ull * kMiB;           // 94% of 23368 MiB
  memory.commit_used_bytes = 26000000ull * 4096ull;    // 98% of the limit
  for (const auto lang : { i18n::Language::kEnglish, i18n::Language::kKorean }) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = MakeCrash(memory);
    BuildEvidenceAndSummary(r, lang);
    const auto* e = FindEvidence(r, en ? L"close to its limit" : L"한도에 가까웠음");
    Require(e != nullptr && e->confidence_level == i18n::ConfidenceLevel::kMedium, "pressure raises the item");
    Require(Contains(e->details, en ? L"VRAM at 94% of the game's budget" : L"VRAM이 게임 예산의 94%"), "VRAM share");
    Require(Contains(e->details, en ? L"system commit at 98% of the commit limit" : L"시스템 커밋이 한도의 98%"),
      "commit share");
    const std::wstring_view tag = en ? L"[Memory]" : L"[메모리]";
    Require(AnyStartsWith(r.recommendations, tag, en ? L"94% of its VRAM budget (21.48/22.82 GB)" : L"VRAM 예산의 94%(21.48/22.82 GB)"),
      "VRAM advice with the numbers");
    Require(AnyStartsWith(r.recommendations, tag, en ? L"98% of its limit" : L"한도의 98%"), "commit advice");
  }

  // A named candidate still comes first.
  auto r = MakeCrash(memory);
  r.fault_module_filename = L"SmoothCam.dll";
  r.fault_module_path = L"G:\\TAKEALOOK\\mods\\SmoothCam\\SKSE\\Plugins\\SmoothCam.dll";
  BuildEvidenceAndSummary(r, i18n::Language::kEnglish);
  const auto next = skydiag::dump_tool::SelectNextActionIndex(r.recommendations);
  std::size_t firstMemory = r.recommendations.size();
  for (std::size_t i = 0; i < r.recommendations.size(); ++i) {
    if (r.recommendations[i].rfind(L"[Memory]", 0) == 0) {
      firstMemory = std::min(firstMemory, i);
    }
  }
  Require(firstMemory < r.recommendations.size(), "the memory advice is there");
  Require(next != std::wstring::npos && next < firstMemory && r.recommendations[next].rfind(L"[Memory]", 0) != 0,
    "memory pressure does not outrank the DLL line");
}

}  // namespace

int main()
{
  try {
    TestDumpStreamsAreRead();
    TestPressureThresholds();
    TestGpuDescription();
    TestNormalMemoryIsContextOnly();
    TestPressureIsReported();
    std::puts("memory snapshot tests passed");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
