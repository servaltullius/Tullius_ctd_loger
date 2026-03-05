#pragma once

#include "Analyzer.h"
#include "MinidumpUtil.h"
#include "WctTypes.h"

#include <Windows.h>

#include <DbgHelp.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skydiag::dump_tool::internal {

std::wstring EventTypeName(std::uint16_t t);

std::wstring FormatEventDetail(std::uint16_t type, std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d);

std::optional<std::uint32_t> InferMainThreadIdFromEvents(const std::vector<EventRow>& events);

std::optional<double> InferHeartbeatAgeFromEventsSec(const std::vector<EventRow>& events);

std::wstring ResourceKindFromPath(std::wstring_view path);

std::vector<SuspectItem> ComputeStackScanSuspects(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<minidump::ModuleInfo>& modules,
  const std::vector<std::uint32_t>& targetTids,
  i18n::Language lang);

bool TryReadContextFromLocation(void* dumpBase, std::uint64_t dumpSize, const MINIDUMP_LOCATION_DESCRIPTOR& loc, CONTEXT& out);

bool TryComputeStackwalkSuspects(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<minidump::ModuleInfo>& modules,
  const std::vector<std::uint32_t>& targetTids,
  std::uint32_t excTid,
  const std::optional<CONTEXT>& excCtx,
  const std::vector<minidump::ThreadRecord>& threads,
  i18n::Language lang,
  AnalysisResult& out);

void ComputeCrashBucket(AnalysisResult& out);

}  // namespace skydiag::dump_tool::internal

