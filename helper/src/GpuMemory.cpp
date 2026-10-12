#include "GpuMemory.h"

#include <Windows.h>

#include <winternl.h>  // NTSTATUS, which d3dkmthk.h expects

#include <d3dkmthk.h>

#include <algorithm>
#include <array>

#include <nlohmann/json.hpp>

#include "HelperCommon.h"
#include "SkyrimDiagHandle.h"

namespace skydiag::helper::internal {
namespace {

using EnumAdapters2Fn = NTSTATUS(APIENTRY*)(D3DKMT_ENUMADAPTERS2*);
using QueryStatisticsFn = NTSTATUS(APIENTRY*)(const D3DKMT_QUERYSTATISTICS*);
using QueryAdapterInfoFn = NTSTATUS(APIENTRY*)(const D3DKMT_QUERYADAPTERINFO*);
using CloseAdapterFn = NTSTATUS(APIENTRY*)(const D3DKMT_CLOSEADAPTER*);

// Looked up at run time: Wine and older Windows builds may lack them, and a
// missing import would keep the helper from starting at all.
struct KmtApi
{
  EnumAdapters2Fn enumAdapters2 = nullptr;
  QueryStatisticsFn queryStatistics = nullptr;
  QueryAdapterInfoFn queryAdapterInfo = nullptr;
  CloseAdapterFn closeAdapter = nullptr;

  bool Ready() const noexcept { return enumAdapters2 && queryStatistics && closeAdapter; }
};

template <typename Fn>
Fn Lookup(HMODULE module, const char* name)
{
  return reinterpret_cast<Fn>(GetProcAddress(module, name));
}

KmtApi LoadKmtApi()
{
  KmtApi api{};
  HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
  if (!gdi) {
    gdi = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  }
  if (!gdi) {
    return api;
  }
  api.enumAdapters2 = Lookup<EnumAdapters2Fn>(gdi, "D3DKMTEnumAdapters2");
  api.queryStatistics = Lookup<QueryStatisticsFn>(gdi, "D3DKMTQueryStatistics");
  api.queryAdapterInfo = Lookup<QueryAdapterInfoFn>(gdi, "D3DKMTQueryAdapterInfo");
  api.closeAdapter = Lookup<CloseAdapterFn>(gdi, "D3DKMTCloseAdapter");
  return api;
}

bool Succeeded(NTSTATUS status) noexcept
{
  return status >= 0;
}

std::wstring AdapterName(const KmtApi& api, D3DKMT_HANDLE adapter)
{
  if (!api.queryAdapterInfo) {
    return {};
  }
  D3DKMT_ADAPTERREGISTRYINFO info{};
  D3DKMT_QUERYADAPTERINFO query{};
  query.hAdapter = adapter;
  query.Type = KMTQAITYPE_ADAPTERREGISTRYINFO;
  query.pPrivateDriverData = &info;
  query.PrivateDriverDataSize = sizeof(info);
  if (!Succeeded(api.queryAdapterInfo(&query))) {
    return {};
  }
  info.AdapterString[MAX_PATH - 1] = L'\0';
  return info.AdapterString;
}

std::uint64_t DedicatedBytes(const KmtApi& api, const LUID& luid)
{
  D3DKMT_QUERYSTATISTICS adapterQuery{};
  adapterQuery.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
  adapterQuery.AdapterLuid = luid;
  if (!Succeeded(api.queryStatistics(&adapterQuery))) {
    return 0;
  }
  std::uint64_t total = 0;
  const UINT segments = adapterQuery.QueryResult.AdapterInformation.NbSegments;
  for (UINT segment = 0; segment < segments; ++segment) {
    D3DKMT_QUERYSTATISTICS segmentQuery{};
    segmentQuery.Type = D3DKMT_QUERYSTATISTICS_SEGMENT;
    segmentQuery.AdapterLuid = luid;
    segmentQuery.QuerySegment.SegmentId = segment;
    if (Succeeded(api.queryStatistics(&segmentQuery)) &&
        !segmentQuery.QueryResult.SegmentInformation.Aperture) {
      total += segmentQuery.QueryResult.SegmentInformation.CommitLimit;
    }
  }
  return total;
}

bool QueryGroup(
  const KmtApi& api,
  const LUID& luid,
  HANDLE process,
  D3DKMT_MEMORY_SEGMENT_GROUP group,
  std::uint64_t& usage,
  std::uint64_t& budget)
{
  D3DKMT_QUERYSTATISTICS query{};
  query.Type = D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT_GROUP;
  query.AdapterLuid = luid;
  query.hProcess = process;
  query.QueryProcessSegmentGroup = group;
  if (!Succeeded(api.queryStatistics(&query))) {
    return false;
  }
  usage = query.QueryResult.ProcessSegmentGroupInformation.Usage;
  budget = query.QueryResult.ProcessSegmentGroupInformation.Budget;
  return true;
}

}  // namespace

std::vector<GpuAdapterMemory> QueryProcessGpuMemory(std::uint32_t pid)
{
  std::vector<GpuAdapterMemory> out;
  const KmtApi api = LoadKmtApi();
  if (!api.Ready()) {
    return out;
  }
  // The per-process statistics need full query rights; a PSS snapshot or a
  // limited handle is refused, so open a handle of our own.
  skydiag::UniqueHandle process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid));
  if (!process) {
    return out;
  }

  std::array<D3DKMT_ADAPTERINFO, 16> adapters{};
  D3DKMT_ENUMADAPTERS2 enumeration{};
  enumeration.NumAdapters = static_cast<ULONG>(adapters.size());
  enumeration.pAdapters = adapters.data();
  if (!Succeeded(api.enumAdapters2(&enumeration))) {
    return out;
  }
  const ULONG count = std::min<ULONG>(enumeration.NumAdapters, static_cast<ULONG>(adapters.size()));
  for (ULONG i = 0; i < count; ++i) {
    const auto& adapter = adapters[i];
    GpuAdapterMemory memory{};
    const bool local = QueryGroup(
      api, adapter.AdapterLuid, process.get(), D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL,
      memory.local_usage, memory.local_budget);
    const bool nonlocal = QueryGroup(
      api, adapter.AdapterLuid, process.get(), D3DKMT_MEMORY_SEGMENT_GROUP_NON_LOCAL,
      memory.nonlocal_usage, memory.nonlocal_budget);
    if ((local || nonlocal) && (memory.local_usage > 0 || memory.nonlocal_usage > 0)) {
      memory.name = AdapterName(api, adapter.hAdapter);
      memory.dedicated_bytes = DedicatedBytes(api, adapter.AdapterLuid);
      out.push_back(std::move(memory));
    }
    D3DKMT_CLOSEADAPTER close{};
    close.hAdapter = adapter.hAdapter;
    api.closeAdapter(&close);
  }
  std::stable_sort(out.begin(), out.end(), [](const GpuAdapterMemory& a, const GpuAdapterMemory& b) {
    return a.local_usage > b.local_usage;
  });
  return out;
}

std::string GpuMemoryJson(const std::vector<GpuAdapterMemory>& adapters)
{
  if (adapters.empty()) {
    return {};
  }
  nlohmann::json j;
  j["schema_version"] = 1;
  j["adapters"] = nlohmann::json::array();
  for (const auto& adapter : adapters) {
    j["adapters"].push_back({
      { "name", WideToUtf8(adapter.name) },
      { "local_usage", adapter.local_usage },
      { "local_budget", adapter.local_budget },
      { "nonlocal_usage", adapter.nonlocal_usage },
      { "nonlocal_budget", adapter.nonlocal_budget },
      { "dedicated_bytes", adapter.dedicated_bytes },
    });
  }
  return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace skydiag::helper::internal
