// ADR-0019: the faulting-instruction decoder and its cross-check with the
// exception record (FaultInstruction.h), on synthetic byte sequences. The
// register and address shapes of the null, bad-index and non-canonical cases
// are those of real crash dumps (2026-09-18 SkyrimUpscaler.dll "mov rbx,
// [rax+0x10]" with rax = 0; 2026-10-05 SmoothCam.dll "cmp [r8+r9*8], r10";
// 2026-10-05 CrashLogger.dll "movzx eax, byte ptr [rdi]" on a garbage rdi);
// no bytes of those dumps are used.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "FaultInstruction.h"

using skydiag::dump_tool::DecodeX64Instruction;
using skydiag::dump_tool::DescribeFaultInstruction;
using skydiag::dump_tool::DescribeFaultInstructionPlain;
using skydiag::dump_tool::ExplainFaultInstruction;
using skydiag::dump_tool::FaultInstructionInfo;
using skydiag::dump_tool::FaultInstructionReportText;
using skydiag::dump_tool::FaultRecord;
using skydiag::dump_tool::FindCallTo;
using skydiag::dump_tool::FormatX64Call;
using skydiag::dump_tool::FormatX64MemoryOperand;
using skydiag::dump_tool::IsCanonicalX64Address;
using skydiag::dump_tool::kAccessViolationExecute;
using skydiag::dump_tool::kAccessViolationNoAddress;
using skydiag::dump_tool::kAccessViolationRead;
using skydiag::dump_tool::kAccessViolationWrite;
using skydiag::dump_tool::X64EffectiveAddress;
using skydiag::dump_tool::X64Instruction;
using skydiag::dump_tool::X64MemoryAccess;
using skydiag::dump_tool::X64Registers;

namespace {

enum Reg : int
{
  kRax,
  kRcx,
  kRdx,
  kRbx,
  kRsp,
  kRbp,
  kRsi,
  kRdi,
  kR8,
  kR9,
  kR10,
  kR11,
  kR12,
  kR13,
  kR14,
  kR15,
};

constexpr std::uint64_t kRip = 0x7FF71FB5F101ull;

X64Instruction Decode(const std::vector<std::uint8_t>& bytes)
{
  const auto insn = DecodeX64Instruction(bytes.data(), bytes.size());
  assert(insn.has_value());
  assert(insn->length == bytes.size());
  return *insn;
}

void ExpectMemory(
  const std::vector<std::uint8_t>& bytes,
  const char* mnemonic,
  X64MemoryAccess access,
  std::uint8_t size,
  const char* operand)
{
  const auto insn = Decode(bytes);
  assert(insn.mnemonic == mnemonic);
  assert(insn.has_memory);
  assert(insn.access == access);
  assert(insn.access_size == size);
  assert(FormatX64MemoryOperand(insn) == operand);
}

void ExpectRejected(const std::vector<std::uint8_t>& bytes)
{
  assert(!DecodeX64Instruction(bytes.data(), bytes.size()).has_value());
}

void TestIntegerMovesAndLoads()
{
  const auto r = X64MemoryAccess::kRead;
  const auto w = X64MemoryAccess::kWrite;
  ExpectMemory({ 0x48, 0x8B, 0x41, 0x24 }, "mov", r, 8, "qword ptr [rcx+0x24]");
  ExpectMemory({ 0x48, 0x8B, 0x58, 0x10 }, "mov", r, 8, "qword ptr [rax+0x10]");
  ExpectMemory({ 0x8B, 0x01 }, "mov", r, 4, "dword ptr [rcx]");
  ExpectMemory({ 0x8A, 0x41, 0xF0 }, "mov", r, 1, "byte ptr [rcx-0x10]");
  ExpectMemory({ 0x89, 0x08 }, "mov", w, 4, "dword ptr [rax]");
  ExpectMemory({ 0x88, 0x48, 0x10 }, "mov", w, 1, "byte ptr [rax+0x10]");
  ExpectMemory({ 0x66, 0x89, 0x41, 0x08 }, "mov", w, 2, "word ptr [rcx+0x8]");
  ExpectMemory({ 0x48, 0x89, 0x8B, 0x00, 0x01, 0x00, 0x00 }, "mov", w, 8, "qword ptr [rbx+0x100]");
  ExpectMemory({ 0x0F, 0xB6, 0x07 }, "movzx", r, 1, "byte ptr [rdi]");
  ExpectMemory({ 0x0F, 0xB7, 0x41, 0x10 }, "movzx", r, 2, "word ptr [rcx+0x10]");
  ExpectMemory({ 0x48, 0x0F, 0xBE, 0x42, 0x05 }, "movsx", r, 1, "byte ptr [rdx+0x5]");
  ExpectMemory({ 0x48, 0x63, 0x41, 0x08 }, "movsxd", r, 4, "dword ptr [rcx+0x8]");
  ExpectMemory({ 0x48, 0xC7, 0x41, 0x08, 0x00, 0x00, 0x00, 0x00 }, "mov", w, 8, "qword ptr [rcx+0x8]");
  ExpectMemory({ 0xC6, 0x41, 0x08, 0x01 }, "mov", w, 1, "byte ptr [rcx+0x8]");
  ExpectMemory({ 0x48, 0x0F, 0x44, 0x41, 0x08 }, "cmove", r, 8, "qword ptr [rcx+0x8]");
  ExpectMemory({ 0x0F, 0x94, 0x41, 0x08 }, "sete", w, 1, "byte ptr [rcx+0x8]");
}

void TestCompareTestAndArithmetic()
{
  const auto r = X64MemoryAccess::kRead;
  const auto rw = X64MemoryAccess::kReadWrite;
  ExpectMemory({ 0x48, 0x39, 0x41, 0x08 }, "cmp", r, 8, "qword ptr [rcx+0x8]");
  ExpectMemory({ 0x4F, 0x39, 0x14, 0xC8 }, "cmp", r, 8, "qword ptr [r8+r9*8]");
  ExpectMemory({ 0x3B, 0x41, 0x10 }, "cmp", r, 4, "dword ptr [rcx+0x10]");
  ExpectMemory({ 0x83, 0x79, 0x10, 0x00 }, "cmp", r, 4, "dword ptr [rcx+0x10]");
  ExpectMemory({ 0x85, 0x41, 0x10 }, "test", r, 4, "dword ptr [rcx+0x10]");
  ExpectMemory({ 0xF6, 0x41, 0x08, 0x01 }, "test", r, 1, "byte ptr [rcx+0x8]");
  ExpectMemory({ 0xF7, 0x41, 0x08, 0x00, 0x00, 0x01, 0x00 }, "test", r, 4, "dword ptr [rcx+0x8]");
  ExpectMemory({ 0x81, 0x41, 0x10, 0x00, 0x01, 0x00, 0x00 }, "add", rw, 4, "dword ptr [rcx+0x10]");
  ExpectMemory({ 0x66, 0x81, 0x41, 0x10, 0x00, 0x01 }, "add", rw, 2, "word ptr [rcx+0x10]");
  ExpectMemory({ 0x48, 0x01, 0x41, 0x10 }, "add", rw, 8, "qword ptr [rcx+0x10]");
  ExpectMemory({ 0x48, 0x03, 0x41, 0x10 }, "add", r, 8, "qword ptr [rcx+0x10]");
  ExpectMemory({ 0xFF, 0x41, 0x08 }, "inc", rw, 4, "dword ptr [rcx+0x8]");
  ExpectMemory({ 0xF0, 0xFF, 0x49, 0x08 }, "dec", rw, 4, "dword ptr [rcx+0x8]");
  ExpectMemory({ 0xF0, 0x0F, 0xC1, 0x01 }, "xadd", rw, 4, "dword ptr [rcx]");
  ExpectMemory({ 0xF0, 0x48, 0x0F, 0xB1, 0x11 }, "cmpxchg", rw, 8, "qword ptr [rcx]");
  ExpectMemory({ 0xC1, 0x61, 0x08, 0x04 }, "shl", rw, 4, "dword ptr [rcx+0x8]");
  ExpectMemory({ 0x0F, 0xBA, 0x61, 0x08, 0x03 }, "bt", r, 4, "dword ptr [rcx+0x8]");
  ExpectMemory({ 0x48, 0x0F, 0xAF, 0x41, 0x08 }, "imul", r, 8, "qword ptr [rcx+0x8]");
  ExpectMemory({ 0x48, 0x87, 0x01 }, "xchg", rw, 8, "qword ptr [rcx]");
}

void TestSse()
{
  const auto r = X64MemoryAccess::kRead;
  const auto w = X64MemoryAccess::kWrite;
  ExpectMemory({ 0xF3, 0x0F, 0x10, 0x43, 0x54 }, "movss", r, 4, "dword ptr [rbx+0x54]");
  ExpectMemory({ 0xF2, 0x0F, 0x11, 0x4C, 0x24, 0x20 }, "movsd", w, 8, "qword ptr [rsp+0x20]");
  ExpectMemory({ 0x0F, 0x10, 0x40, 0x30 }, "movups", r, 16, "xmmword ptr [rax+0x30]");
  ExpectMemory({ 0x41, 0x0F, 0x11, 0x06 }, "movups", w, 16, "xmmword ptr [r14]");
  ExpectMemory({ 0x41, 0x0F, 0x28, 0x06 }, "movaps", r, 16, "xmmword ptr [r14]");
  ExpectMemory({ 0x66, 0x0F, 0x29, 0x01 }, "movapd", w, 16, "xmmword ptr [rcx]");
  ExpectMemory({ 0x66, 0x0F, 0x7F, 0x84, 0x24, 0x20, 0x01, 0x00, 0x00 }, "movdqa", w, 16, "xmmword ptr [rsp+0x120]");
  ExpectMemory({ 0xF3, 0x0F, 0x6F, 0x01 }, "movdqu", r, 16, "xmmword ptr [rcx]");
  ExpectMemory({ 0x66, 0x48, 0x0F, 0x6E, 0x01 }, "movq", r, 8, "qword ptr [rcx]");
  ExpectMemory({ 0x66, 0x0F, 0x7E, 0x01 }, "movd", w, 4, "dword ptr [rcx]");
  ExpectMemory({ 0xF3, 0x0F, 0x7E, 0x01 }, "movq", r, 8, "qword ptr [rcx]");
  ExpectMemory({ 0x66, 0x0F, 0xD6, 0x01 }, "movq", w, 8, "qword ptr [rcx]");
  ExpectMemory({ 0x0F, 0x2E, 0x41, 0x10 }, "ucomiss", r, 4, "dword ptr [rcx+0x10]");
  ExpectMemory({ 0x66, 0x0F, 0x2F, 0x41, 0x10 }, "comisd", r, 8, "qword ptr [rcx+0x10]");
  ExpectMemory({ 0xF3, 0x0F, 0x58, 0x41, 0x10 }, "addss", r, 4, "dword ptr [rcx+0x10]");
  ExpectMemory({ 0xF2, 0x0F, 0x59, 0x41, 0x10 }, "mulsd", r, 8, "qword ptr [rcx+0x10]");
  ExpectMemory({ 0x0F, 0x59, 0x01 }, "mulps", r, 16, "xmmword ptr [rcx]");
  ExpectMemory({ 0xF3, 0x48, 0x0F, 0x2A, 0x01 }, "cvtsi2ss", r, 8, "qword ptr [rcx]");
  ExpectMemory({ 0xF3, 0x0F, 0x5A, 0x01 }, "cvtss2sd", r, 4, "dword ptr [rcx]");
  ExpectMemory({ 0x0F, 0xC6, 0x01, 0x1B }, "shufps", r, 16, "xmmword ptr [rcx]");
  ExpectMemory({ 0xF3, 0x0F, 0xC2, 0x01, 0x01 }, "cmpss", r, 4, "dword ptr [rcx]");

  // Legacy SSE that needs a 16-byte aligned operand, and the forms that do not.
  assert(Decode({ 0x41, 0x0F, 0x28, 0x06 }).requires_alignment);
  assert(Decode({ 0x66, 0x0F, 0x7F, 0x01 }).requires_alignment);
  assert(Decode({ 0x0F, 0x59, 0x01 }).requires_alignment);
  assert(!Decode({ 0x0F, 0x10, 0x40, 0x30 }).requires_alignment);
  assert(!Decode({ 0xF3, 0x0F, 0x6F, 0x01 }).requires_alignment);
  assert(!Decode({ 0xF3, 0x0F, 0x58, 0x41, 0x10 }).requires_alignment);

  // A register-to-register form has no memory operand.
  const auto regToReg = Decode({ 0x0F, 0x28, 0xC1 });
  assert(regToReg.mnemonic == "movaps" && !regToReg.has_memory);
}

void TestAddressing()
{
  // SIB without a base: [index*scale + disp32].
  auto insn = Decode({ 0x8B, 0x04, 0x8D, 0x00, 0x10, 0x00, 0x00 });
  assert(insn.memory.base == -1 && insn.memory.index == kRcx && insn.memory.scale == 4);
  assert(FormatX64MemoryOperand(insn) == "dword ptr [rcx*4+0x1000]");
  // r13 as a base needs a displacement; r12 needs a SIB.
  insn = Decode({ 0x41, 0x8B, 0x45, 0x00 });
  assert(insn.memory.base == kR13 && insn.memory.displacement == 0);
  insn = Decode({ 0x41, 0x8B, 0x04, 0x24 });
  assert(insn.memory.base == kR12 && insn.memory.index == -1);
  // rsp cannot be an index, r12 (REX.X) can.
  insn = Decode({ 0x8B, 0x04, 0x24 });
  assert(insn.memory.base == kRsp && insn.memory.index == -1);
  insn = Decode({ 0x42, 0x8B, 0x04, 0x21 });
  assert(insn.memory.base == kRcx && insn.memory.index == kR12 && insn.memory.scale == 1);
  // disp32, negative.
  insn = Decode({ 0x48, 0x8B, 0x81, 0xF0, 0xFF, 0xFF, 0xFF });
  assert(insn.memory.displacement == -0x10);
  // RIP-relative: counted from the end of the instruction, immediate included.
  insn = Decode({ 0xC6, 0x05, 0x10, 0x00, 0x00, 0x00, 0x01 });
  assert(insn.memory.rip_relative && insn.access == X64MemoryAccess::kWrite && insn.length == 7u);
  assert(FormatX64MemoryOperand(insn) == "byte ptr [rip+0x10]");
  X64Registers regs{};
  assert(X64EffectiveAddress(insn, 0x1000, regs) == 0x1000u + 7u + 0x10u);
  insn = Decode({ 0x48, 0x8B, 0x05, 0xF0, 0xFF, 0xFF, 0xFF });
  assert(X64EffectiveAddress(insn, 0x1000, regs) == 0x1000u + 7u - 0x10u);

  regs[kR8] = 0x21509A50000ull;
  regs[kR9] = 0x689ACull;
  insn = Decode({ 0x4F, 0x39, 0x14, 0xC8 });
  assert(X64EffectiveAddress(insn, kRip, regs) == 0x21509D94D60ull);
}

void TestBranchesAndStack()
{
  auto insn = Decode({ 0xFF, 0x50, 0x18 });
  assert(insn.mnemonic == "call" && insn.is_call && insn.branch_through_memory && insn.access_size == 8u);
  assert(FormatX64MemoryOperand(insn) == "qword ptr [rax+0x18]");
  insn = Decode({ 0xFF, 0x15, 0x10, 0x00, 0x00, 0x00 });
  assert(insn.is_call && insn.memory.rip_relative);
  insn = Decode({ 0xFF, 0xD0 });
  assert(insn.is_call && insn.call_register == kRax && !insn.has_memory);
  insn = Decode({ 0x41, 0xFF, 0xD3 });
  assert(insn.is_call && insn.call_register == kR11);
  insn = Decode({ 0xE8, 0x10, 0x00, 0x00, 0x00 });
  assert(insn.is_call && insn.call_is_relative && insn.call_relative == 0x10);
  assert(FormatX64Call(insn, 0x1000) == "call 0x1010");
  insn = Decode({ 0xFF, 0x60, 0x08 });
  assert(insn.mnemonic == "jmp" && !insn.is_call && insn.branch_through_memory);
  insn = Decode({ 0xFF, 0x70, 0x08 });
  assert(insn.mnemonic == "push" && insn.access == X64MemoryAccess::kRead && !insn.stack_slot);

  X64Registers regs{};
  regs[kRsp] = 0x5000;
  insn = Decode({ 0x50 });
  assert(insn.mnemonic == "push" && insn.stack_slot && insn.access == X64MemoryAccess::kWrite);
  assert(X64EffectiveAddress(insn, 0, regs) == 0x4FF8u);
  insn = Decode({ 0x41, 0x5F });
  assert(insn.mnemonic == "pop" && insn.stack_slot && insn.access == X64MemoryAccess::kRead);
  assert(X64EffectiveAddress(insn, 0, regs) == 0x5000u);
}

void TestNoAccessAndRejections()
{
  // lea, nop and prefetch compute an address but never touch memory.
  for (const auto& bytes : std::vector<std::vector<std::uint8_t>>{
         { 0x48, 0x8D, 0x41, 0x10 }, { 0x0F, 0x1F, 0x44, 0x00, 0x00 }, { 0x0F, 0x18, 0x09 } }) {
    const auto insn = Decode(bytes);
    assert(!insn.has_memory);
  }
  ExpectRejected({ 0x48, 0x8D, 0xC1 });                                      // lea without memory
  ExpectRejected({ 0xC5, 0xF8, 0x10, 0x00 });                                // VEX vmovups
  ExpectRejected({ 0x62, 0xF1, 0x7C, 0x48, 0x10, 0x00 });                    // EVEX
  ExpectRejected({ 0x67, 0x8B, 0x01 });                                      // 32-bit addressing
  ExpectRejected({ 0x65, 0x48, 0x8B, 0x04, 0x25, 0x30, 0x00, 0x00, 0x00 }); // gs:
  ExpectRejected({ 0x66, 0x0F, 0x38, 0x00, 0x01 });                          // 0F 38
  ExpectRejected({ 0x0F, 0x6F, 0x01 });                                      // MMX
  ExpectRejected({ 0xF3, 0x48, 0x0F, 0xB8, 0x01 });                          // popcnt
  ExpectRejected({ 0xF3, 0x48, 0xA5 });                                      // rep movsq
  ExpectRejected({ 0xFF, 0x18 });                                            // call far
  ExpectRejected({ 0xF3, 0xF2, 0x0F, 0x10, 0x01 });                          // F3 and F2
  ExpectRejected({ 0x48, 0x8B, 0x41 });                                      // disp8 cut off
  ExpectRejected({ 0x48, 0x8B, 0x80, 0x00, 0x00 });                          // disp32 cut off
  ExpectRejected({ 0x81, 0x41, 0x10, 0x00, 0x01 });                          // imm32 cut off
  ExpectRejected({ 0x48 });
  ExpectRejected({ 0x0F });
  ExpectRejected(std::vector<std::uint8_t>(16, 0x66));
  assert(!DecodeX64Instruction(nullptr, 4).has_value());
  assert(!DecodeX64Instruction(std::vector<std::uint8_t>{ 0x8B, 0x01 }.data(), 0).has_value());
  // Trailing bytes are the next instruction's.
  const std::vector<std::uint8_t> two = { 0x48, 0x8B, 0x58, 0x10, 0x48, 0x8B, 0x35, 0x19, 0xE0, 0x22, 0x00 };
  const auto first = DecodeX64Instruction(two.data(), two.size());
  assert(first && first->length == 4u);
}

void TestCanonical()
{
  assert(IsCanonicalX64Address(0));
  assert(IsCanonicalX64Address(0x00007FFFFFFFFFFFull));
  assert(IsCanonicalX64Address(0xFFFF800000000000ull));
  assert(!IsCanonicalX64Address(0x0000800000000000ull));
  assert(!IsCanonicalX64Address(0x89E2DDDE1A260000ull));
}

FaultRecord Fault(std::uint64_t kind, std::uint64_t address, const X64Registers& regs)
{
  FaultRecord fault{};
  fault.exception_address = kRip;
  fault.access_kind = kind;
  fault.address = address;
  fault.has_registers = true;
  fault.registers = regs;
  return fault;
}

FaultInstructionInfo Explain(const FaultRecord& fault, const std::vector<std::uint8_t>& code)
{
  return ExplainFaultInstruction(fault, code.data(), code.size());
}

// The task's example and the 2026-09-18 shape: a read through a register that
// held 0.
void TestNullBaseSentence()
{
  X64Registers regs{};
  regs[kRcx] = 0;
  const auto info = Explain(Fault(kAccessViolationRead, 0x24, regs), { 0x48, 0x8B, 0x41, 0x24, 0xCC, 0xCC });
  assert(info.verdict == "null_pointer");
  assert(info.access == "read" && info.address_matches);
  assert(info.mnemonic == "mov" && info.operand == "qword ptr [rcx+0x24]" && info.bytes == "48 8B 41 24");
  assert(info.base_register == "rcx" && info.base_value == 0u && info.effective_address == 0x24u);
  assert(DescribeFaultInstruction(info, L"SkyrimSE.exe+0x3ff101", true) ==
         L"The instruction at SkyrimSE.exe+0x3ff101 (mov) tried to read 8 bytes from [rcx+0x24], and rcx was 0: it used "
         L"an object that was already gone or never created (a null pointer).");
  assert(DescribeFaultInstruction(info, L"SkyrimSE.exe+0x3ff101", false) ==
         L"SkyrimSE.exe+0x3ff101의 명령어(mov)가 [rcx+0x24]에서 8바이트를 읽으려 했는데 rcx가 0이었습니다. 이미 사라졌거나 "
         L"만들어진 적 없는 오브젝트를 썼다는 뜻입니다(null 포인터).");
  assert(DescribeFaultInstructionPlain(info, true) ==
         L"It crashed while using an object that was already gone or never created (a null pointer).");
  assert(DescribeFaultInstructionPlain(info, false) == L"이미 사라졌거나 만들어진 적 없는 오브젝트를 쓰다가 튕겼습니다(null 포인터).");
  assert(FaultInstructionReportText(info) == "mov qword ptr [rcx+0x24] (48 8B 41 24), read 0x24, rcx=0x0 -> null_pointer");

  // A write through a near-null pointer.
  regs[kRax] = 0x8;
  const auto write = Explain(Fault(kAccessViolationWrite, 0x18, regs), { 0x88, 0x48, 0x10 });
  assert(write.verdict == "null_pointer" && write.access == "write");
  const auto text = DescribeFaultInstruction(write, L"Mod.dll+0x10", true);
  assert(text.find(L"tried to write 1 byte to [rax+0x10], and rax was 0x8, an address near 0") != std::wstring::npos);

  // A call through a null object's virtual table.
  regs[kRax] = 0;
  const auto call = Explain(Fault(kAccessViolationRead, 0x18, regs), { 0xFF, 0x50, 0x18 });
  assert(call.verdict == "null_pointer" && call.branch_through_memory);
  assert(DescribeFaultInstruction(call, L"", true) ==
         L"The instruction (call) tried to read the address of the function to call from [rax+0x18], and rax was 0: it "
         L"called a function of an object that was already gone or never created (a null pointer).");
  assert(DescribeFaultInstructionPlain(call, false).find(L"오브젝트의 함수를") != std::wstring::npos);
}

void TestOtherVerdicts()
{
  X64Registers regs{};

  // A real-looking pointer with nothing behind it.
  regs[kRcx] = 0x7FF012345000ull;
  auto info = Explain(Fault(kAccessViolationRead, 0x7FF012345024ull, regs), { 0x48, 0x8B, 0x41, 0x24 });
  assert(info.verdict == "bad_pointer");
  auto text = DescribeFaultInstruction(info, L"Mod.dll+0x10", true);
  assert(text.find(L"rcx was 0x7FF012345000; there was no readable memory at 0x7FF012345024") != std::wstring::npos);
  assert(text.find(L"freed already") != std::wstring::npos);
  info = Explain(Fault(kAccessViolationWrite, 0x7FF012345024ull, regs), { 0x48, 0x89, 0x41, 0x24 });
  assert(info.verdict == "bad_pointer");
  assert(DescribeFaultInstruction(info, L"", false).find(L"쓸 수 있는 메모리가 없었습니다") != std::wstring::npos);

  // 2026-10-05 SmoothCam.dll shape: a valid base and a huge index.
  regs[kR8] = 0x21509A50000ull;
  regs[kR9] = 0x689ACull;
  regs[kR10] = 0x10E48ull;
  info = Explain(Fault(kAccessViolationRead, 0x21509D94D60ull, regs), { 0x4F, 0x39, 0x14, 0xC8, 0x73, 0x17 });
  assert(info.verdict == "bad_index");
  assert(info.base_register == "r8" && info.index_register == "r9" && info.scale == 8u);
  text = DescribeFaultInstruction(info, L"SmoothCam.dll+0x15fcc", true);
  assert(text == L"The instruction at SmoothCam.dll+0x15fcc (cmp) tried to read 8 bytes from [r8+r9*8]; the address is r8 "
                 L"(0x21509A50000) + r9 (0x689AC) × 8, and there was no readable memory at 0x21509D94D60: the index (r9) "
                 L"was probably out of range, or the pointer was wrong.");
  assert(!DescribeFaultInstructionPlain(info, false).empty());

  // 2026-10-05 CrashLogger.dll shape: a general-protection fault (no address)
  // on a pointer that cannot be an address.
  regs[kRdi] = 0x89E2DDDE1A260000ull;
  info = Explain(Fault(kAccessViolationRead, kAccessViolationNoAddress, regs), { 0x0F, 0xB6, 0x07 });
  assert(info.verdict == "noncanonical" && info.address_matches && info.base_register == "rdi");
  text = DescribeFaultInstruction(info, L"Mod.dll+0x686e0", true);
  assert(text.find(L"rdi was 0x89E2DDDE1A260000, which cannot be a memory address") != std::wstring::npos);
  assert(DescribeFaultInstruction(info, L"", false).find(L"rdi가 0x89E2DDDE1A260000였습니다.") != std::wstring::npos);

  // A general-protection fault on a canonical, aligned-enough operand: the
  // decoder cannot say why, so nothing (no "address 0xFFFFFFFFFFFFFFFF").
  regs[kRcx] = 0x10000;
  info = Explain(Fault(kAccessViolationRead, kAccessViolationNoAddress, regs), { 0x48, 0x8B, 0x41, 0x24 });
  assert(info.verdict.empty() && DescribeFaultInstruction(info, L"x", true).empty());

  // movaps on an address that is not 16-byte aligned.
  regs[kRax] = 0x10000008ull;
  info = Explain(Fault(kAccessViolationRead, kAccessViolationNoAddress, regs), { 0x0F, 0x28, 0x00 });
  assert(info.verdict == "misaligned");
  assert(DescribeFaultInstruction(info, L"", true).find(L"aligned to 16 bytes, but [rax] was 0x10000008") !=
         std::wstring::npos);

  // A RIP-relative global.
  info = Explain(Fault(kAccessViolationRead, kRip + 7u + 0x10u, regs), { 0x48, 0x8B, 0x05, 0x10, 0x00, 0x00, 0x00 });
  assert(info.verdict == "fixed_address" && info.base_register.empty());

  // push on a broken stack pointer.
  regs[kRsp] = 0x3000;
  info = Explain(Fault(kAccessViolationWrite, 0x2FF8, regs), { 0x53 });
  assert(info.verdict == "stack");
  assert(DescribeFaultInstruction(info, L"", true).find(L"the stack pointer was broken") != std::wstring::npos);
  // An rsp operand with no index on a broken stack pointer.
  info = Explain(Fault(kAccessViolationRead, 0x3020, regs), { 0x48, 0x8B, 0x44, 0x24, 0x20 });
  assert(info.verdict == "stack");

  // A local array read with a wild index is the index, not the stack:
  // mov rax,[rsp+rcx*8+0x20].
  regs[kRsp] = 0x7FF000005000ull;
  regs[kRcx] = 0x10000000ull;
  info = Explain(Fault(kAccessViolationRead, 0x7FF080005020ull, regs), { 0x48, 0x8B, 0x44, 0xCC, 0x20 });
  assert(info.verdict == "bad_index" && info.base_register == "rsp" && info.index_register == "rcx");

  // A table at a fixed address chosen by an index (mov eax,[rcx*4+0x1000000])
  // is the index's address, not a fixed one, even with a small index.
  regs[kRcx] = 0x20;
  info = Explain(Fault(kAccessViolationRead, 0x1000080ull, regs), { 0x8B, 0x04, 0x8D, 0x00, 0x00, 0x00, 0x01 });
  assert(info.verdict == "bad_index" && info.base_register.empty() && info.index_register == "rcx");
  text = DescribeFaultInstruction(info, L"", true);
  assert(text.find(L"the address is rcx (0x20) × 4 + 0x1000000") != std::wstring::npos);
  assert(text.find(L"fixed address") == std::wstring::npos);
  // The everyday sentence fits a write as well as a read.
  info = Explain(Fault(kAccessViolationWrite, 0x1000080ull, regs), { 0x89, 0x04, 0x8D, 0x00, 0x00, 0x00, 0x01 });
  assert(info.verdict == "bad_index" && info.access == "write");
  assert(DescribeFaultInstructionPlain(info, true) ==
         L"It crashed using a position outside a list or table (a wrong index), or a broken pointer.");

  // An 8-byte read that runs into the next page faults on that page.
  regs[kRcx] = 0x7FF012345FF8ull;
  info = Explain(Fault(kAccessViolationRead, 0x7FF012346000ull, regs), { 0x48, 0x8B, 0x41, 0x04 });
  assert(info.verdict == "bad_pointer" && info.address_matches);
}

// The registers do not give the faulting address: only read/write and the
// address are stated, never a register.
void TestAddressOnly()
{
  X64Registers regs{};
  regs[kRcx] = 0x1000;
  auto info = Explain(Fault(kAccessViolationRead, 0x24, regs), { 0x48, 0x8B, 0x41, 0x24 });
  assert(info.verdict == "address_only" && !info.address_matches && info.base_register.empty());
  auto text = DescribeFaultInstruction(info, L"Mod.dll+0x10", true);
  assert(text == L"The code at Mod.dll+0x10 tried to read memory at 0x24 and could not. An address this close to 0 usually "
                 L"means a null pointer: an object that was already gone or never created.");
  assert(text.find(L"rcx") == std::wstring::npos && text.find(L"mov") == std::wstring::npos);
  assert(DescribeFaultInstructionPlain(info, true).empty());

  // A context that is not the one at the exception address.
  auto fault = Fault(kAccessViolationWrite, 0x7FF012345024ull, regs);
  fault.has_registers = false;
  info = Explain(fault, { 0x48, 0x89, 0x41, 0x24 });
  assert(info.verdict == "address_only");
  assert(DescribeFaultInstruction(info, L"", false) == L"코드가 0x7FF012345024 주소에 메모리를 쓰려다 실패했습니다.");

  // A write fault on an instruction that only reads.
  regs[kRcx] = 0;
  info = Explain(Fault(kAccessViolationWrite, 0x24, regs), { 0x48, 0x8B, 0x41, 0x24 });
  assert(info.verdict == "address_only");
}

void TestNothingToSay()
{
  X64Registers regs{};
  // No code bytes, an unknown instruction, lea, an exception kind that is not
  // read/write/execute.
  assert(Explain(Fault(kAccessViolationRead, 0x24, regs), {}).verdict.empty());
  assert(Explain(Fault(kAccessViolationRead, 0x24, regs), { 0xC5, 0xF8, 0x10, 0x41, 0x24 }).verdict.empty());
  assert(Explain(Fault(kAccessViolationRead, 0x24, regs), { 0x48, 0x8D, 0x41, 0x24 }).verdict.empty());
  assert(Explain(Fault(2, 0x24, regs), { 0x48, 0x8B, 0x41, 0x24 }).verdict.empty());
  // An execute fault somewhere other than the instruction pointer.
  assert(Explain(Fault(kAccessViolationExecute, 0x24, regs), { 0x48, 0x8B, 0x41, 0x24 }).verdict.empty());
  FaultInstructionInfo none{};
  assert(DescribeFaultInstruction(none, L"x", true).empty() && DescribeFaultInstructionPlain(none, true).empty());
  assert(FaultInstructionReportText(none).empty());
}

void TestExecute()
{
  FaultRecord fault{};
  fault.access_kind = kAccessViolationExecute;
  fault.exception_address = 0;
  fault.address = 0;
  auto info = ExplainFaultInstruction(fault, nullptr, 0);
  assert(info.verdict == "null_jump" && info.access == "execute" && info.mnemonic.empty());
  assert(DescribeFaultInstruction(info, L"", true).rfind(L"The code tried to run instructions at 0x0: it called a "
                                                          L"function through an empty (null) pointer", 0) == 0);
  // A fetch from an unmapped page is reported as a read of the instruction
  // pointer itself.
  fault.access_kind = kAccessViolationRead;
  fault.exception_address = 0x1F2E3D4C5B60ull;
  fault.address = 0x1F2E3D4C5B60ull;
  info = ExplainFaultInstruction(fault, nullptr, 0);
  assert(info.verdict == "bad_jump");
  info.call_site = L"SkyrimSE.exe+0x1234";
  info.call_instruction = "call qword ptr [rax+0x18]";
  const auto text = DescribeFaultInstruction(info, L"", false);
  assert(text.find(L"0x1F2E3D4C5B60 주소의 명령을 실행하려 했지만") != std::wstring::npos);
  assert(text.find(L"호출한 곳은 SkyrimSE.exe+0x1234(call qword ptr [rax+0x18])입니다.") != std::wstring::npos);
  assert(FaultInstructionReportText(info) == "execute 0x1F2E3D4C5B60, from call qword ptr [rax+0x18] -> bad_jump");
}

// The 2026-10-26 forced close: PrismaUI.dll's present hook called a hook
// trampoline below SkyrimSE.exe whose page had been freed (MEM_FREE in the
// dump's memory-info stream). The probe sets target_state; the decoder words
// it. Committed data memory says so too; any other state keeps the wording.
void TestJumpTargetState()
{
  FaultRecord fault{};
  fault.access_kind = kAccessViolationExecute;
  fault.exception_address = 0x7FF7055F0000ull;
  fault.address = 0x7FF7055F0000ull;
  const auto plain = ExplainFaultInstruction(fault, nullptr, 0);
  assert(plain.verdict == "bad_jump" && plain.target_state.empty());

  auto freed = plain;
  freed.target_state = "freed";
  freed.call_site = L"PrismaUI.dll+0x59ce2";
  freed.call_instruction = "call qword ptr [rsp+0x20]";
  assert(DescribeFaultInstruction(freed, L"", true) ==
         L"The code tried to run instructions at 0x7FF7055F0000, but no memory was allocated there: it jumped into "
         L"memory that had been freed (for example a hook stub that was removed). The call was made at "
         L"PrismaUI.dll+0x59ce2 (call qword ptr [rsp+0x20]).");
  assert(DescribeFaultInstruction(freed, L"", false) ==
         L"코드가 0x7FF7055F0000 주소의 명령을 실행하려 했지만 그곳에는 할당된 메모리가 없었습니다. 해제된 메모리(예: "
         L"제거된 후킹 코드)로 이동했다는 뜻입니다. 호출한 곳은 PrismaUI.dll+0x59ce2(call qword ptr [rsp+0x20])입니다.");
  assert(DescribeFaultInstructionPlain(freed, true) ==
         L"It crashed jumping into memory that had been freed (for example a hook stub that was removed).");
  assert(DescribeFaultInstructionPlain(freed, false) == L"해제된 메모리(예: 제거된 후킹 코드)로 이동하다가 튕겼습니다.");
  assert(FaultInstructionReportText(freed) ==
         "execute 0x7FF7055F0000 (freed), from call qword ptr [rsp+0x20] -> bad_jump");

  auto data = plain;
  data.target_state = "not_executable";
  assert(DescribeFaultInstruction(data, L"", true).find(L"which is memory holding data, not code (it is not "
                                                        L"executable)") != std::wstring::npos);
  assert(DescribeFaultInstruction(data, L"", false).find(L"실행할 수 없는 데이터 메모리입니다") != std::wstring::npos);
  assert(DescribeFaultInstructionPlain(data, true) ==
         L"It crashed jumping to data instead of code (a broken function pointer).");
  assert(FaultInstructionReportText(data) == "execute 0x7FF7055F0000 (not_executable) -> bad_jump");

  // Reserved or executable memory, or no memory-info stream: today's wording.
  for (const char* state : { "", "reserved", "executable" }) {
    auto other = plain;
    other.target_state = state;
    assert(DescribeFaultInstruction(other, L"", true) == DescribeFaultInstruction(plain, L"", true));
    assert(DescribeFaultInstructionPlain(other, false) == DescribeFaultInstructionPlain(plain, false));
  }
  // A null_jump is never called freed: the first 64 KB is never allocated.
  auto nullJump = plain;
  nullJump.verdict = "null_jump";
  nullJump.target_state = "freed";
  assert(DescribeFaultInstruction(nullJump, L"", true).find(L"freed") == std::wstring::npos);
}

void TestFindCallTo()
{
  constexpr std::uint64_t kRet = 0x7FF700001000ull;
  constexpr std::uint64_t kTarget = 0x1F2E3D4C5B60ull;
  constexpr std::uint64_t kSlot = 0x20000000018ull;
  X64Registers regs{};
  regs[kRax] = kSlot - 0x18u;
  regs[kRsp] = 0x5000;
  const auto read = [](std::uint64_t address) -> std::optional<std::uint64_t> {
    if (address == kSlot || address == 0x5010u) {
      return kTarget;
    }
    return std::nullopt;
  };
  // Filler, then the call that ends at the return address.
  const auto window = [](std::vector<std::uint8_t> call) {
    std::vector<std::uint8_t> bytes(15u - call.size(), 0xCC);
    bytes.insert(bytes.end(), call.begin(), call.end());
    return bytes;
  };
  std::size_t length = 0;
  auto bytes = window({ 0xFF, 0x50, 0x18 });
  auto call = FindCallTo(bytes.data(), bytes.size(), kRet, kTarget, regs, read, &length);
  assert(call && length == 3u && FormatX64Call(*call, kRet) == "call qword ptr [rax+0x18]");

  // [rsp+8] at the call is [rsp+0x10] at the fault: the call pushed 8 bytes.
  bytes = window({ 0xFF, 0x54, 0x24, 0x08 });
  call = FindCallTo(bytes.data(), bytes.size(), kRet, kTarget, regs, read, &length);
  assert(call && length == 4u);

  regs[kRdx] = kTarget;
  bytes = window({ 0xFF, 0xD2 });
  call = FindCallTo(bytes.data(), bytes.size(), kRet, kTarget, regs, read, &length);
  assert(call && length == 2u && FormatX64Call(*call, kRet) == "call rdx");

  bytes = window({ 0xE8, 0x00, 0x00, 0x00, 0x00 });
  const std::int32_t rel = 0x100;
  std::memcpy(bytes.data() + bytes.size() - 4u, &rel, sizeof(rel));
  call = FindCallTo(bytes.data(), bytes.size(), kRet, kRet + 0x100u, regs, read, &length);
  assert(call && length == 5u && call->call_is_relative);

  // A call to somewhere else, or no call at all.
  bytes = window({ 0xFF, 0xD2 });
  assert(!FindCallTo(bytes.data(), bytes.size(), kRet, kTarget + 1u, regs, read, &length));
  bytes = window({ 0x48, 0x8B, 0x41, 0x24 });
  assert(!FindCallTo(bytes.data(), bytes.size(), kRet, kTarget, regs, read, &length));
  assert(!FindCallTo(nullptr, 0, kRet, kTarget, regs, read, &length));

  // 41 FF D0 is call r8; its tail FF D0 reads as call rax. With only r8 going
  // there it is call r8; with both, the bytes do not say which call it was.
  auto both = regs;
  both[kR8] = kTarget;
  both[kRax] = 0x1234;
  bytes = window({ 0x41, 0xFF, 0xD0 });
  call = FindCallTo(bytes.data(), bytes.size(), kRet, kTarget, both, read, &length);
  assert(call && length == 3u && FormatX64Call(*call, kRet) == "call r8");
  both[kRax] = kTarget;
  assert(!FindCallTo(bytes.data(), bytes.size(), kRet, kTarget, both, read, &length));

  // A redundant REX before a rel32 call reads the same: the shorter call.
  bytes = window({ 0x48, 0xE8, 0x00, 0x00, 0x00, 0x00 });
  std::memcpy(bytes.data() + bytes.size() - 4u, &rel, sizeof(rel));
  call = FindCallTo(bytes.data(), bytes.size(), kRet, kRet + 0x100u, regs, read, &length);
  assert(call && length == 5u && call->call_is_relative);
}

}  // namespace

int main()
{
  TestIntegerMovesAndLoads();
  TestCompareTestAndArithmetic();
  TestSse();
  TestAddressing();
  TestBranchesAndStack();
  TestNoAccessAndRejections();
  TestCanonical();
  TestNullBaseSentence();
  TestOtherVerdicts();
  TestAddressOnly();
  TestNothingToSay();
  TestExecute();
  TestJumpTargetState();
  TestFindCallTo();
  return 0;
}
