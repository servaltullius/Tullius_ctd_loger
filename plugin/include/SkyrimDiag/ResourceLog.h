#pragma once

#include <cstdint>
#include <string_view>

namespace skydiag::plugin {

// Best-effort: record an interesting resource path (e.g. meshes/*.nif) into shared memory.
// The input must be UTF-8.
void NoteResourceOpen(std::string_view pathUtf8) noexcept;

// Install lightweight resource hooks (best-effort).
bool InstallResourceHooks() noexcept;

// Adaptive sampling to reduce overhead on heavy loose-file bursts.
void ConfigureResourceLogThrottle(
  bool enableAdaptive,
  std::uint32_t highWatermarkPerSec,
  std::uint32_t maxSampleDivisor) noexcept;

}  // namespace skydiag::plugin
