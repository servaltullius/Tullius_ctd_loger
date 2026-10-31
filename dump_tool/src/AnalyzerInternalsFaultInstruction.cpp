#include "AnalyzerInternals.h"

#include "AnalyzerInternalsStackwalkPriv.h"
#include "FaultInstruction.h"
#include "FreedHook.h"
#include "SkyrimDiagProtocol.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace skydiag::dump_tool::internal {

using stackwalk_internal::MinidumpMemoryView;

namespace {

constexpr std::uint32_t kExceptionAccessViolation = 0xC0000005u;

// The dump's memory-info stream entry for `address` (MiniDumpWithFullMemoryInfo
// lists free regions too): freed / reserved / not_executable / executable, or
// empty when the stream is missing or does not cover it.
std::string MemoryStateAt(void* dumpBase, std::uint64_t dumpSize, std::uint64_t address)
{
  void* ptr = nullptr;
  ULONG size = 0;
  if (!minidump::ReadStreamSized(dumpBase, dumpSize, MemoryInfoListStream, &ptr, &size) || !ptr ||
      size < sizeof(MINIDUMP_MEMORY_INFO_LIST)) {
    return {};
  }
  const auto* list = static_cast<const MINIDUMP_MEMORY_INFO_LIST*>(ptr);
  if (list->SizeOfHeader < sizeof(MINIDUMP_MEMORY_INFO_LIST) || list->SizeOfEntry < sizeof(MINIDUMP_MEMORY_INFO) ||
      list->SizeOfHeader > size) {
    return {};
  }
  const std::uint64_t available = (size - list->SizeOfHeader) / list->SizeOfEntry;
  const std::uint64_t count = std::min<std::uint64_t>(list->NumberOfEntries, available);
  const auto* entries = static_cast<const std::uint8_t*>(ptr) + list->SizeOfHeader;
  for (std::uint64_t i = 0; i < count; ++i) {
    MINIDUMP_MEMORY_INFO info{};
    std::memcpy(&info, entries + i * list->SizeOfEntry, sizeof(info));
    if (address < info.BaseAddress || address - info.BaseAddress >= info.RegionSize) {
      continue;
    }
    if (info.State == MEM_FREE) {
      return "freed";
    }
    if (info.State == MEM_RESERVE) {
      return "reserved";
    }
    constexpr ULONG32 kExecute = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const bool executable = (info.Protect & kExecute) != 0u && (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0u;
    return executable ? "executable" : "not_executable";
  }
  return {};
}

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
  // A null_jump target is in the first 64 KB, which is never allocated: it
  // was not freed, so only bad_jump says what the target memory was.
  if (info.verdict == "bad_jump") {
    info.target_state = MemoryStateAt(dumpBase, dumpSize, fault.address);
  }
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

void ResolveFreedHookOwner(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<minidump::ModuleInfo>& modules,
  AnalysisResult& out)
{
  void* ptr = nullptr;
  ULONG size = 0;
  if (!minidump::ReadStreamSized(dumpBase, dumpSize, skydiag::protocol::kMinidumpUserStream_HookPages, &ptr, &size) ||
      !ptr || size == 0u) {
    return;
  }
  HookPageRecordInfo record{};
  if (!ParseHookPageRecordJson(std::string_view(static_cast<const char*>(ptr), size), &record)) {
    return;
  }
  const auto moduleNamed = [&](const std::wstring& name) -> const minidump::ModuleInfo* {
    for (const auto& m : modules) {
      if (_wcsicmp(m.filename.c_str(), name.c_str()) == 0) {
        return &m;
      }
    }
    return nullptr;
  };
  // The game EXE and Windows own no hook stub; a module the dump no longer
  // lists (unloaded) may well be the owner.
  auto info = ResolveFreedHook(record, out.fault_instruction.address, [&](const std::wstring& name) {
    const auto* m = moduleNamed(name);
    return m && (m->is_systemish || m->is_game_exe || minidump::IsLikelyWindowsSystemModulePath(m->path));
  });
  if (info.owner_module.empty()) {
    out.freed_hook = std::move(info);
    return;
  }
  if (const auto* m = moduleNamed(info.owner_module)) {
    info.owner_module_path = m->path;
    info.owner_mod_name = m->inferred_mod_name;
  }
  out.fault_instruction.freed_hook_owner = info.owner_module;
  if (info.exact) {
    wchar_t offset[24]{};
    swprintf_s(offset, L"+0x%llx", static_cast<unsigned long long>(info.owner_offset));
    out.fault_instruction.freed_hook_stub_target = info.owner_module + offset;
  }
  out.freed_hook = std::move(info);
}

}  // namespace skydiag::dump_tool::internal
