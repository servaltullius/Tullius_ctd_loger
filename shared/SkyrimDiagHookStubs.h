#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace skydiag {

// A jump stub found in executable private memory (ADR-0024): where it starts
// in the bytes, how long it is, its form, and the absolute address it jumps to.
struct HookStub
{
  std::uint32_t offset = 0;
  std::uint32_t length = 0;
  const char* kind = "";
  std::uint64_t target = 0;
};

// The jump stubs hooking code leaves in its trampoline pages:
// - "jmp_abs": FF 25 00000000 <abs64> (CommonLib REL::Trampoline, SKSE's
//   branch pool: 14 bytes)
// - "mov_rax_jmp": 48 B8 <abs64> FF E0 (mov rax, imm64; jmp rax: 12 bytes)
// - "mov_r11_jmp": 49 BB <abs64> 41 FF E3 (mov r11, imm64; jmp r11: 13 bytes)
// A stub to address 0 is unwritten space. At most maxStubs, in byte order.
inline std::vector<HookStub> DecodeHookStubs(const std::uint8_t* bytes, std::size_t size, std::size_t maxStubs = 128)
{
  std::vector<HookStub> stubs;
  if (!bytes) {
    return stubs;
  }
  const auto abs64At = [&](std::size_t at) {
    std::uint64_t value = 0;
    std::memcpy(&value, bytes + at, sizeof(value));
    return value;
  };
  std::size_t i = 0;
  while (i < size && stubs.size() < maxStubs) {
    HookStub stub{};
    if (i + 14 <= size && bytes[i] == 0xFF && bytes[i + 1] == 0x25 && bytes[i + 2] == 0 && bytes[i + 3] == 0 &&
        bytes[i + 4] == 0 && bytes[i + 5] == 0) {
      stub = { static_cast<std::uint32_t>(i), 14u, "jmp_abs", abs64At(i + 6) };
    } else if (i + 12 <= size && bytes[i] == 0x48 && bytes[i + 1] == 0xB8 && bytes[i + 10] == 0xFF &&
               bytes[i + 11] == 0xE0) {
      stub = { static_cast<std::uint32_t>(i), 12u, "mov_rax_jmp", abs64At(i + 2) };
    } else if (i + 13 <= size && bytes[i] == 0x49 && bytes[i + 1] == 0xBB && bytes[i + 10] == 0x41 &&
               bytes[i + 11] == 0xFF && bytes[i + 12] == 0xE3) {
      stub = { static_cast<std::uint32_t>(i), 13u, "mov_r11_jmp", abs64At(i + 2) };
    }
    if (stub.length == 0u) {
      ++i;
      continue;
    }
    if (stub.target != 0u) {
      stubs.push_back(stub);
    }
    i += stub.length;
  }
  return stubs;
}

}  // namespace skydiag
