// End to end: the helper's real dump writer and the production analyzer must
// agree on this process's memory. RAM and commit come from the dump's own
// streams on every machine; the GPU part is checked when a Direct3D device
// lands on a real GPU. CI runners only have the Microsoft Basic Render Driver,
// which the kernel keeps no per-process VRAM statistics for, so it is skipped
// there.

#include <Windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <psapi.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include "Analyzer.h"
#include "GpuMemory.h"
#include "MemorySnapshot.h"
#include "HelperRuntimeTestUtils.h"
#include "SkyrimDiagHelper/DumpProfile.h"
#include "SkyrimDiagHelper/DumpWriter.h"

using skydiag::helper::CaptureKind;
using skydiag::helper::DumpMode;
using skydiag::helper::ResolveDumpProfile;
using skydiag::helper::WriteDumpWithStreams;
using skydiag::tests::runtime::MakeSharedLayout;
using skydiag::tests::runtime::MakeTempDir;
using skydiag::tests::runtime::OpenSelfProcessHandle;
using skydiag::tests::runtime::Require;

namespace {

constexpr std::uint64_t kMiB = 1024ull * 1024ull;

template <typename T>
void SafeRelease(T*& p)
{
  if (p) {
    p->Release();
    p = nullptr;
  }
}

// Two 64 MiB textures in the VRAM of a real GPU, or nothing.
struct GpuLoad
{
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  ID3D11Texture2D* textures[2]{};

  GpuLoad()
  {
    if (FAILED(D3D11CreateDevice(
          nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) {
      return;
    }
    if (!OnRealGpu()) {
      Release();
      return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 4096;
    desc.Height = 4096;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    for (auto*& texture : textures) {
      if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) {
        Release();
        return;
      }
      ID3D11RenderTargetView* view = nullptr;
      if (SUCCEEDED(device->CreateRenderTargetView(texture, nullptr, &view))) {
        const float color[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
        context->ClearRenderTargetView(view, color);  // makes the texture resident
        SafeRelease(view);
      }
    }
    context->Flush();
  }

  ~GpuLoad() { Release(); }

  void Release()
  {
    for (auto*& texture : textures) {
      SafeRelease(texture);
    }
    SafeRelease(context);
    SafeRelease(device);
  }

  bool Ready() const { return textures[1] != nullptr; }

  // Not the Microsoft Basic Render Driver (vendor 0x1414) and with memory of
  // its own.
  bool OnRealGpu() const
  {
    IDXGIDevice* dxgiDevice = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice)))) {
      return false;
    }
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC desc{};
    const bool described = SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc));
    SafeRelease(adapter);
    SafeRelease(dxgiDevice);
    return described && desc.VendorId != 0x1414u && desc.DedicatedVideoMemory > 0;
  }
};

void TestMemoryIsReadBackFromTheDump()
{
  GpuLoad gpu;

  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  Require(
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) != FALSE,
    "GetProcessMemoryInfo");

  const auto outBase = MakeTempDir(L"skydiag_memory_snapshot_e2e");
  const auto dumpPath = outBase / L"SkyrimDiag_Manual_20261012_120000_000.dmp";
  auto shared = MakeSharedLayout();
  const HANDLE process = OpenSelfProcessHandle();
  std::wstring err;
  const bool written = WriteDumpWithStreams(
    process,
    GetCurrentProcessId(),
    dumpPath.wstring(),
    shared.get(),
    sizeof(skydiag::SharedLayout),
    {},
    {},
    /*isCrash=*/false,
    ResolveDumpProfile(DumpMode::kDefault, CaptureKind::Manual),
    /*isProcessSnapshot=*/false,
    &err);
  CloseHandle(process);
  if (!written) {
    std::fprintf(stderr, "dump failed: %ls\n", err.c_str());
  }
  Require(written, "The dump must be written");

  skydiag::dump_tool::AnalyzeOptions opt{};
  opt.language = skydiag::dump_tool::i18n::Language::kEnglish;
  opt.output_dir = outBase.wstring();
  skydiag::dump_tool::AnalysisResult result{};
  Require(skydiag::dump_tool::AnalyzeDump(dumpPath.wstring(), outBase.wstring(), opt, result, &err), "AnalyzeDump failed");

  const auto& m = result.memory;
  Require(m.has_system && m.has_process, "Windows puts RAM, commit and process counters into every dump");
  Require(m.ram_total_bytes > 0 && m.ram_available_bytes <= m.ram_total_bytes, "RAM numbers");
  Require(m.commit_used_bytes > 0 && m.commit_used_bytes <= m.commit_limit_bytes, "commit numbers");
  // Same totals as the live API: catches a missing or doubled page size.
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  Require(GlobalMemoryStatusEx(&status) != FALSE, "GlobalMemoryStatusEx");
  const auto closeTo = [](std::uint64_t a, std::uint64_t b) {
    const std::uint64_t diff = a > b ? a - b : b - a;
    return diff <= b / 50u;  // within 2%
  };
  Require(closeTo(m.ram_total_bytes, status.ullTotalPhys), "RAM total matches the system");
  Require(closeTo(m.commit_limit_bytes, status.ullTotalPageFile), "commit limit matches the system");
  // The dump writer itself allocates, so allow for growth, not for a mix-up.
  Require(
    m.process_private_bytes >= counters.PrivateUsage / 2 && m.process_private_bytes <= counters.PrivateUsage * 4 + 256 * kMiB,
    "the game commit is this process's commit charge");

  bool hasEvidence = false;
  for (const auto& e : result.evidence) {
    hasEvidence = hasEvidence || e.title == L"Memory at capture time";
  }
  Require(hasEvidence, "The memory is listed as evidence");

  if (!gpu.Ready()) {
    std::puts("no real GPU; GPU part skipped");
    Require(!m.has_gpu || m.vram_usage_bytes + m.gpu_shared_usage_bytes > 0, "a GPU part is only written for memory in use");
  } else {
    Require(m.has_gpu, "The helper's GPU memory stream must be read back");
    Require(m.vram_usage_bytes >= 128 * kMiB, "The two 64 MiB textures count as VRAM in use");
    Require(m.vram_budget_bytes >= m.vram_usage_bytes, "The budget covers what is in use");
    Require(!m.gpu_name.empty(), "The adapter is named");
    const auto direct = skydiag::helper::internal::QueryProcessGpuMemory(GetCurrentProcessId());
    Require(!direct.empty() && direct.front().name == m.gpu_name, "The analyzer keeps the most-used adapter");
    std::printf("GPU: %ls, %llu/%llu MiB\n", m.gpu_name.c_str(),
      static_cast<unsigned long long>(m.vram_usage_bytes / kMiB), static_cast<unsigned long long>(m.vram_budget_bytes / kMiB));
  }

  std::filesystem::remove_all(outBase);
}

}  // namespace

// The helper's JSON writer against the analyzer's reader, without a GPU, so
// CI checks the stream contract too.
void TestGpuStreamRoundTrip()
{
  std::vector<skydiag::helper::internal::GpuAdapterMemory> adapters(2);
  adapters[0].name = L"AMD Radeon(TM) Graphics";
  adapters[0].local_usage = 12 * kMiB;
  adapters[0].local_budget = 46ull * 1024 * kMiB;
  adapters[1].name = L"NVIDIA GeForce RTX 4090";
  adapters[1].local_usage = 4946 * kMiB;
  adapters[1].local_budget = 23368 * kMiB;
  adapters[1].nonlocal_usage = 100 * kMiB;
  adapters[1].nonlocal_budget = 44ull * 1024 * kMiB;
  adapters[1].dedicated_bytes = 24ull * 1024 * kMiB;
  const auto json = skydiag::helper::internal::GpuMemoryJson(adapters);
  Require(!json.empty() && skydiag::helper::internal::GpuMemoryJson({}).empty(), "JSON only for adapters in use");

  skydiag::dump_tool::MemorySnapshot m{};
  Require(skydiag::dump_tool::ApplyGpuMemoryJson(json, m), "the analyzer reads what the helper writes");
  Require(m.has_gpu && m.gpu_name == L"NVIDIA GeForce RTX 4090", "the most-used adapter");
  Require(m.vram_usage_bytes == adapters[1].local_usage && m.vram_budget_bytes == adapters[1].local_budget &&
            m.gpu_shared_usage_bytes == adapters[1].nonlocal_usage && m.vram_dedicated_bytes == adapters[1].dedicated_bytes,
    "every field survives the round trip");
}

int main()
{
  try {
    TestGpuStreamRoundTrip();
    TestMemoryIsReadBackFromTheDump();
    std::puts("memory snapshot e2e tests passed");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
