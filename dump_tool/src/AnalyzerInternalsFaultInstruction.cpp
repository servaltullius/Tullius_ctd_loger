#include "AnalyzerInternals.h"

#include "AnalyzerInternalsStackwalkPriv.h"
#include "FaultInstruction.h"

#include <array>
#include <cstring>

namespace skydiag::dump_tool::internal {

using stackwalk_internal::MinidumpMemoryView;

namespace {

constexpr std::uint32_t kExceptionAccessViolation = 0xC0000005u;

}  // namespace

FaultInstructionInfo ProbeFaultInstruction(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<minidump::ModuleInfo>& modules,
  const std::vector<minidump::ThreadRecord>& threads,
  const AnalysisResult& out,
  const std::optional<CONTEXT>& excCtx)
{
  if (out.exc_code != kExceptionAccessViolation || out.exc_info.size() < 2u || IsCrashLoggerModuleName(out.fault_module_filename)) {
    return {};
  }
  // The dump's own memory only: a local SkyrimSE.exe has its code encrypted
  // on disk, and a guess from other bytes would be worse than nothing.
  MinidumpMemoryView mem;
  if (!mem.Init(dumpBase, dumpSize, &threads)) {
    return {};
  }

  FaultRecord fault{};
  fault.exception_address = out.exc_addr;
  fault.access_kind = out.exc_info[0];
  fault.address = out.exc_info[1];
  // The registers speak for the fault only when the context is the one at the
  // exception address.
  if (excCtx && excCtx->Rip == out.exc_addr) {
    const CONTEXT& c = *excCtx;
    fault.has_registers = true;
    fault.registers = { c.Rax, c.Rcx, c.Rdx, c.Rbx, c.Rsp, c.Rbp, c.Rsi, c.Rdi,
      c.R8, c.R9, c.R10, c.R11, c.R12, c.R13, c.R14, c.R15 };
  }

  std::array<std::uint8_t, kMaxX64InstructionBytes> code{};
  std::size_t got = 0;
  if (!mem.Read(out.exc_addr, code.data(), code.size(), got)) {
    got = 0;
  }
  auto info = ExplainFaultInstruction(fault, code.data(), got);
  if ((info.verdict != "null_jump" && info.verdict != "bad_jump") || !fault.has_registers) {
    return info;
  }

  // The CPU was sent to a bad address. When a call sent it there, the return
  // address it pushed is on top of the stack, right after that call.
  std::uint64_t ret = 0;
  std::size_t gotRet = 0;
  std::array<std::uint8_t, kMaxX64InstructionBytes> before{};
  std::size_t gotBefore = 0;
  if (!mem.Read(fault.registers[4], &ret, sizeof(ret), gotRet) || gotRet != sizeof(ret) || ret < before.size() ||
      !mem.Read(ret - before.size(), before.data(), before.size(), gotBefore) || gotBefore != before.size()) {
    return info;
  }
  const auto site = minidump::FindModuleIndexForAddress(modules, ret);
  if (!site) {
    return info;
  }
  const auto readQword = [&](std::uint64_t address) -> std::optional<std::uint64_t> {
    std::uint64_t value = 0;
    std::size_t n = 0;
    if (!mem.Read(address, &value, sizeof(value), n) || n != sizeof(value)) {
      return std::nullopt;
    }
    return value;
  };
  std::size_t length = 0;
  if (const auto call = FindCallTo(before.data(), before.size(), ret, fault.address, fault.registers, readQword, &length)) {
    const auto& m = modules[*site];
    info.call_site = minidump::ModulePlusOffsetText(m.filename, ret - length - m.base);
    info.call_instruction = FormatX64Call(*call, ret);
  }
  return info;
}

}  // namespace skydiag::dump_tool::internal
