#include "FaultInstruction.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace skydiag::dump_tool {
namespace {

constexpr std::uint8_t kRexW = 0x8u;
constexpr std::uint8_t kRexR = 0x4u;
constexpr std::uint8_t kRexX = 0x2u;
constexpr std::uint8_t kRexB = 0x1u;
constexpr int kRsp = 4;

// The SSE prefix that selects the instruction (none, 66, F3, F2).
enum class SsePrefix : std::uint8_t
{
  kNone,
  k66,
  kF3,
  kF2,
};

struct Prefixes
{
  bool operand_size = false;  // 66
  bool rep = false;           // F3
  bool repne = false;         // F2
  std::uint8_t rex = 0;
  std::size_t length = 0;     // bytes before the opcode
};

// Legacy prefixes, then REX. nullopt for the ones the decoder does not
// handle: 67 (32-bit addressing) and FS/GS (the segment base is unknown).
std::optional<Prefixes> ReadPrefixes(const std::uint8_t* code, std::size_t size)
{
  Prefixes p{};
  std::size_t i = 0;
  for (; i < size; ++i) {
    const std::uint8_t b = code[i];
    if (b == 0x66u) {
      p.operand_size = true;
    } else if (b == 0xF3u) {
      p.rep = true;
    } else if (b == 0xF2u) {
      p.repne = true;
    } else if (b == 0xF0u || b == 0x2Eu || b == 0x3Eu || b == 0x26u || b == 0x36u) {
      // lock; segment overrides x64 ignores
    } else if (b == 0x67u || b == 0x64u || b == 0x65u) {
      return std::nullopt;
    } else {
      break;
    }
  }
  if (p.rep && p.repne) {
    return std::nullopt;
  }
  if (i < size && (code[i] & 0xF0u) == 0x40u) {
    p.rex = code[i];
    ++i;
  }
  if (i >= size) {
    return std::nullopt;
  }
  p.length = i;
  return p;
}

SsePrefix SelectSsePrefix(const Prefixes& p)
{
  if (p.rep) {
    return SsePrefix::kF3;
  }
  if (p.repne) {
    return SsePrefix::kF2;
  }
  return p.operand_size ? SsePrefix::k66 : SsePrefix::kNone;
}

struct ModRm
{
  int reg = 0;  // ModRM.reg (with REX.R): a register operand or a group's opcode extension
  bool is_memory = false;
  int rm_register = -1;  // mod == 3
  X64MemoryOperand memory;
  std::size_t length = 0;  // ModRM, SIB and displacement bytes
};

std::optional<ModRm> ReadModRm(const std::uint8_t* code, std::size_t size, std::size_t at, std::uint8_t rex)
{
  if (at >= size) {
    return std::nullopt;
  }
  ModRm m{};
  const std::uint8_t modrm = code[at];
  const unsigned mod = modrm >> 6u;
  const unsigned rm = modrm & 7u;
  m.reg = static_cast<int>(((modrm >> 3u) & 7u) | ((rex & kRexR) != 0u ? 8u : 0u));
  if (mod == 3u) {
    m.rm_register = static_cast<int>(rm | ((rex & kRexB) != 0u ? 8u : 0u));
    m.length = 1;
    return m;
  }
  m.is_memory = true;
  auto& mem = m.memory;
  std::size_t i = at + 1u;
  bool disp32 = mod == 2u;
  if (rm == 4u) {
    if (i >= size) {
      return std::nullopt;
    }
    const std::uint8_t sib = code[i++];
    const unsigned index = ((sib >> 3u) & 7u) | ((rex & kRexX) != 0u ? 8u : 0u);
    const unsigned base = sib & 7u;
    if (index != 4u) {  // rsp cannot be an index; r12 can
      mem.index = static_cast<int>(index);
      mem.scale = static_cast<std::uint8_t>(1u << (sib >> 6u));
    }
    if (base == 5u && mod == 0u) {
      disp32 = true;  // no base
    } else {
      mem.base = static_cast<int>(base | ((rex & kRexB) != 0u ? 8u : 0u));
    }
  } else if (rm == 5u && mod == 0u) {
    mem.rip_relative = true;
    disp32 = true;
  } else {
    mem.base = static_cast<int>(rm | ((rex & kRexB) != 0u ? 8u : 0u));
  }
  if (mod == 1u) {
    if (i >= size) {
      return std::nullopt;
    }
    mem.displacement = static_cast<std::int64_t>(code[i] ^ 0x80u) - 0x80;  // sign-extended disp8
    i += 1u;
  } else if (disp32) {
    if (i + 4u > size) {
      return std::nullopt;
    }
    std::int32_t disp = 0;
    std::memcpy(&disp, code + i, sizeof(disp));
    mem.displacement = disp;
    i += 4u;
  }
  m.length = i - at;
  return m;
}

// One instruction form with a ModRM operand.
struct Form
{
  const char* mnemonic = nullptr;
  X64MemoryAccess access = X64MemoryAccess::kNone;
  std::uint8_t size = 0;
  std::size_t immediate = 0;
  bool aligned = false;
  bool branch = false;
  bool is_call = false;
  // lea, nop, prefetch: a memory operand that is never accessed.
  bool no_access = false;
  // lea: the register form does not exist.
  bool memory_only = false;
};

std::optional<X64Instruction> Finish(
  const std::uint8_t* code,
  std::size_t size,
  std::size_t modrmAt,
  std::uint8_t rex,
  const Form& form)
{
  if (!form.mnemonic) {
    return std::nullopt;
  }
  const auto m = ReadModRm(code, size, modrmAt, rex);
  if (!m || (form.memory_only && !m->is_memory)) {
    return std::nullopt;
  }
  const std::size_t length = modrmAt + m->length + form.immediate;
  if (length > size) {
    return std::nullopt;
  }
  X64Instruction insn{};
  insn.length = length;
  insn.mnemonic = form.mnemonic;
  if (form.is_call) {
    insn.is_call = true;
    insn.call_register = m->is_memory ? -1 : m->rm_register;
  }
  if (m->is_memory && !form.no_access) {
    insn.has_memory = true;
    insn.memory = m->memory;
    insn.access = form.access;
    insn.access_size = form.size;
    insn.requires_alignment = form.aligned;
    insn.branch_through_memory = form.branch;
  }
  return insn;
}

// The ModRM.reg field of the byte at code[at], before REX (the opcode
// extension of a group).
int GroupOf(const std::uint8_t* code, std::size_t size, std::size_t at)
{
  return at < size ? static_cast<int>((code[at] >> 3u) & 7u) : -1;
}

// An integer operand: 8 bytes with REX.W, else 2 with 66, else 4.
std::uint8_t OperandSize(const Prefixes& p)
{
  if ((p.rex & kRexW) != 0u) {
    return 8;
  }
  return p.operand_size ? 2 : 4;
}

constexpr std::array<const char*, 8> kAlu = { "add", "or", "adc", "sbb", "and", "sub", "xor", "cmp" };
constexpr std::array<const char*, 8> kShift = { "rol", "ror", "rcl", "rcr", "shl", "shr", "sal", "sar" };
constexpr std::array<const char*, 8> kGroup3 = { "test", "test", "not", "neg", "mul", "imul", "div", "idiv" };
constexpr std::array<const char*, 16> kCmov = { "cmovo", "cmovno", "cmovb", "cmovae", "cmove", "cmovne", "cmovbe",
  "cmova", "cmovs", "cmovns", "cmovp", "cmovnp", "cmovl", "cmovge", "cmovle", "cmovg" };
constexpr std::array<const char*, 16> kSet = { "seto", "setno", "setb", "setae", "sete", "setne", "setbe", "seta",
  "sets", "setns", "setp", "setnp", "setl", "setge", "setle", "setg" };

// A one-byte opcode (code[at] is the byte after it).
std::optional<X64Instruction> DecodeOneByte(
  const std::uint8_t* code,
  std::size_t size,
  std::size_t at,
  std::uint8_t op,
  const Prefixes& p)
{
  const std::uint8_t opSize = OperandSize(p);
  const std::uint8_t byteSize = 1;
  const std::size_t immZ = opSize == 2u ? 2u : 4u;
  if (p.rep || p.repne) {
    return std::nullopt;  // rep / xacquire forms are not decoded
  }
  Form f{};
  if (op < 0x40u && (op & 7u) < 4u) {
    const unsigned operation = op >> 3u;
    const bool toRegister = (op & 2u) != 0u;
    f.mnemonic = kAlu[operation];
    f.size = (op & 1u) != 0u ? opSize : byteSize;
    f.access = (toRegister || operation == 7u) ? X64MemoryAccess::kRead : X64MemoryAccess::kReadWrite;
    return Finish(code, size, at, p.rex, f);
  }
  if (op >= 0x50u && op <= 0x5Fu) {
    if (p.operand_size) {
      return std::nullopt;
    }
    const bool push = op < 0x58u;
    X64Instruction insn{};
    insn.length = at;
    insn.mnemonic = push ? "push" : "pop";
    insn.has_memory = true;
    insn.stack_slot = true;
    insn.memory.base = kRsp;
    insn.memory.displacement = push ? -8 : 0;
    insn.access = push ? X64MemoryAccess::kWrite : X64MemoryAccess::kRead;
    insn.access_size = 8;
    return insn;
  }
  switch (op) {
    case 0x63u:
      f = Form{ "movsxd", X64MemoryAccess::kRead, 4 };
      break;
    case 0x69u:
    case 0x6Bu:
      f = Form{ "imul", X64MemoryAccess::kRead, opSize, op == 0x69u ? immZ : 1u };
      break;
    case 0x80u:
    case 0x81u:
    case 0x83u: {
      const int g = GroupOf(code, size, at);
      if (g < 0) {
        return std::nullopt;
      }
      f.mnemonic = kAlu[static_cast<std::size_t>(g)];
      f.access = g == 7 ? X64MemoryAccess::kRead : X64MemoryAccess::kReadWrite;
      f.size = op == 0x80u ? byteSize : opSize;
      f.immediate = op == 0x81u ? immZ : 1u;
      break;
    }
    case 0x84u:
    case 0x85u:
      f = Form{ "test", X64MemoryAccess::kRead, op == 0x84u ? std::uint8_t{ 1 } : opSize };
      break;
    case 0x86u:
    case 0x87u:
      f = Form{ "xchg", X64MemoryAccess::kReadWrite, op == 0x86u ? std::uint8_t{ 1 } : opSize };
      break;
    case 0x88u:
    case 0x89u:
      f = Form{ "mov", X64MemoryAccess::kWrite, op == 0x88u ? std::uint8_t{ 1 } : opSize };
      break;
    case 0x8Au:
    case 0x8Bu:
      f = Form{ "mov", X64MemoryAccess::kRead, op == 0x8Au ? std::uint8_t{ 1 } : opSize };
      break;
    case 0x8Du:
      f.mnemonic = "lea";
      f.no_access = true;
      f.memory_only = true;
      break;
    case 0x8Fu:
      if (GroupOf(code, size, at) != 0) {
        return std::nullopt;
      }
      f = Form{ "pop", X64MemoryAccess::kWrite, p.operand_size ? std::uint8_t{ 2 } : std::uint8_t{ 8 } };
      break;
    case 0xC0u:
    case 0xC1u:
    case 0xD0u:
    case 0xD1u:
    case 0xD2u:
    case 0xD3u: {
      const int g = GroupOf(code, size, at);
      if (g < 0) {
        return std::nullopt;
      }
      f.mnemonic = kShift[static_cast<std::size_t>(g)];
      f.access = X64MemoryAccess::kReadWrite;
      f.size = (op & 1u) != 0u ? opSize : byteSize;
      f.immediate = op < 0xD0u ? 1u : 0u;
      break;
    }
    case 0xC6u:
    case 0xC7u:
      if (GroupOf(code, size, at) != 0) {
        return std::nullopt;
      }
      f = Form{ "mov", X64MemoryAccess::kWrite, op == 0xC6u ? std::uint8_t{ 1 } : opSize, op == 0xC6u ? 1u : immZ };
      break;
    case 0xF6u:
    case 0xF7u: {
      const int g = GroupOf(code, size, at);
      if (g < 0) {
        return std::nullopt;
      }
      f.mnemonic = kGroup3[static_cast<std::size_t>(g)];
      f.access = (g == 2 || g == 3) ? X64MemoryAccess::kReadWrite : X64MemoryAccess::kRead;
      f.size = op == 0xF6u ? byteSize : opSize;
      if (g < 2) {
        f.immediate = op == 0xF6u ? 1u : immZ;
      }
      break;
    }
    case 0xFEu:
    case 0xFFu: {
      const int g = GroupOf(code, size, at);
      if (g == 0 || g == 1) {
        f = Form{ g == 0 ? "inc" : "dec", X64MemoryAccess::kReadWrite, op == 0xFEu ? std::uint8_t{ 1 } : opSize };
      } else if (op == 0xFFu && (g == 2 || g == 4)) {
        f = Form{ g == 2 ? "call" : "jmp", X64MemoryAccess::kRead, 8 };
        f.branch = true;
        f.is_call = g == 2;
      } else if (op == 0xFFu && g == 6) {
        f = Form{ "push", X64MemoryAccess::kRead, p.operand_size ? std::uint8_t{ 2 } : std::uint8_t{ 8 } };
      } else {
        return std::nullopt;
      }
      break;
    }
    case 0xE8u: {
      if (p.operand_size || at + 4u > size) {
        return std::nullopt;
      }
      std::int32_t rel = 0;
      std::memcpy(&rel, code + at, sizeof(rel));
      X64Instruction insn{};
      insn.length = at + 4u;
      insn.mnemonic = "call";
      insn.is_call = true;
      insn.call_is_relative = true;
      insn.call_relative = rel;
      return insn;
    }
    default:
      return std::nullopt;
  }
  return Finish(code, size, at, p.rex, f);
}

// An SSE opcode's four forms by prefix (none, 66, F3, F2); a null mnemonic
// is a form that does not exist or is MMX.
struct SseForms
{
  std::array<Form, 4> by_prefix;
};

Form SseForm(const char* mnemonic, X64MemoryAccess access, std::uint8_t size, bool aligned = false, std::size_t imm = 0)
{
  Form f{ mnemonic, access, size, imm };
  f.aligned = aligned;
  return f;
}

// Packed (aligned, 16 bytes) and scalar math: sqrt, add, mul, sub, min, div,
// max, cmp.
SseForms SseMath(const char* ps, const char* pd, const char* ss, const char* sd, std::size_t imm = 0)
{
  const auto r = X64MemoryAccess::kRead;
  return SseForms{ { SseForm(ps, r, 16, true, imm), SseForm(pd, r, 16, true, imm), SseForm(ss, r, 4, false, imm),
    SseForm(sd, r, 8, false, imm) } };
}

std::optional<SseForms> SseFormsFor(std::uint8_t op, bool rexW)
{
  const auto r = X64MemoryAccess::kRead;
  const auto w = X64MemoryAccess::kWrite;
  const Form none{};
  const std::uint8_t gpr = rexW ? std::uint8_t{ 8 } : std::uint8_t{ 4 };
  switch (op) {
    case 0x10u:
      return SseForms{ { SseForm("movups", r, 16), SseForm("movupd", r, 16), SseForm("movss", r, 4), SseForm("movsd", r, 8) } };
    case 0x11u:
      return SseForms{ { SseForm("movups", w, 16), SseForm("movupd", w, 16), SseForm("movss", w, 4), SseForm("movsd", w, 8) } };
    case 0x12u:
      return SseForms{ { SseForm("movlps", r, 8), SseForm("movlpd", r, 8), none, none } };
    case 0x13u:
      return SseForms{ { SseForm("movlps", w, 8), SseForm("movlpd", w, 8), none, none } };
    case 0x14u:
      return SseForms{ { SseForm("unpcklps", r, 16, true), SseForm("unpcklpd", r, 16, true), none, none } };
    case 0x15u:
      return SseForms{ { SseForm("unpckhps", r, 16, true), SseForm("unpckhpd", r, 16, true), none, none } };
    case 0x16u:
      return SseForms{ { SseForm("movhps", r, 8), SseForm("movhpd", r, 8), none, none } };
    case 0x17u:
      return SseForms{ { SseForm("movhps", w, 8), SseForm("movhpd", w, 8), none, none } };
    case 0x28u:
      return SseForms{ { SseForm("movaps", r, 16, true), SseForm("movapd", r, 16, true), none, none } };
    case 0x29u:
      return SseForms{ { SseForm("movaps", w, 16, true), SseForm("movapd", w, 16, true), none, none } };
    case 0x2Au:
      return SseForms{ { none, none, SseForm("cvtsi2ss", r, gpr), SseForm("cvtsi2sd", r, gpr) } };
    case 0x2Cu:
      return SseForms{ { none, none, SseForm("cvttss2si", r, 4), SseForm("cvttsd2si", r, 8) } };
    case 0x2Du:
      return SseForms{ { none, none, SseForm("cvtss2si", r, 4), SseForm("cvtsd2si", r, 8) } };
    case 0x2Eu:
      return SseForms{ { SseForm("ucomiss", r, 4), SseForm("ucomisd", r, 8), none, none } };
    case 0x2Fu:
      return SseForms{ { SseForm("comiss", r, 4), SseForm("comisd", r, 8), none, none } };
    case 0x51u:
      return SseMath("sqrtps", "sqrtpd", "sqrtss", "sqrtsd");
    case 0x54u:
      return SseForms{ { SseForm("andps", r, 16, true), SseForm("andpd", r, 16, true), none, none } };
    case 0x55u:
      return SseForms{ { SseForm("andnps", r, 16, true), SseForm("andnpd", r, 16, true), none, none } };
    case 0x56u:
      return SseForms{ { SseForm("orps", r, 16, true), SseForm("orpd", r, 16, true), none, none } };
    case 0x57u:
      return SseForms{ { SseForm("xorps", r, 16, true), SseForm("xorpd", r, 16, true), none, none } };
    case 0x58u:
      return SseMath("addps", "addpd", "addss", "addsd");
    case 0x59u:
      return SseMath("mulps", "mulpd", "mulss", "mulsd");
    case 0x5Au:
      return SseForms{ { SseForm("cvtps2pd", r, 8), SseForm("cvtpd2ps", r, 16, true), SseForm("cvtss2sd", r, 4),
        SseForm("cvtsd2ss", r, 8) } };
    case 0x5Bu:
      return SseForms{ { SseForm("cvtdq2ps", r, 16, true), SseForm("cvtps2dq", r, 16, true),
        SseForm("cvttps2dq", r, 16, true), none } };
    case 0x5Cu:
      return SseMath("subps", "subpd", "subss", "subsd");
    case 0x5Du:
      return SseMath("minps", "minpd", "minss", "minsd");
    case 0x5Eu:
      return SseMath("divps", "divpd", "divss", "divsd");
    case 0x5Fu:
      return SseMath("maxps", "maxpd", "maxss", "maxsd");
    case 0x6Eu:
      return SseForms{ { none, SseForm(rexW ? "movq" : "movd", r, gpr), none, none } };
    case 0x6Fu:
      return SseForms{ { none, SseForm("movdqa", r, 16, true), SseForm("movdqu", r, 16), none } };
    case 0x7Eu:
      return SseForms{ { none, SseForm(rexW ? "movq" : "movd", w, gpr), SseForm("movq", r, 8), none } };
    case 0x7Fu:
      return SseForms{ { none, SseForm("movdqa", w, 16, true), SseForm("movdqu", w, 16), none } };
    case 0xC2u:
      return SseMath("cmpps", "cmppd", "cmpss", "cmpsd", 1);
    case 0xC6u:
      return SseForms{ { SseForm("shufps", r, 16, true, 1), SseForm("shufpd", r, 16, true, 1), none, none } };
    case 0xD6u:
      return SseForms{ { none, SseForm("movq", w, 8), none, none } };
    default:
      return std::nullopt;
  }
}

// A 0F opcode (code[at] is the byte after it).
std::optional<X64Instruction> DecodeTwoByte(
  const std::uint8_t* code,
  std::size_t size,
  std::size_t at,
  std::uint8_t op,
  const Prefixes& p)
{
  if (const auto sse = SseFormsFor(op, (p.rex & kRexW) != 0u)) {
    const auto& form = sse->by_prefix[static_cast<std::size_t>(SelectSsePrefix(p))];
    return Finish(code, size, at, p.rex, form);
  }
  if (p.rep || p.repne) {
    return std::nullopt;  // popcnt, tzcnt and the like are not decoded
  }
  const std::uint8_t opSize = OperandSize(p);
  Form f{};
  if (op >= 0x40u && op <= 0x4Fu) {
    f = Form{ kCmov[op - 0x40u], X64MemoryAccess::kRead, opSize };
  } else if (op >= 0x90u && op <= 0x9Fu) {
    f = Form{ kSet[op - 0x90u], X64MemoryAccess::kWrite, 1 };
  } else {
    switch (op) {
      case 0x0Du:
      case 0x18u:
        f.mnemonic = "prefetch";
        f.no_access = true;
        break;
      case 0x1Fu:
        f.mnemonic = "nop";
        f.no_access = true;
        break;
      case 0xAFu:
        f = Form{ "imul", X64MemoryAccess::kRead, opSize };
        break;
      case 0xB0u:
      case 0xB1u:
        f = Form{ "cmpxchg", X64MemoryAccess::kReadWrite, op == 0xB0u ? std::uint8_t{ 1 } : opSize };
        break;
      case 0xB6u:
      case 0xB7u:
        f = Form{ "movzx", X64MemoryAccess::kRead, op == 0xB6u ? std::uint8_t{ 1 } : std::uint8_t{ 2 } };
        break;
      case 0xBEu:
      case 0xBFu:
        f = Form{ "movsx", X64MemoryAccess::kRead, op == 0xBEu ? std::uint8_t{ 1 } : std::uint8_t{ 2 } };
        break;
      case 0xBAu: {
        const int g = GroupOf(code, size, at);
        constexpr std::array<const char*, 4> kBit = { "bt", "bts", "btr", "btc" };
        if (g < 4) {
          return std::nullopt;
        }
        f = Form{ kBit[static_cast<std::size_t>(g - 4)], g == 4 ? X64MemoryAccess::kRead : X64MemoryAccess::kReadWrite,
          opSize, 1 };
        break;
      }
      case 0xC0u:
      case 0xC1u:
        f = Form{ "xadd", X64MemoryAccess::kReadWrite, op == 0xC0u ? std::uint8_t{ 1 } : opSize };
        break;
      default:
        return std::nullopt;
    }
  }
  return Finish(code, size, at, p.rex, f);
}

std::string Hex(std::uint64_t value, bool upper = true)
{
  std::array<char, 24> buf{};
  std::snprintf(buf.data(), buf.size(), upper ? "0x%llX" : "0x%llx", static_cast<unsigned long long>(value));
  return buf.data();
}

// "[rax+rcx*8-0x10]", "[rip+0x1a2b]", "[0x1000]".
std::string BracketText(const X64MemoryOperand& m)
{
  std::string text = "[";
  if (m.rip_relative) {
    text += "rip";
  }
  if (m.base >= 0) {
    text += X64RegisterName(m.base);
  }
  if (m.index >= 0) {
    if (text.size() > 1u) {
      text += "+";
    }
    text += X64RegisterName(m.index);
    if (m.scale != 1u) {
      text += "*" + std::to_string(m.scale);
    }
  }
  if (m.displacement != 0 || text.size() == 1u) {
    const bool negative = m.displacement < 0;
    const auto magnitude = negative ? (0ull - static_cast<std::uint64_t>(m.displacement))
                                    : static_cast<std::uint64_t>(m.displacement);
    if (text.size() > 1u) {
      text += negative ? "-" : "+";
    }
    text += Hex(magnitude, /*upper=*/false);
  }
  return text + "]";
}

std::string BytesText(const std::uint8_t* code, std::size_t length)
{
  std::string text;
  std::array<char, 4> buf{};
  for (std::size_t i = 0; i < length; ++i) {
    std::snprintf(buf.data(), buf.size(), "%02X", static_cast<unsigned>(code[i]));
    text += (i == 0 ? "" : " ");
    text += buf.data();
  }
  return text;
}

bool Reads(X64MemoryAccess access)
{
  return access == X64MemoryAccess::kRead || access == X64MemoryAccess::kReadWrite;
}

bool Writes(X64MemoryAccess access)
{
  return access == X64MemoryAccess::kWrite || access == X64MemoryAccess::kReadWrite;
}

}  // namespace

std::optional<X64Instruction> DecodeX64Instruction(const std::uint8_t* code, std::size_t size)
{
  if (!code || size == 0u) {
    return std::nullopt;
  }
  size = std::min(size, kMaxX64InstructionBytes);
  const auto p = ReadPrefixes(code, size);
  if (!p) {
    return std::nullopt;
  }
  const std::size_t at = p->length + 1u;
  const std::uint8_t op = code[p->length];
  if (op != 0x0Fu) {
    return DecodeOneByte(code, size, at, op, *p);
  }
  if (at >= size) {
    return std::nullopt;
  }
  return DecodeTwoByte(code, size, at + 1u, code[at], *p);
}

std::string X64RegisterName(int reg)
{
  static constexpr std::array<const char*, 16> kNames = { "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8",
    "r9", "r10", "r11", "r12", "r13", "r14", "r15" };
  if (reg < 0 || reg >= static_cast<int>(kNames.size())) {
    return {};
  }
  return kNames[static_cast<std::size_t>(reg)];
}

std::string FormatX64MemoryOperand(const X64Instruction& insn)
{
  const char* width = nullptr;
  switch (insn.access_size) {
    case 1:
      width = "byte";
      break;
    case 2:
      width = "word";
      break;
    case 4:
      width = "dword";
      break;
    case 8:
      width = "qword";
      break;
    case 16:
      width = "xmmword";
      break;
    default:
      break;
  }
  const std::string bracket = BracketText(insn.memory);
  return width ? (std::string(width) + " ptr " + bracket) : bracket;
}

std::string FormatX64Call(const X64Instruction& call, std::uint64_t returnAddress)
{
  if (call.call_is_relative) {
    return "call " + Hex(returnAddress + static_cast<std::uint64_t>(call.call_relative));
  }
  if (call.call_register >= 0) {
    return "call " + X64RegisterName(call.call_register);
  }
  return "call " + FormatX64MemoryOperand(call);
}

std::optional<std::uint64_t> X64EffectiveAddress(
  const X64Instruction& insn,
  std::uint64_t instructionAddress,
  const X64Registers& registers)
{
  if (!insn.has_memory) {
    return std::nullopt;
  }
  const auto& m = insn.memory;
  std::uint64_t address = static_cast<std::uint64_t>(m.displacement);
  if (m.rip_relative) {
    address += instructionAddress + insn.length;
  }
  if (m.base >= 0) {
    address += registers[static_cast<std::size_t>(m.base)];
  }
  if (m.index >= 0) {
    address += registers[static_cast<std::size_t>(m.index)] * m.scale;
  }
  return address;
}

bool IsCanonicalX64Address(std::uint64_t address)
{
  const std::uint64_t top = address >> 47u;
  return top == 0u || top == 0x1FFFFu;
}

FaultInstructionInfo ExplainFaultInstruction(const FaultRecord& fault, const std::uint8_t* code, std::size_t codeSize)
{
  FaultInstructionInfo info{};
  const auto kind = fault.access_kind;
  if (kind != kAccessViolationRead && kind != kAccessViolationWrite && kind != kAccessViolationExecute) {
    return info;
  }
  info.address = fault.address;

  // Fetching the instruction itself faulted: the CPU was sent somewhere that
  // is not code. The exception record says so; no code bytes are needed.
  if (kind != kAccessViolationWrite && fault.address == fault.exception_address) {
    info.access = "execute";
    info.verdict = fault.address < kNullPageEnd ? "null_jump" : "bad_jump";
    return info;
  }
  if (kind == kAccessViolationExecute) {
    return FaultInstructionInfo{};
  }

  const auto insn = DecodeX64Instruction(code, codeSize);
  if (!insn || !insn->has_memory) {
    return FaultInstructionInfo{};  // unknown, or nothing it could have faulted on
  }
  const bool write = kind == kAccessViolationWrite;
  info.access = write ? "write" : "read";
  info.mnemonic = insn->mnemonic;
  info.operand = FormatX64MemoryOperand(*insn);
  info.bytes = BytesText(code, insn->length);
  info.access_size = insn->access_size;
  info.branch_through_memory = insn->branch_through_memory;
  info.displacement = insn->memory.displacement;
  info.verdict = "address_only";

  const auto unexplained = [&]() {
    // A general-protection fault has no address to state.
    return fault.address == kAccessViolationNoAddress ? FaultInstructionInfo{} : info;
  };
  if (!fault.has_registers || !(write ? Writes(insn->access) : Reads(insn->access))) {
    return unexplained();
  }
  const auto ea = X64EffectiveAddress(*insn, fault.exception_address, fault.registers);
  if (!ea) {
    return unexplained();
  }
  const std::uint64_t span = std::max<std::uint64_t>(insn->access_size, 1u);
  std::string verdict;
  if (fault.address == kAccessViolationNoAddress) {
    if (!IsCanonicalX64Address(*ea) || !IsCanonicalX64Address(*ea + span - 1u)) {
      verdict = "noncanonical";
    } else if (insn->requires_alignment && (*ea % 16u) != 0u) {
      verdict = "misaligned";
    } else {
      return FaultInstructionInfo{};
    }
  } else if (fault.address < *ea || fault.address - *ea >= span) {
    return unexplained();
  }

  info.address_matches = true;
  info.effective_address = *ea;
  const auto& m = insn->memory;
  if (m.base >= 0) {
    info.base_register = X64RegisterName(m.base);
    info.base_value = fault.registers[static_cast<std::size_t>(m.base)];
  }
  if (m.index >= 0) {
    info.index_register = X64RegisterName(m.index);
    info.index_value = fault.registers[static_cast<std::size_t>(m.index)];
    info.scale = m.scale;
  }
  if (verdict.empty()) {
    const std::uint64_t scaledIndex = m.index >= 0 ? info.index_value * m.scale : 0u;
    const bool bigIndex = m.index >= 0 && scaledIndex >= kNullPageEnd && (0ull - scaledIndex) >= kNullPageEnd;
    if (insn->stack_slot || m.base == kRsp) {
      verdict = "stack";
    } else if (m.base >= 0 && info.base_value < kNullPageEnd) {
      verdict = "null_pointer";
    } else if (bigIndex) {
      verdict = "bad_index";
    } else if (m.base >= 0) {
      verdict = "bad_pointer";
    } else {
      verdict = "fixed_address";
    }
  }
  info.verdict = std::move(verdict);
  return info;
}

std::optional<X64Instruction> FindCallTo(
  const std::uint8_t* before,
  std::size_t size,
  std::uint64_t returnAddress,
  std::uint64_t target,
  const X64Registers& registers,
  const std::function<std::optional<std::uint64_t>(std::uint64_t)>& readQword,
  std::size_t* outLength)
{
  if (!before) {
    return std::nullopt;
  }
  // The call pushed the return address: at the call, rsp was 8 higher.
  X64Registers atCall = registers;
  atCall[kRsp] += 8u;
  const std::size_t longest = std::min(size, kMaxX64InstructionBytes);
  for (std::size_t length = 2; length <= longest; ++length) {
    const std::uint8_t* start = before + (size - length);
    const auto insn = DecodeX64Instruction(start, length);
    if (!insn || !insn->is_call || insn->length != length) {
      continue;
    }
    std::optional<std::uint64_t> went;
    if (insn->call_is_relative) {
      went = returnAddress + static_cast<std::uint64_t>(insn->call_relative);
    } else if (insn->call_register >= 0) {
      went = atCall[static_cast<std::size_t>(insn->call_register)];
    } else if (const auto slot = X64EffectiveAddress(*insn, returnAddress - length, atCall); slot && readQword) {
      went = readQword(*slot);
    }
    if (went && *went == target) {
      if (outLength) {
        *outLength = length;
      }
      return insn;
    }
  }
  return std::nullopt;
}

namespace {

std::wstring Widen(std::string_view text)
{
  std::wstring out;
  out.reserve(text.size());
  for (const char c : text) {
    out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
  }
  return out;
}

std::wstring HexW(std::uint64_t value)
{
  return Widen(Hex(value));
}

// "[rax+0x10]" from "qword ptr [rax+0x10]".
std::wstring Bracket(const FaultInstructionInfo& info)
{
  const auto at = info.operand.find('[');
  return Widen(at == std::string::npos ? info.operand : info.operand.substr(at));
}

std::wstring Bytes(std::uint8_t size, bool en)
{
  if (en) {
    return std::to_wstring(size) + (size == 1u ? L" byte" : L" bytes");
  }
  return std::to_wstring(size) + L"바이트";
}

// "r8 (0x21509A50000) + r9 (0x689AC) × 8 + 0x10".
std::wstring AddressExpression(const FaultInstructionInfo& info)
{
  std::wstring text;
  if (!info.base_register.empty()) {
    text = Widen(info.base_register) + L" (" + HexW(info.base_value) + L")";
  }
  if (!info.index_register.empty()) {
    text += (text.empty() ? L"" : L" + ") + Widen(info.index_register) + L" (" + HexW(info.index_value) + L")";
    if (info.scale != 1u) {
      text += L" × " + std::to_wstring(info.scale);
    }
  }
  if (info.displacement != 0) {
    const bool negative = info.displacement < 0;
    const auto magnitude = negative ? (0ull - static_cast<std::uint64_t>(info.displacement))
                                    : static_cast<std::uint64_t>(info.displacement);
    text += (text.empty() ? (negative ? L"-" : L"") : (negative ? L" - " : L" + ")) + HexW(magnitude);
  }
  return text;
}

}  // namespace

std::wstring DescribeFaultInstruction(const FaultInstructionInfo& info, std::wstring_view location, bool en)
{
  if (info.verdict.empty()) {
    return {};
  }
  const std::wstring loc(location);
  const std::wstring address = HexW(info.address);
  const std::wstring callSite = info.call_site.empty()
    ? std::wstring{}
    : (en ? (L" The call was made at " + info.call_site + L" (" + Widen(info.call_instruction) + L").")
          : (L" 호출한 곳은 " + info.call_site + L"(" + Widen(info.call_instruction) + L")입니다."));
  if (info.verdict == "null_jump") {
    return (en ? (L"The code tried to run instructions at " + address +
                   L": it called a function through an empty (null) pointer, for example a function of an object that "
                   L"was already gone or never created.")
               : (L"코드가 " + address + L" 주소의 명령을 실행하려 했습니다. 비어 있는(null) 함수 포인터로 함수를 불렀다는 "
                   L"뜻입니다. 예를 들어 이미 사라졌거나 만들어진 적 없는 오브젝트의 함수입니다.")) +
      callSite;
  }
  if (info.verdict == "bad_jump") {
    return (en ? (L"The code tried to run instructions at " + address +
                   L", which is not code: it called or jumped through a function pointer or return address that was "
                   L"wrong (freed or corrupted).")
               : (L"코드가 " + address + L" 주소의 명령을 실행하려 했지만 그곳은 코드가 아닙니다. 잘못된(해제됐거나 "
                   L"망가진) 함수 포인터나 반환 주소로 호출하거나 이동했다는 뜻입니다.")) +
      callSite;
  }

  const bool write = info.access == "write";
  if (info.verdict == "address_only") {
    std::wstring text = en
      ? ((loc.empty() ? L"The code" : (L"The code at " + loc)) + (write ? L" tried to write" : L" tried to read") +
          L" memory at " + address + L" and could not.")
      : ((loc.empty() ? L"코드가 " : (loc + L"의 코드가 ")) + address + (write ? L" 주소에 메모리를 쓰려다" : L" 주소의 메모리를 읽으려다") +
          L" 실패했습니다.");
    if (info.address < kNullPageEnd) {
      text += en ? L" An address this close to 0 usually means a null pointer: an object that was already gone or never "
                   L"created."
                 : L" 0에 가까운 주소는 보통 null 포인터, 즉 이미 사라졌거나 만들어진 적 없는 오브젝트를 썼다는 뜻입니다.";
    }
    return text;
  }

  const std::wstring mnemonic = Widen(info.mnemonic);
  const std::wstring subject = en ? ((loc.empty() ? L"The instruction" : (L"The instruction at " + loc)) + L" (" + mnemonic + L")")
                                  : ((loc.empty() ? L"예외가 난 명령어" : (loc + L"의 명령어")) + L"(" + mnemonic + L")");
  const std::wstring bracket = Bracket(info);
  const std::wstring ea = HexW(info.effective_address);
  const std::wstring base = Widen(info.base_register);
  const std::wstring size = Bytes(info.access_size, en);
  const bool call = info.mnemonic == "call";

  if (info.verdict == "misaligned") {
    return en ? (subject + L" needs an address aligned to 16 bytes, but " + bracket + L" was " + ea +
                  L": the data was not aligned.")
              : (subject + L"는 16바이트 단위로 정렬된 주소가 필요한데 " + bracket + L"의 주소가 " + ea +
                  L"였습니다(정렬되지 않은 데이터).");
  }
  if (info.verdict == "stack") {
    return en ? (subject + L" tried to use the stack at " + ea + L" (rsp was " + HexW(info.base_value) +
                  L"), where there was no memory: the stack pointer was broken.")
              : (subject + L"가 스택 " + ea + L"(rsp " + HexW(info.base_value) +
                  L")를 쓰려 했지만 메모리가 없었습니다. 스택 포인터가 망가졌다는 뜻입니다.");
  }

  // What the instruction tried to do with the operand.
  std::wstring tried;
  if (info.branch_through_memory) {
    tried = en ? (L" tried to read the address " + std::wstring(call ? L"of the function to call" : L"to jump to") +
                   L" from " + bracket)
               : (L"가 " + bracket + (call ? L"에서 호출할 함수 주소를 읽으려 했는데" : L"에서 이동할 주소를 읽으려 했는데"));
  } else if (write) {
    tried = en ? (L" tried to write " + size + L" to " + bracket) : (L"가 " + bracket + L"에 " + size + L"를 쓰려 했는데");
  } else {
    tried = en ? (L" tried to read " + size + L" from " + bracket) : (L"가 " + bracket + L"에서 " + size + L"를 읽으려 했는데");
  }
  if (info.verdict == "fixed_address") {
    return en ? (subject + (write ? L" tried to write " + size + L" to" : L" tried to read " + size + L" from") +
                  L" the fixed address " + ea + (write ? L"; that memory could not be written." : L"; there was no readable memory there."))
              : (subject + L"가 고정 주소 " + ea + (write ? L"에 " + size + L"를 쓰려 했지만 쓸 수 있는 메모리가 없었습니다."
                                                             : L"에서 " + size + L"를 읽으려 했지만 그곳에 읽을 수 있는 메모리가 없었습니다."));
  }
  if (info.verdict == "null_pointer") {
    const bool zero = info.base_value == 0u;
    if (en) {
      return subject + tried + L", and " + base + (zero ? L" was 0" : (L" was " + HexW(info.base_value) + L", an address near 0")) +
        (call ? L": it called a function of an object that was already gone or never created (a null pointer)."
              : L": it used an object that was already gone or never created (a null pointer).");
    }
    return subject + tried + L" " + base + (zero ? L"가 0이었습니다." : (L"가 0에 가까운 " + HexW(info.base_value) + L"였습니다.")) +
      (call ? L" 이미 사라졌거나 만들어진 적 없는 오브젝트의 함수를 부르려 했다는 뜻입니다(null 포인터)."
            : L" 이미 사라졌거나 만들어진 적 없는 오브젝트를 썼다는 뜻입니다(null 포인터).");
  }
  if (info.verdict == "noncanonical") {
    const bool baseIsBad = !info.base_register.empty() && !IsCanonicalX64Address(info.base_value);
    if (en) {
      const std::wstring what = baseIsBad ? (base + L" was " + HexW(info.base_value))
                                          : (L"the address was " + ea + L" (" + AddressExpression(info) + L")");
      return subject + tried + L", and " + what +
        L", which cannot be a memory address: the pointer had been overwritten with other data (corrupted memory).";
    }
    const std::wstring what = baseIsBad ? (base + L"가 " + HexW(info.base_value))
                                        : (L"주소가 " + ea + L"(" + AddressExpression(info) + L")");
    return subject + tried + L" " + what +
      L"였습니다. 이 값은 메모리 주소가 될 수 없으므로 포인터 자리가 다른 데이터로 덮어써졌다는 뜻입니다(메모리 손상).";
  }
  const std::wstring noMemory = en
    ? (write ? (L"memory at " + ea + L" could not be written (read-only or not there)")
             : (L"there was no readable memory at " + ea))
    : (write ? (ea + L"에는 쓸 수 있는 메모리가 없었습니다(읽기 전용이거나 없는 메모리).")
             : (ea + L"에는 읽을 수 있는 메모리가 없었습니다."));
  if (info.verdict == "bad_index") {
    const std::wstring index = Widen(info.index_register);
    return en ? (subject + tried + L"; the address is " + AddressExpression(info) + L", and " + noMemory +
                  L": the index (" + index + L") was probably out of range, or the pointer was wrong.")
              : (subject + tried + L" 주소는 " + AddressExpression(info) + L"이고 " + noMemory +
                  L" 인덱스(" + index + L")가 범위를 벗어났거나 포인터가 잘못됐을 가능성이 큽니다.");
  }
  // bad_pointer
  return en ? (subject + tried + L", and " + base + L" was " + HexW(info.base_value) + L"; " + noMemory +
                L": the object was probably freed already, or the pointer is corrupted.")
            : (subject + tried + L" " + base + L"가 " + HexW(info.base_value) + L"였고 " + noMemory +
                L" 오브젝트가 이미 해제됐거나 포인터가 망가졌을 가능성이 큽니다.");
}

std::wstring DescribeFaultInstructionPlain(const FaultInstructionInfo& info, bool en)
{
  const auto& v = info.verdict;
  if (v == "null_pointer") {
    if (info.mnemonic == "call") {
      return en ? L"It crashed calling a function of an object that was already gone or never created (a null pointer)."
                : L"이미 사라졌거나 만들어진 적 없는 오브젝트의 함수를 부르다가 튕겼습니다(null 포인터).";
    }
    return en ? L"It crashed while using an object that was already gone or never created (a null pointer)."
              : L"이미 사라졌거나 만들어진 적 없는 오브젝트를 쓰다가 튕겼습니다(null 포인터).";
  }
  if (v == "bad_pointer") {
    return en ? L"It crashed while using memory that was already freed, or a broken pointer."
              : L"이미 해제된 메모리나 망가진 포인터를 쓰다가 튕겼습니다.";
  }
  if (v == "bad_index") {
    return en ? L"It crashed reading far outside a list or table (a wrong index), or through a broken pointer."
              : L"목록이나 표의 범위를 크게 벗어난 위치(잘못된 인덱스)나 망가진 포인터를 쓰다가 튕겼습니다.";
  }
  if (v == "noncanonical") {
    return en ? L"It crashed on a pointer that had been overwritten with other data (corrupted memory)."
              : L"다른 데이터로 덮어써진 포인터를 쓰다가 튕겼습니다(메모리 손상).";
  }
  if (v == "null_jump") {
    return en ? L"It crashed calling a function through an empty (null) pointer."
              : L"비어 있는(null) 함수 포인터로 함수를 부르다가 튕겼습니다.";
  }
  if (v == "bad_jump") {
    return en ? L"It crashed jumping to an address that is not code (a broken function pointer)."
              : L"코드가 아닌 주소로 이동하다가 튕겼습니다(망가진 함수 포인터).";
  }
  return {};
}

std::string FaultInstructionReportText(const FaultInstructionInfo& info)
{
  if (info.verdict.empty()) {
    return {};
  }
  std::string text;
  if (info.mnemonic.empty()) {
    text = "execute " + Hex(info.address);
    if (!info.call_instruction.empty()) {
      text += ", from " + info.call_instruction;
    }
  } else {
    text = info.mnemonic + " " + info.operand + " (" + info.bytes + "), " + info.access + " " + Hex(info.address);
    if (!info.base_register.empty()) {
      text += ", " + info.base_register + "=" + Hex(info.base_value);
    }
    if (!info.index_register.empty()) {
      text += ", " + info.index_register + "=" + Hex(info.index_value);
    }
  }
  return text + " -> " + info.verdict;
}

}  // namespace skydiag::dump_tool
