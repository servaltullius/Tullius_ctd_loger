#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace skydiag::dump_tool {

// ADR-0016: a hook installed while another thread runs the hooked code.
//
// SKSE plugins hook a game call by writing a jump stub into the trampoline
// area next to SkyrimSE.exe and pointing the call at it. Unused trampoline
// bytes are int3 (0xCC). When a plugin installs a hook while the game is
// running, another thread can take the patched call before the stub is
// complete and execute the padding: an EXCEPTION_BREAKPOINT at an address no
// module covers. By the time the dump is written the stub is complete, so the
// dump holds a jump at the address the CPU reported as an int3, and the jump
// goes to the plugin whose hook it is (2026-10-13: zzzz_TypeMode.dll, whose
// log shows it installing a hook in the same millisecond, on the main thread).

inline constexpr std::uint32_t kExceptionBreakpoint = 0x80000003u;

// Bytes HookStubJumpTarget needs (the longest stub it decodes).
inline constexpr std::size_t kHookStubBytes = 14;

// The absolute target of a jump stub at the start of bytes, or nullopt.
// Decodes the stubs SKSE and CommonLibSSE trampolines and common hook
// libraries write:
//   FF 25 00 00 00 00 <imm64>       jmp qword ptr [rip+0]
//   48 B8 <imm64> FF E0              mov rax, imm64; jmp rax
//   49 BB <imm64> 41 FF E3           mov r11, imm64; jmp r11
inline std::optional<std::uint64_t> HookStubJumpTarget(const std::uint8_t* bytes, std::size_t size)
{
  if (!bytes) {
    return std::nullopt;
  }
  std::uint64_t target = 0;
  if (size >= 14 && bytes[0] == 0xFF && bytes[1] == 0x25 && bytes[2] == 0 && bytes[3] == 0 && bytes[4] == 0 &&
      bytes[5] == 0) {
    std::memcpy(&target, bytes + 6, sizeof(target));
  } else if (size >= 12 && bytes[0] == 0x48 && bytes[1] == 0xB8 && bytes[10] == 0xFF && bytes[11] == 0xE0) {
    std::memcpy(&target, bytes + 2, sizeof(target));
  } else if (size >= 13 && bytes[0] == 0x49 && bytes[1] == 0xBB && bytes[10] == 0x41 && bytes[11] == 0xFF &&
             bytes[12] == 0xE3) {
    std::memcpy(&target, bytes + 2, sizeof(target));
  } else {
    return std::nullopt;
  }
  if (target == 0) {
    return std::nullopt;
  }
  return target;
}

// True when the 5 bytes just before returnAddress are `call rel32` to target:
// the patched game call that entered the stub.
inline bool IsCallRel32To(const std::uint8_t* before, std::uint64_t returnAddress, std::uint64_t target)
{
  if (!before || before[0] != 0xE8) {
    return false;
  }
  std::int32_t rel = 0;
  std::memcpy(&rel, before + 1, sizeof(rel));
  return returnAddress + static_cast<std::uint64_t>(static_cast<std::int64_t>(rel)) == target;
}

}  // namespace skydiag::dump_tool
