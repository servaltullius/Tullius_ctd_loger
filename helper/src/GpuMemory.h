#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace skydiag::helper::internal {

// One graphics adapter the game process holds memory on, as the kernel
// graphics driver counts it for that process. local_* is the adapter's own
// memory (VRAM on a discrete GPU) and nonlocal_* is system memory the GPU
// uses; usage and budget are what DXGI's QueryVideoMemoryInfo reports inside
// the game, and what Crash Logger prints as "GPU MEMORY: usage/budget".
struct GpuAdapterMemory
{
  std::wstring name;
  std::uint64_t local_usage = 0;
  std::uint64_t local_budget = 0;
  std::uint64_t nonlocal_usage = 0;
  std::uint64_t nonlocal_budget = 0;
  // Sum of the adapter's non-aperture segments (its dedicated memory).
  std::uint64_t dedicated_bytes = 0;
};

// Adapters with any memory in use by the process, largest local usage first.
// Empty when the process has none or the queries fail. Reads kernel counters
// only (D3DKMTQueryStatistics); it takes well under a millisecond.
std::vector<GpuAdapterMemory> QueryProcessGpuMemory(std::uint32_t pid);

// The JSON the dump carries in its GPU memory user stream; empty for no
// adapters.
std::string GpuMemoryJson(const std::vector<GpuAdapterMemory>& adapters);

}  // namespace skydiag::helper::internal
