#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace skydiag::dump_tool {

// ADR-0018: what the instruction at an access violation was doing, in plain
// words ("it read 8 bytes from [rcx+0x24] and rcx was 0").
//
// A small x64 decoder for the instruction forms game and plugin code faults
// on: integer moves, loads and stores (mov, movzx, movsx, movsxd), compares,
// tests and arithmetic with a memory operand, SSE moves and scalar or packed
// math (movss, movsd, movups, movaps, movdqu, addss, comiss, ...), call/jmp
// through memory, push and pop. Legacy prefixes, REX, ModRM, SIB, disp8,
// disp32 and RIP-relative operands are decoded; VEX/EVEX, 0F 38/0F 3A,
// 32-bit addressing (67) and FS/GS operands are not. An instruction it does
// not know decodes to nothing: nothing is guessed.

// The longest x64 instruction.
inline constexpr std::size_t kMaxX64InstructionBytes = 15;

// EXCEPTION_ACCESS_VIOLATION ExceptionInformation[0].
inline constexpr std::uint64_t kAccessViolationRead = 0;
inline constexpr std::uint64_t kAccessViolationWrite = 1;
inline constexpr std::uint64_t kAccessViolationExecute = 8;
// ExceptionInformation[1] when the CPU raised a general-protection fault (a
// non-canonical address, a misaligned SSE operand), not a page fault.
inline constexpr std::uint64_t kAccessViolationNoAddress = ~0ull;
// Windows never maps the first 64 KB: an address below it is a null pointer
// plus an offset.
inline constexpr std::uint64_t kNullPageEnd = 0x10000;

enum class X64MemoryAccess : std::uint8_t
{
  kNone,
  kRead,
  kWrite,
  kReadWrite,
};

// [base + index*scale + displacement], or [rip + displacement].
struct X64MemoryOperand
{
  bool rip_relative = false;
  int base = -1;   // general-purpose register 0..15 (rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8..r15); -1 none
  int index = -1;
  std::uint8_t scale = 1;
  std::int64_t displacement = 0;
};

struct X64Instruction
{
  std::size_t length = 0;
  std::string mnemonic;  // "mov", "movzx", "cmp", "movups", "call", ...
  // The instruction reads or writes memory through `memory`. False for lea,
  // nop, prefetch and register-only forms.
  bool has_memory = false;
  X64MemoryOperand memory;
  X64MemoryAccess access = X64MemoryAccess::kNone;
  std::uint8_t access_size = 0;  // bytes the memory operand covers
  // Legacy SSE that faults on an operand not aligned to 16 bytes (movaps,
  // movdqa, packed math with a memory operand).
  bool requires_alignment = false;
  // call / jmp through memory: the value read there is where the CPU goes next.
  bool branch_through_memory = false;
  // push / pop of a register: the memory is the stack slot ([rsp-8] / [rsp]).
  bool stack_slot = false;
  // Any call (rel32, register or memory); the target of the first two.
  bool is_call = false;
  int call_register = -1;
  std::int64_t call_relative = 0;
  bool call_is_relative = false;
};

// Decodes the instruction at code[0]; nullopt when its form is not one of
// those above or the bytes end before it does.
std::optional<X64Instruction> DecodeX64Instruction(const std::uint8_t* code, std::size_t size);

// "rax" .. "r15"; empty when reg is out of range.
std::string X64RegisterName(int reg);

// "qword ptr [rcx+0x24]", "byte ptr [r8+r9*8-0x10]", "dword ptr [rip+0x1a2b]".
std::string FormatX64MemoryOperand(const X64Instruction& insn);

// "call qword ptr [rax+0x18]", "call rax", "call 0x7FF71FB5F000" (a rel32
// call's target, from the return address after it).
std::string FormatX64Call(const X64Instruction& call, std::uint64_t returnAddress);

// The general-purpose registers of a thread context, in register-number order.
using X64Registers = std::array<std::uint64_t, 16>;

// The address the memory operand covers first, from the registers at the
// instruction; nullopt without a memory operand. instructionAddress is where
// the instruction starts (RIP-relative operands count from its end).
std::optional<std::uint64_t> X64EffectiveAddress(
  const X64Instruction& insn,
  std::uint64_t instructionAddress,
  const X64Registers& registers);

// The 48-bit canonical form x64 requires of every address.
bool IsCanonicalX64Address(std::uint64_t address);

// What ExplainFaultInstruction reads from the dump.
struct FaultRecord
{
  std::uint64_t exception_address = 0;  // MINIDUMP_EXCEPTION.ExceptionAddress
  std::uint64_t access_kind = 0;        // ExceptionInformation[0]
  std::uint64_t address = 0;            // ExceptionInformation[1]
  // The faulting thread's context from the exception stream, when it holds
  // the exception address as its RIP.
  bool has_registers = false;
  X64Registers registers{};
};

// What the faulting instruction of an access violation did (ADR-0018).
struct FaultInstructionInfo
{
  // verdict values:
  //   null_pointer   the operand's base register held 0 or an address below 64 KB
  //   bad_pointer    the base register held an address with no (readable/writable) memory
  //   bad_index      the address was a base plus an index, and the index was large
  //   noncanonical   the address cannot exist (a pointer overwritten with other data)
  //   misaligned     movaps-like SSE on an address not aligned to 16 bytes
  //   fixed_address  an absolute or RIP-relative operand (no register)
  //   stack          push / pop on a broken stack pointer
  //   address_only   decoded, but the registers do not give the faulting address:
  //                  only the read/write and the address are stated
  //   null_jump      the CPU was sent to run code below 64 KB (a null function pointer)
  //   bad_jump       the CPU was sent to run code where there is none
  // empty: nothing to say (no code bytes, an unknown instruction, not an AV).
  std::string verdict;
  std::string access;          // read / write / execute (the exception record's)
  std::uint64_t address = 0;   // the exception record's faulting address
  // The decoded instruction (empty for null_jump / bad_jump).
  std::string mnemonic;
  std::string operand;         // FormatX64MemoryOperand
  std::string bytes;           // the instruction's own bytes, "48 8B 41 24"
  std::uint8_t access_size = 0;
  bool branch_through_memory = false;
  // The operand's address from the registers equals the faulting address
  // (or, for a general-protection fault, explains it).
  bool address_matches = false;
  std::uint64_t effective_address = 0;
  std::string base_register;   // only with address_matches
  std::uint64_t base_value = 0;
  std::string index_register;
  std::uint64_t index_value = 0;
  std::uint8_t scale = 1;
  std::int64_t displacement = 0;
  // null_jump / bad_jump: the call that went there ("SkyrimSE.exe+0x1234")
  // and its instruction ("call qword ptr [rax+0x18]"); empty when the return
  // address on the stack does not show one.
  std::wstring call_site;
  std::string call_instruction;
};

// Reads read/write AVs from the code bytes at the exception address (code,
// codeSize; codeSize may be shorter than an instruction when the dump holds
// fewer). Execute AVs (and fetches that faulted on the exception address
// itself) need no code bytes. Returns an empty verdict when there is nothing
// to say.
FaultInstructionInfo ExplainFaultInstruction(const FaultRecord& fault, const std::uint8_t* code, std::size_t codeSize);

// For null_jump / bad_jump: the call instruction that ends at returnAddress
// and whose target, from the registers at the fault, is target. before holds
// the bytes just below returnAddress (before[size-1] is at returnAddress-1).
// readQword reads 8 bytes of the dump (for call [mem]); rsp is the value at
// the fault (the call pushed the return address there). Sets *outLength to
// the call's length.
std::optional<X64Instruction> FindCallTo(
  const std::uint8_t* before,
  std::size_t size,
  std::uint64_t returnAddress,
  std::uint64_t target,
  const X64Registers& registers,
  const std::function<std::optional<std::uint64_t>(std::uint64_t)>& readQword,
  std::size_t* outLength);

// The evidence sentence; location is the fault's Module+Offset (may be empty).
std::wstring DescribeFaultInstruction(const FaultInstructionInfo& info, std::wstring_view location, bool en);

// The same in everyday words for the quick answer; empty for the verdicts
// that need the detail to make sense.
std::wstring DescribeFaultInstructionPlain(const FaultInstructionInfo& info, bool en);

// The report line: "mov qword ptr [rcx+0x24] (48 8B 41 24); rcx=0x0".
std::string FaultInstructionReportText(const FaultInstructionInfo& info);

}  // namespace skydiag::dump_tool
