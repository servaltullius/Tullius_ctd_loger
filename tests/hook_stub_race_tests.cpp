// A crash on the int3 padding of a hook stub that was still being written
// (HookStubRace.h, ADR-0016). The bytes below are the stub from the
// 2026-10-13 dump: FF 25 00000000 <zzzz_TypeMode.dll+0x1BFC0>, entered by
// `call rel32` at SkyrimSE.exe+0xCD8FBB.

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

#include "HookStubRace.h"

using skydiag::dump_tool::HookStubJumpTarget;
using skydiag::dump_tool::IsCallRel32To;
using skydiag::dump_tool::kHookStubBytes;

namespace {

constexpr std::uint64_t kTarget = 0x7FFA4C4EBFC0ull;

template <std::size_t N>
std::array<std::uint8_t, kHookStubBytes> Stub(const std::uint8_t (&prefix)[N], std::size_t immAt)
{
  std::array<std::uint8_t, kHookStubBytes> bytes{};
  bytes.fill(0xCC);
  std::memcpy(bytes.data(), prefix, N);
  std::memcpy(bytes.data() + immAt, &kTarget, sizeof(kTarget));
  return bytes;
}

void TestDecodesTheStubForms()
{
  const std::uint8_t ff25[] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
  auto bytes = Stub(ff25, 6);
  assert(HookStubJumpTarget(bytes.data(), bytes.size()) == kTarget);

  std::uint8_t movRax[] = { 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0 };
  bytes = Stub(movRax, 2);
  assert(HookStubJumpTarget(bytes.data(), bytes.size()) == kTarget);

  std::uint8_t movR11[] = { 0x49, 0xBB, 0, 0, 0, 0, 0, 0, 0, 0, 0x41, 0xFF, 0xE3 };
  bytes = Stub(movR11, 2);
  assert(HookStubJumpTarget(bytes.data(), bytes.size()) == kTarget);
}

void TestRejectsWhatIsNoFinishedStub()
{
  // Still padding: the stub was never written, nothing to attribute.
  std::array<std::uint8_t, kHookStubBytes> padding{};
  padding.fill(0xCC);
  assert(!HookStubJumpTarget(padding.data(), padding.size()));
  // jmp [rip+disp] with a displacement is some other code, not a stub.
  const std::uint8_t ripDisp[] = { 0xFF, 0x25, 0x10, 0x00, 0x00, 0x00 };
  auto bytes = Stub(ripDisp, 6);
  assert(!HookStubJumpTarget(bytes.data(), bytes.size()));
  // A null target, a short read and no bytes.
  const std::uint8_t ff25[] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
  bytes = Stub(ff25, 6);
  std::memset(bytes.data() + 6, 0, 8);
  assert(!HookStubJumpTarget(bytes.data(), bytes.size()));
  bytes = Stub(ff25, 6);
  assert(!HookStubJumpTarget(bytes.data(), 13));
  assert(!HookStubJumpTarget(nullptr, kHookStubBytes));
}

void TestCallRel32ReachesTheStub()
{
  // SkyrimSE.exe+0xCD8FBB: E8 rel32, return address +0xCD8FC0.
  const std::uint64_t ret = 0x7FF6AB538FC0ull;
  const std::uint64_t stub = 0x7FF6AA84583Dull;
  const auto rel = static_cast<std::int32_t>(static_cast<std::int64_t>(stub) - static_cast<std::int64_t>(ret));
  std::uint8_t call[5] = { 0xE8 };
  std::memcpy(call + 1, &rel, sizeof(rel));
  assert(IsCallRel32To(call, ret, stub));
  assert(!IsCallRel32To(call, ret, stub + 1));
  call[0] = 0xE9;  // jmp rel32 pushes no return address
  assert(!IsCallRel32To(call, ret, stub));
  assert(!IsCallRel32To(nullptr, ret, stub));
}

}  // namespace

int main()
{
  TestDecodesTheStubForms();
  TestRejectsWhatIsNoFinishedStub();
  TestCallRel32ReachesTheStub();
  return 0;
}
