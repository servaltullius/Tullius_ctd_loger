#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "Analyzer.h"

namespace skydiag::dump_tool {

// ADR-0026: the helper's kMinidumpUserStream_GpuDriverEvents JSON; false
// (and `out` untouched) when it is not that.
bool ApplyGpuDriverLogJson(std::string_view json, GpuDriverLog& out);

// Reads the stream when the dump has one (freeze and manual captures). A
// crash dump has none; the helper leaves the same JSON beside it as
// <stem>_GpuDriverEvents.json (AnalyzerPipeline's LoadGpuDriverLog reads it).
void ReadGpuDriverLog(void* dumpBase, std::uint64_t dumpSize, GpuDriverLog& out);

// ADR-0029: a crash this soon after a driver error is worth checking the
// driver for first. After a driver fault or reset (TDR) the game's Direct3D
// device is gone, and the next code to use it crashes wherever it is: on
// 2026-11-10 a mod's texture load crashed 14 s after nvlddmkm 153, and the
// report named the mod. A minute covers a game that limps on a little; an
// error from earlier in the session is evidence only (ADR-0026).
inline constexpr std::uint64_t kGpuDriverErrorNearCrashSec = 60;

// From the newest driver error or warning at or before the capture to the
// capture, in seconds, and that event; none when there is no such event.
struct GpuDriverLastProblem
{
  std::uint64_t seconds_before_capture = 0;
  std::wstring name;  // "nvlddmkm 153"
};
std::optional<GpuDriverLastProblem> LastGpuDriverProblemBefore(const GpuDriverLog& log, std::uint64_t captureUnix);

// The events that say something went wrong: critical, error and warning.
std::size_t GpuDriverProblemCount(const GpuDriverLog& log);

// "게임 시작 이후 3건: nvlddmkm 153 3번(Error occurred on GPUID: 100), 마지막은
// 캡처 2분 전" / "3 since the game started: ...", or that there were none.
// Errors and warnings only; says so when the helper could not read the log to
// the end. Empty when the log was not read.
std::wstring DescribeGpuDriverLog(const GpuDriverLog& log, std::uint64_t captureUnix, bool en);

}  // namespace skydiag::dump_tool
