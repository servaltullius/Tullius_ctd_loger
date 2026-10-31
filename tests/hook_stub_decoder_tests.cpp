// ADR-0024: the jump stubs the helper reads in trampoline pages.

#include "SkyrimDiagHookStubs.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

void PutQword(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint64_t value)
{
  std::memcpy(bytes.data() + at, &value, sizeof(value));
}

// CommonLib's REL::Trampoline and SKSE's branch pool: FF 25 00000000 <abs64>,
// one after another, the rest of the page zero.
void TestJmpAbsStubsInARow()
{
  std::vector<std::uint8_t> page(64, 0);
  const std::uint8_t jmp[] = { 0xFF, 0x25, 0, 0, 0, 0 };
  std::memcpy(page.data(), jmp, sizeof(jmp));
  PutQword(page, 6, 0x00007FFA12345678ull);
  std::memcpy(page.data() + 14, jmp, sizeof(jmp));
  PutQword(page, 20, 0x00007FFA00001000ull);

  const auto stubs = skydiag::DecodeHookStubs(page.data(), page.size());
  assert(stubs.size() == 2);
  assert(stubs[0].offset == 0 && stubs[0].length == 14 && stubs[0].target == 0x00007FFA12345678ull);
  assert(std::strcmp(stubs[0].kind, "jmp_abs") == 0);
  assert(stubs[1].offset == 14 && stubs[1].target == 0x00007FFA00001000ull);
}

// mov rax, imm64; jmp rax and mov r11, imm64; jmp r11, after other bytes.
void TestMovJmpStubs()
{
  std::vector<std::uint8_t> page(48, 0x90);
  const std::uint8_t movRax[] = { 0x48, 0xB8 };
  std::memcpy(page.data() + 3, movRax, sizeof(movRax));
  PutQword(page, 5, 0x00007FF6AAAA0000ull);
  page[13] = 0xFF;
  page[14] = 0xE0;
  const std::uint8_t movR11[] = { 0x49, 0xBB };
  std::memcpy(page.data() + 20, movR11, sizeof(movR11));
  PutQword(page, 22, 0x00007FF6BBBB0000ull);
  page[30] = 0x41;
  page[31] = 0xFF;
  page[32] = 0xE3;

  const auto stubs = skydiag::DecodeHookStubs(page.data(), page.size());
  assert(stubs.size() == 2);
  assert(stubs[0].offset == 3 && stubs[0].length == 12 && std::strcmp(stubs[0].kind, "mov_rax_jmp") == 0 &&
         stubs[0].target == 0x00007FF6AAAA0000ull);
  assert(stubs[1].offset == 20 && stubs[1].length == 13 && std::strcmp(stubs[1].kind, "mov_r11_jmp") == 0 &&
         stubs[1].target == 0x00007FF6BBBB0000ull);
}

// Unwritten space (a stub to 0), a pattern cut off at the end of what was
// read, and the cap.
void TestEdges()
{
  std::vector<std::uint8_t> page(14 + 10, 0);
  page[0] = 0xFF;
  page[1] = 0x25;  // target 0: not written yet
  page[14] = 0xFF;
  page[15] = 0x25;  // only 10 bytes left: cut off
  assert(skydiag::DecodeHookStubs(page.data(), page.size()).empty());
  assert(skydiag::DecodeHookStubs(nullptr, 100).empty());

  std::vector<std::uint8_t> many(14 * 10, 0);
  for (std::size_t i = 0; i < 10; ++i) {
    many[i * 14] = 0xFF;
    many[i * 14 + 1] = 0x25;
    PutQword(many, i * 14 + 6, 0x1000u + i);
  }
  assert(skydiag::DecodeHookStubs(many.data(), many.size(), 4).size() == 4);
}

}  // namespace

int main()
{
  TestJmpAbsStubsInARow();
  TestMovJmpStubs();
  TestEdges();
  return 0;
}
