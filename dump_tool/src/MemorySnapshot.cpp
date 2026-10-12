#include "MemorySnapshot.h"

#include <Windows.h>

#include <DbgHelp.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include <nlohmann/json.hpp>

#include "MinidumpUtil.h"
#include "SkyrimDiagProtocol.h"
#include "Utf.h"

namespace skydiag::dump_tool {
namespace {

void ReadSystemMemory(void* dumpBase, std::uint64_t dumpSize, MemorySnapshot& out)
{
  void* ptr = nullptr;
  ULONG size = 0;
  if (!minidump::ReadStreamSized(dumpBase, dumpSize, SystemMemoryInfoStream, &ptr, &size) || !ptr ||
      size < sizeof(MINIDUMP_SYSTEM_MEMORY_INFO_1)) {
    return;
  }
  MINIDUMP_SYSTEM_MEMORY_INFO_1 info{};
  std::memcpy(&info, ptr, sizeof(info));
  const std::uint64_t pageSize = info.BasicInfo.PageSize;
  if (pageSize == 0 || (info.Flags & MINIDUMP_SYSMEMINFO1_BASICPERF) == 0) {
    return;
  }
  out.has_system = true;
  out.ram_total_bytes = pageSize * info.BasicInfo.NumberOfPhysicalPages;
  out.ram_available_bytes = pageSize * info.BasicPerfInfo.AvailablePages;
  out.commit_used_bytes = pageSize * info.BasicPerfInfo.CommittedPages;
  out.commit_limit_bytes = pageSize * info.BasicPerfInfo.CommitLimit;
}

void ReadProcessCounters(void* dumpBase, std::uint64_t dumpSize, MemorySnapshot& out)
{
  void* ptr = nullptr;
  ULONG size = 0;
  if (!minidump::ReadStreamSized(dumpBase, dumpSize, ProcessVmCountersStream, &ptr, &size) || !ptr ||
      size < sizeof(USHORT)) {
    return;
  }
  USHORT revision = 0;
  std::memcpy(&revision, ptr, sizeof(revision));
  if (revision == 1 && size >= sizeof(MINIDUMP_PROCESS_VM_COUNTERS_1)) {
    MINIDUMP_PROCESS_VM_COUNTERS_1 counters{};
    std::memcpy(&counters, ptr, sizeof(counters));
    out.has_process = true;
    out.process_private_bytes = counters.PrivateUsage;
    out.process_working_set_bytes = counters.WorkingSetSize;
    out.process_peak_working_set_bytes = counters.PeakWorkingSetSize;
    return;
  }
  if (revision >= 2 && size >= sizeof(MINIDUMP_PROCESS_VM_COUNTERS_2)) {
    MINIDUMP_PROCESS_VM_COUNTERS_2 counters{};
    std::memcpy(&counters, ptr, sizeof(counters));
    if ((counters.Flags & MINIDUMP_PROCESS_VM_COUNTERS) == 0) {
      return;
    }
    out.has_process = true;
    // PagefileUsage is the same commit charge when the EX field is absent.
    out.process_private_bytes =
      (counters.Flags & MINIDUMP_PROCESS_VM_COUNTERS_EX) != 0 ? counters.PrivateUsage : counters.PagefileUsage;
    out.process_working_set_bytes = counters.WorkingSetSize;
    out.process_peak_working_set_bytes = counters.PeakWorkingSetSize;
  }
}

unsigned Percent(std::uint64_t part, std::uint64_t whole)
{
  if (whole == 0) {
    return 0;
  }
  return static_cast<unsigned>(std::llround(static_cast<double>(part) * 100.0 / static_cast<double>(whole)));
}

}  // namespace

std::wstring FormatGb(std::uint64_t bytes, int decimals)
{
  wchar_t buf[64]{};
  swprintf_s(buf, L"%.*f", decimals, static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
  return buf;
}

bool ApplyGpuMemoryJson(std::string_view json, MemorySnapshot& out)
{
  const auto j = nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
  if (j.is_discarded() || !j.is_object() || !j.contains("adapters") || !j["adapters"].is_array()) {
    return false;
  }
  const nlohmann::json* best = nullptr;
  std::uint64_t bestUsage = 0;
  for (const auto& adapter : j["adapters"]) {
    if (!adapter.is_object()) {
      continue;
    }
    const std::uint64_t usage = adapter.value("local_usage", std::uint64_t{ 0 });
    if (!best || usage > bestUsage) {
      best = &adapter;
      bestUsage = usage;
    }
  }
  if (!best) {
    return false;
  }
  out.has_gpu = true;
  out.gpu_name = Utf8ToWide(best->value("name", std::string{}));
  out.vram_usage_bytes = bestUsage;
  out.vram_budget_bytes = best->value("local_budget", std::uint64_t{ 0 });
  out.vram_dedicated_bytes = best->value("dedicated_bytes", std::uint64_t{ 0 });
  out.gpu_shared_usage_bytes = best->value("nonlocal_usage", std::uint64_t{ 0 });
  return true;
}

void ReadMemorySnapshot(void* dumpBase, std::uint64_t dumpSize, MemorySnapshot& out)
{
  ReadSystemMemory(dumpBase, dumpSize, out);
  ReadProcessCounters(dumpBase, dumpSize, out);
  void* ptr = nullptr;
  ULONG size = 0;
  if (minidump::ReadStreamSized(dumpBase, dumpSize, skydiag::protocol::kMinidumpUserStream_GpuMemory, &ptr, &size) &&
      ptr && size > 0) {
    ApplyGpuMemoryJson(std::string_view(static_cast<const char*>(ptr), size), out);
  }
}

MemoryPressure AssessMemoryPressure(const MemorySnapshot& memory)
{
  MemoryPressure pressure{};
  if (memory.has_system) {
    pressure.commit_percent = Percent(memory.commit_used_bytes, memory.commit_limit_bytes);
    pressure.commit_high = memory.commit_limit_bytes > 0 && pressure.commit_percent >= kCommitPressurePercent;
  }
  if (memory.has_gpu) {
    pressure.vram_percent = Percent(memory.vram_usage_bytes, memory.vram_budget_bytes);
    pressure.vram_high = memory.vram_budget_bytes > 0 && pressure.vram_percent >= kVramPressurePercent;
  }
  return pressure;
}

std::wstring DescribeSystemMemory(const MemorySnapshot& memory, bool en)
{
  std::wstring text;
  if (memory.has_system) {
    const unsigned commitPercent = Percent(memory.commit_used_bytes, memory.commit_limit_bytes);
    text = en
      ? (L"RAM " + FormatGb(memory.ram_available_bytes, 1) + L"/" + FormatGb(memory.ram_total_bytes, 1) +
          L" GB available, commit " + FormatGb(memory.commit_used_bytes, 1) + L"/" + FormatGb(memory.commit_limit_bytes, 1) +
          L" GB (" + std::to_wstring(commitPercent) + L"%)")
      : (L"사용 가능 RAM " + FormatGb(memory.ram_available_bytes, 1) + L"/" + FormatGb(memory.ram_total_bytes, 1) +
          L" GB, 커밋 " + FormatGb(memory.commit_used_bytes, 1) + L"/" + FormatGb(memory.commit_limit_bytes, 1) +
          L" GB (" + std::to_wstring(commitPercent) + L"%)");
  }
  if (memory.has_process) {
    if (!text.empty()) {
      text += L", ";
    }
    text += en
      ? (L"game commit " + FormatGb(memory.process_private_bytes, 2) + L" GB, working set " +
          FormatGb(memory.process_working_set_bytes, 2) + L" GB")
      : (L"게임 커밋 " + FormatGb(memory.process_private_bytes, 2) + L" GB, 워킹셋 " +
          FormatGb(memory.process_working_set_bytes, 2) + L" GB");
  }
  return text;
}

std::wstring DescribeGpuMemory(const MemorySnapshot& memory, bool en)
{
  if (!memory.has_gpu) {
    return {};
  }
  std::wstring text = memory.gpu_name.empty() ? std::wstring(L"GPU") : memory.gpu_name;
  text += L", VRAM " + FormatGb(memory.vram_usage_bytes, 2) + L"/" + FormatGb(memory.vram_budget_bytes, 2);
  text += en ? L" GB of budget" : L" GB 예산";
  if (memory.vram_budget_bytes > 0) {
    text += L" (" + std::to_wstring(Percent(memory.vram_usage_bytes, memory.vram_budget_bytes)) + L"%)";
  }
  if (memory.gpu_shared_usage_bytes > 0) {
    text += (en ? L", shared " : L", 공유 메모리 ") + FormatGb(memory.gpu_shared_usage_bytes, 2) + L" GB";
  }
  return text;
}

}  // namespace skydiag::dump_tool
