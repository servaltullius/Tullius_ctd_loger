#pragma once

#include "Analyzer.h"
#include "MinidumpUtil.h"
#include "MainThreadWait.h"
#include "ModalDialogWait.h"
#include "WctTypes.h"

#include <Windows.h>

#include <DbgHelp.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skydiag::dump_tool {
class AddressResolver;
}  // namespace skydiag::dump_tool

namespace skydiag::dump_tool::internal {

// The Crash Logger SKSE plugin's own DLL (CrashLogger.dll, or CrashLoggerSSE.dll
// in older builds), by file name.
inline bool IsCrashLoggerModuleName(std::wstring_view filename)
{
  const auto lower = minidump::WideLower(filename);
  return lower == L"crashlogger.dll" || lower == L"crashloggersse.dll";
}

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

// exceptionContext: the faulting context from the exception stream. The
// exception thread's scan starts at its Rsp when that lies in the captured
// stack: by the time the dump is written the thread is inside the crash
// handlers (Crash Logger, WER), and their frames below the fault would
// otherwise take the heaviest slots. No default: a caller passes nullptr
// only when the dump has no exception context.
std::vector<SuspectItem> ComputeStackScanSuspects(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<minidump::ModuleInfo>& modules,
  const std::vector<std::uint32_t>& targetTids,
  std::uint32_t exceptionTid,
  i18n::Language lang,
  const CONTEXT* exceptionContext);

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
  std::vector<ModalStackFrame>* outModalProbeFrames = nullptr,
  const std::vector<std::uint32_t>* sideTids = nullptr,
  std::vector<ThreadTopFrames>* outSideFrames = nullptr,
  const AddressResolver* gameFunctions = nullptr);

// What crash_logger_thread_dump_deadlock reads from the dump itself.
struct CrashLoggerThreadDumpProbe
{
  // The thread other than the main thread and the lock owner with the most
  // Crash Logger return addresses on its stack (raw scan; Crash Logger ships
  // no PDB, so it is matched by module); 0 when none.
  std::uint32_t crash_logger_tid = 0;
  std::wstring crash_logger_module;
  // MainThreadLockOwnerInfo::ip_state of the lock owner.
  std::string owner_ip_state;
};

CrashLoggerThreadDumpProbe ProbeCrashLoggerThreadDump(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<minidump::ModuleInfo>& modules,
  const std::vector<minidump::ThreadRecord>& threads,
  std::uint32_t mainTid,
  std::uint32_t ownerTid);

// ADR-0021: the engine-lock facts a freeze signature matches on: the game
// threads waiting in a delay (out.game_lock_waits), the wait-for cycle, and a
// reader of the game executable's writable data that the dump itself holds.
void FillGameLockSignatureInput(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<minidump::ModuleInfo>& modules,
  const AnalysisResult& out,
  SignatureMatchInput& input);

// What engine_spin_nan reads from the dump for one thread: a floating-point
// compare loop at its instruction pointer (EngineSpinLoop.h), from the code
// bytes the dump holds or else the matching local image, and whether the
// compared XMM register (low lane, from the thread's own context) is NaN.
struct FloatCompareSpinProbe
{
  bool loop_found = false;
  bool nan = false;
  std::uint64_t rip = 0;
  std::string compare;       // comiss / ucomiss / comisd / ucomisd
  std::string nan_register;  // e.g. "xmm0"
  std::uint64_t nan_bits = 0;
  std::uint32_t mxcsr = 0;
};

FloatCompareSpinProbe ProbeFloatCompareSpin(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<minidump::ModuleInfo>& modules,
  const std::vector<minidump::ThreadRecord>& threads,
  std::uint32_t tid);

// ADR-0019: an access violation's faulting instruction, decoded from the
// code bytes the dump holds at the exception address and checked against the
// exception record with the faulting context's registers (FaultInstruction.h).
// Not for a fault inside Crash Logger (its own memory probes).
FaultInstructionInfo ProbeFaultInstruction(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<minidump::ModuleInfo>& modules,
  const std::vector<minidump::ThreadRecord>& threads,
  const AnalysisResult& out,
  const std::optional<CONTEXT>& excCtx);

// ADR-0016: the crash ran the int3 padding of a hook stub that was being
// written; detected only for EXCEPTION_BREAKPOINT outside every module.
HookStubRaceInfo DetectHookStubRace(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<minidump::ModuleInfo>& modules,
  const std::vector<minidump::ThreadRecord>& threads,
  const AnalysisResult& out,
  const std::optional<CONTEXT>& excCtx);

void ComputeCrashBucket(AnalysisResult& out);
// After freeze_analysis is built: a lock_owner_wait or
// crash_logger_thread_dump_deadlock is keyed by the lock owner instead of the
// main thread's wait. True when that changed crash_bucket_key.
bool RekeyLockOwnerFreezeBucket(AnalysisResult& out);

}  // namespace skydiag::dump_tool::internal
