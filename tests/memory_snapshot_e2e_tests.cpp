// End to end: the helper's real dump writer and the production analyzer must
// agree on this process's memory. RAM and commit come from the dump's own
// streams on every machine; the GPU part is checked when a hardware Direct3D
// device can be created (CI runners have none, so it is skipped there).

#include <Windows.h>

#include <d3d11.h>
#include <psapi.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>

#include "Analyzer.h"
#include "GpuMemory.h"
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

// A hardware device with two 64 MiB textures in VRAM, or nothing.
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
    std::puts("no hardware Direct3D device; GPU part skipped");
    Require(!m.has_gpu || m.vram_usage_bytes > 0, "a GPU part is only written for memory in use");
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

int main()
{
  try {
    TestMemoryIsReadBackFromTheDump();
    std::puts("memory snapshot e2e tests passed");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
