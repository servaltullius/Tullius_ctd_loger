#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "Analyzer.h"

namespace skydiag::dump_tool {

// Fills the RAM/commit part from the dump's SystemMemoryInfo and
// ProcessVmCounters streams and the VRAM part from the helper's GPU memory
// stream. Fields stay unset for what the dump does not carry.
void ReadMemorySnapshot(void* dumpBase, std::uint64_t dumpSize, MemorySnapshot& out);

// Takes the most-used adapter from the helper's GPU memory JSON.
bool ApplyGpuMemoryJson(std::string_view json, MemorySnapshot& out);

// Pressure means the capture happened close to a limit that makes allocations
// fail: system commit near the commit limit, or the game's VRAM use near the
// budget Windows gives it (past it, Windows starts evicting the game's
// textures to system memory).
inline constexpr unsigned kCommitPressurePercent = 90u;
inline constexpr unsigned kVramPressurePercent = 90u;

struct MemoryPressure
{
  unsigned commit_percent = 0;
  unsigned vram_percent = 0;
  bool commit_high = false;
  bool vram_high = false;

  bool Any() const noexcept { return commit_high || vram_high; }
};

MemoryPressure AssessMemoryPressure(const MemorySnapshot& memory);

// Bytes as GiB with the given decimals ("22.82").
std::wstring FormatGb(std::uint64_t bytes, int decimals);

// "RAM 27.0/61.6 GB available, commit 54.3/101.6 GB (53%), game commit 23.39 GB, working set 11.65 GB"
std::wstring DescribeSystemMemory(const MemorySnapshot& memory, bool en);
// "NVIDIA GeForce RTX 4090, VRAM 4.83/22.82 GB of budget (21%), shared 0.10 GB"
std::wstring DescribeGpuMemory(const MemorySnapshot& memory, bool en);

}  // namespace skydiag::dump_tool
