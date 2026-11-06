#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "Analyzer.h"

namespace skydiag::dump_tool {

// ADR-0026: the helper's kMinidumpUserStream_GpuDriverEvents JSON; false
// (and `out` untouched) when it is not that.
bool ApplyGpuDriverLogJson(std::string_view json, GpuDriverLog& out);

// Reads the stream when the dump has one (freeze and manual captures).
void ReadGpuDriverLog(void* dumpBase, std::uint64_t dumpSize, GpuDriverLog& out);

// The events that say something went wrong: critical, error and warning.
std::size_t GpuDriverProblemCount(const GpuDriverLog& log);

// "게임 시작 이후 3건: nvlddmkm 153 3번(Error occurred on GPUID: 100), 마지막은
// 캡처 2분 전" / "3 since the game started: ...", or that there were none.
// Empty when the log was not read.
std::wstring DescribeGpuDriverLog(const GpuDriverLog& log, std::uint64_t captureUnix, bool en);

}  // namespace skydiag::dump_tool
