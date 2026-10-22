#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace skydiag::dump_tool {

// x64 CONTEXT layout (winnt.h), for reading a thread context straight from a
// dump's bytes: MxCsr, and FltSave (XMM_SAVE_AREA32 at 0x100) XmmRegisters at
// FltSave + 0xA0, 16 bytes each.
inline constexpr std::size_t kContextMxCsrOffset = 0x34;
inline constexpr std::size_t kContextXmmRegistersOffset = 0x1A0;
inline constexpr std::size_t kContextXmmRegisterCount = 16;

// Code bytes read on each side of the instruction pointer.
inline constexpr std::size_t kSpinLoopWindowBefore = 32;
inline constexpr std::size_t kSpinLoopWindowAfter = 32;
// Longest loop body a backward jump may close.
inline constexpr std::size_t kSpinLoopMaxBytes = 64;

// A floating-point compare (comiss/ucomiss/comisd/ucomisd) directly followed
// by a backward conditional jump that is taken when the compare is unordered
// (jb, jbe, je or jp: an operand that is NaN sets CF, ZF and PF), with the
// instruction pointer inside the loop the jump closes. Such a loop, for
// example "while (x < 0) x += 2*pi" (addss; comiss; jb), never ends once x is
// NaN.
struct FloatCompareLoop
{
  bool found = false;
  bool is_double = false;         // comisd / ucomisd
  bool unordered_compare = false; // ucomiss / ucomisd
  std::uint8_t reg = 0;           // ModRM.reg operand, XMM 0..15
  bool has_rm_reg = false;        // the second operand is a register too
  std::uint8_t rm_reg = 0;
  std::size_t compare_offset = 0; // in the code window
};

// code: bytes around the instruction pointer, which is at code[ripOffset].
FloatCompareLoop FindFloatCompareLoop(const std::vector<std::uint8_t>& code, std::size_t ripOffset);

// "comiss" / "ucomiss" / "comisd" / "ucomisd".
std::string FloatCompareMnemonic(const FloatCompareLoop& loop);

// The low lane of XMM register `reg` (the 4 or 8 bytes a scalar compare
// reads) from a thread context of `size` bytes; nullopt when the context is
// too short to hold it or reg is out of range.
std::optional<std::uint64_t> ReadContextXmmLowLane(
  const std::uint8_t* context,
  std::size_t size,
  unsigned reg,
  bool isDouble);

// MxCsr from a thread context; nullopt when the context is too short.
std::optional<std::uint32_t> ReadContextMxCsr(const std::uint8_t* context, std::size_t size);

// The IEEE 754 bits are a NaN (all exponent bits set, a non-zero fraction).
bool IsNaNBits(std::uint64_t bits, bool isDouble);

// The compared register of the loop that holds NaN (the ModRM.reg operand
// first), or nullopt when neither does or the context lacks the registers.
std::optional<std::uint8_t> FindNaNCompareRegister(
  const FloatCompareLoop& loop,
  const std::uint8_t* context,
  std::size_t size,
  std::uint64_t* outBits = nullptr);

}  // namespace skydiag::dump_tool
