#pragma once

#include "Analyzer.h"
#include "MinidumpUtil.h"
#include "ModalDialogWait.h"
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

// lastHeartbeatMs: the last main-thread heartbeat (-1 = unknown). Game-state
// times are measured to it when it is later than the newest event.
BlackboxFreezeSummary BuildBlackboxFreezeSummary(
  const std::vector<EventRow>& events,
  bool loadingContext,
  double lastHeartbeatMs = -1.0);

FirstChanceSummary BuildFirstChanceSummary(
  const std::vector<EventRow>& events,
  bool loadingContext);

std::wstring ResourceKindFromPath(std::wstring_view path);

std::vector<SuspectItem> ComputeStackScanSuspects(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<minidump::ModuleInfo>& modules,
  const std::vector<std::uint32_t>& targetTids,
  std::uint32_t exceptionTid,
  i18n::Language lang);

std::vector<std::uint32_t> FindThreadsWithNearStackModule(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<minidump::ModuleInfo>& modules,
  std::wstring_view moduleFilename,
  std::size_t maxSlots);

bool TryReadContextFromLocation(void* dumpBase, std::uint64_t dumpSize, const MINIDUMP_LOCATION_DESCRIPTOR& loc, CONTEXT& out);

bool TryComputeStackwalkSuspects(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<minidump::ModuleInfo>& modules,
  const std::vector<std::uint32_t>& targetTids,
  std::uint32_t preferredTid,
  std::uint32_t excTid,
  const std::optional<CONTEXT>& excCtx,
  const std::vector<minidump::ThreadRecord>& threads,
  i18n::Language lang,
  AnalysisResult& out,
  std::uint32_t modalProbeTid = 0,
  std::vector<ModalStackFrame>* outModalProbeFrames = nullptr);

void ComputeCrashBucket(AnalysisResult& out);

}  // namespace skydiag::dump_tool::internal
